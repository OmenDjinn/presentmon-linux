// SPDX-License-Identifier: MIT
// VRR readout, shared by the in-game overlay, the GUI's live view (with the driver's VRR state from DRM) and the
// GUI's capture analysis (a whole CSV), so all three judge the same frames the same way.
//
// Modelled on a monitor's refresh-rate OSD: the rate the panel refreshes at now, how far it swings, and one word
// for what kind of swing it is. A monitor's counter measures the refresh cycles it receives, not the game's frames,
// so the readout counts panel refreshes:
//   * Inside the VRR range the panel refreshes when a frame arrives: one refresh per displayed frame, and
//     1000 / (time since the previous refresh) is its rate for that frame.
//   * A scan-out takes 1 / (the mode's rate), so the panel cannot refresh faster than that. A frame that comes
//     while one is running waits for its end (FIFO, MAILBOX: an ordinary page flip) or tears into it and starts
//     no refresh of its own (IMMEDIATE: KWin flips those asynchronously under VRR).
//   * It cannot wait longer than 1 / (lowest VRR rate). What the source does then is VrrFloor: with Rescan
//     (NVIDIA) the display engine scans the previous frame out again 1 / vrrMinHz after the last refresh started,
//     and again every 1 / vrrMinHz while nothing comes. With Unknown the readout counts no such refreshes
//     (amdgpu multiplies frames instead; untested).
//   So the readout walks the displayed frames and keeps the time since the panel's last refresh started. Every
//   1 / vrrMinHz of it is a re-scan (Rescan). A frame that cannot tear then starts a refresh (it waited if it had
//   to: such a frame comes 0.92-0.99 of a scan-out after the re-scan started, the active lines of the mode; one
//   that seems to come less than half a scan-out after a re-scan came just before it, timestamp noise). A frame
//   that may tear starts one only if at least kScanShare (0.9) of a scan-out has passed since the last refresh
//   started, else it tore in and the time runs on.
//   Measured 2026-11-13 on NVIDIA 620.70 / KWin 6.8.2, 48 Hz floor (docs/FINDINGS.md 7):
//     30 fps:              frames every 33.3 ms; the panel refreshes after 20.8 and 12.5 ms (48 / 80 Hz): 60 Hz.
//     45 fps, MAILBOX:     frames alternate 19.4 / 25.0 ms on a 240 Hz mode, 17.7 / 26.9 ms on a 165 Hz mode
//                          (floor + one scan-out, then the rest); three refreshes per two frames: 67.5 Hz.
//     45 fps, IMMEDIATE:   frames every 22.2 ms, tearing into the re-scans; four refreshes per three frames: 60 Hz.
//     400 fps, IMMEDIATE:  screen times 2.5 ms apart on a 240 Hz mode; every second one starts a scan: 200 Hz.
//   * When frames stop (sinceLastMs, live only) the panel keeps re-scanning at its floor: the readout says so
//     instead of holding the last rate.
// Refreshes caused by something else than the game's frames (the compositor's cursor-only commits while the game
// runs under ~50 fps, another window on top) are invisible to it. What the panel does is inferred from the
// frames' screen times and the driver's source, not read from the monitor.
//
// Per displayed frame it looks at MsBetweenDisplayChange (the screen interval) and MsBetweenPresents (the game's).
//
// Numbers:
//   hz        the refresh rate now: panel refreshes over the last nowMs (250) of screen time. A monitor's OSD
//             averages a few refreshes too. With VRR off, the output's fixed rate.
//   low/high  5th / 95th percentile of the per-refresh rates over the judged frames; swing = (high - low) / 2,
//             shown as "±swing". Hz is not linear in time: at 165 Hz a 2 ms step is 165 -> 124 Hz, at 60 Hz
//             60 -> 54 Hz. That is why the verdict below judges milliseconds, not Hz.
//   judged    the last recentMs of screen time (2 s for the live readout, at least kMinFrames frames), or every
//             frame (recentMs 0, a capture).
//
// Events, per pair of consecutive displayed frames (the thresholds of the earlier 0-100 score, unchanged):
//   uneven   the screen interval changed by more than max(2 ms, 25 % of its median). "display" when the game's
//            own interval stayed within half that (the unevenness was added after the game: compositor, driver,
//            another screen's animation), else "game". The 2 ms floor keeps out the game's normal variation: in
//            Lies of P (vkd3d-proton, 165 fps, VRR on, 2026-11-09) the game's interval jumped by more than 1.2 ms
//            13 % of the time, which nobody sees.
//   stutter  a frame stayed on screen more than 2.5x the median; with VrrFloor::Rescan also a frame the panel
//            refreshed without, in a game whose median interval is inside the range (<= 0.75 / vrrMinHz): a
//            hitch, not a frame rate under the floor
//   below    the frame stayed longer than the panel's lowest VRR rate allows (needs vrrMinHz): with VrrFloor::Rescan
//            the panel refreshed without it (above); otherwise its interval is just over 1 / vrrMinHz. Each is a
//            big refresh-rate jump, a classic cause of VRR flicker.
// Verdict, first that applies:
//   fixed         VRR is off (state below)
//   no new frames nothing reached the screen for 750 ms and four frame intervals (live only)
//   below range   >= 15 % of the frames were "below" (Rescan, median not inside the range): the game runs under
//                 the VRR range
//   unstable      >= 15 % uneven pairs           (Lies of P with the other screen's 15 fps wallpaper: 21 %)
//   stutter       >= 0.5 stutters per second     (one in the live 2 s)
//   below range   >= 0.5 per second of: "below" frames (Rescan, median not inside the range), else crossings of
//                 the floor
//   jittery       >= 5 % uneven pairs            (a steady test app with that wallpaper: 10 %)
//   at max        the game runs at the refresh cap (state idle): VRR has nothing to vary
//   varying       the 250 ms rate moved by >= 15 % of the median within the judged frames: VRR follows a frame rate
//                 that changes, smoothly (not a fault; a cap holds it still)
//   stable
// There is no published standard for VRR stability; these thresholds are this project's. 5 % and 15 % match
// where the earlier score said "minor jitter" (< 85) and "unstable" (< 65), calibrated on the recordings above.
//
// State, from all frames given (the overlay's last 10 s):
//   off      the driver says VRR is off, or the screen updates snap to a refresh grid (>= 80 % of the spacings on
//            multiples of the period) while the game's own intervals do not (< 50 %): fixed refresh. The period is
//            the output's when known, else the lowest common rate (48 ... 500 Hz) that shows this snap.
//   idle     the game runs at the refresh cap (its median interval within 3 % of the period or shorter)
//   active   the driver says VRR is on, or the period is known and the screen follows a game that is off its grid
//   unknown  cannot be told (the game itself runs on the grid, e.g. FIFO at the refresh rate or a 120 fps cap on
//            240 Hz, and no driver state): judged as if VRR were on
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace pml::metrics {

struct VrrFrame
{
	float presentMs = 0;   // the game's frame interval (MsBetweenPresents); <= 0 or NaN = unknown
	float displayMs = 0;   // time since the previous screen update; <= 0 or NaN = not displayed
	bool tears = false;    // presented in a mode that may tear (IMMEDIATE): during a scan-out it starts no refresh
};

enum class DriverVrr : uint8_t
{
	Unknown,
	On,
	Off,
};

// What the source does when no new frame arrives within 1 / vrrMinHz (the panel cannot wait longer).
enum class VrrFloor : uint8_t
{
	Unknown,   // not known for this driver: below the floor the readout counts frames only
	Rescan,    // the display engine scans the last frame out again by itself (NVIDIA, see the top of this file)
};

// About the output the game is on (not just any output with VRR on).
struct VrrContext
{
	double refreshHz = 0;   // its current mode refresh (the VRR maximum); 0 = unknown
	double vrrMinHz = 0;    // the panel's lowest VRR rate (EDID range); 0 = unknown
	DriverVrr driver = DriverVrr::Unknown;   // its CRTC's VRR_ENABLED, when the caller can read it
	VrrFloor floor = VrrFloor::Unknown;
};

struct VrrOptions
{
	double recentMs = 0;   // judge the last recentMs of screen time (at least kMinFrames frames); 0 = every frame
	double nowMs = 250;    // `hz` = the rate over the last nowMs of screen time
	// Live readouts: how long ago (the caller's clock) the newest frame given reached the screen. Past kStaleMs
	// and kStaleIntervals of its frame intervals the verdict is "no new frames". 0 = not live (a capture).
	double sinceLastMs = 0;
};

// The live readout's windows (overlay and GUI live page).
inline constexpr VrrOptions kVrrLive{2000, 250};

enum class VrrState : uint8_t
{
	NoData,   // fewer than kMinFrames displayed frames
	Unknown,
	Off,
	Idle,
	Active,
};

enum class VrrStability : uint8_t
{
	None,      // no data
	Fixed,     // VRR off: the screen refreshes at a fixed rate
	Stable,
	Varying,   // the rate follows a changing frame rate, smoothly
	AtMax,     // the game runs at the refresh cap
	Jittery,
	Stutter,
	BelowRange,   // frames slower than the panel's lowest VRR rate: it refreshes without them
	Unstable,
	NoFrames,     // live: nothing new reached the screen for a while
};

enum class VrrCause : uint8_t
{
	None,      // not jittery or unstable
	Display,   // uneven on screen while the game paced evenly
	Game,      // the game's own frame times swing
	Floor,     // below range: the frame rate is under the panel's lowest VRR rate
};

struct VrrReadout
{
	static constexpr double kNA = std::numeric_limits<double>::quiet_NaN();
	static constexpr size_t kMinFrames = 30;
	static constexpr double kStaleMs = 750;        // no new frame on screen for this long ...
	static constexpr double kStaleIntervals = 4;   // ... and this many of the last frame intervals: "no new frames"

	VrrState state = VrrState::NoData;
	VrrStability stability = VrrStability::None;
	VrrCause cause = VrrCause::None;   // where the unevenness comes from, when jittery or unstable
	double hz = kNA;                   // panel refreshes per second now (the fixed rate when VRR is off; the floor
	                                   // with "no new frames" and VrrFloor::Rescan, else NA then)
	double swingHz = kNA;              // ± around it: (highHz - lowHz) / 2; 0 when VRR is off
	double lowHz = kNA;                // 5th percentile of the per-refresh rates, judged frames
	double highHz = kNA;               // 95th percentile
	double medianHz = kNA;             // median per-refresh rate, judged frames
	double meanHz = kNA;               // panel refreshes per second over the judged frames
	double driftPct = kNA;             // spread of the 250 ms rates within the judged frames, percent of the median
	size_t frames = 0;                 // displayed frames given
	size_t judgedFrames = 0;
	double judgedSeconds = 0;          // screen time they cover
	double unevenPct = kNA;            // judged pairs with a spacing jump, percent
	double unevenDisplayPct = kNA;     // ... of them with the game steady
	size_t stutters = 0;
	double lfcCrossings = kNA;         // times the frame rate crossed the floor (either way); NA without vrrMinHz
	double belowRangePct = kNA;        // judged frames below the floor (see "below"), percent; NA without vrrMinHz
	double rescans = kNA;              // refreshes the panel made without a new frame (VrrFloor::Rescan), judged
	                                   // frames; NA when not modelled
	double onGridPct = kNA;            // spacings on multiples of the refresh period, percent (all frames); NA without one
	double refreshHz = kNA;            // the period used: the output's, or the common rate the spacings snapped to
};

// Frames in order, oldest first (dropped ones included: they are skipped). O(n log n).
VrrReadout analyzeVrr(std::span<const VrrFrame> frames, const VrrContext &context = {}, const VrrOptions &options = {});

// Short English labels for UIs.
const char *vrrStabilityLabel(VrrStability stability);   // "stable", "jittery", ...; "" for None
const char *vrrStateLabel(VrrState state);               // "no data", "unknown", "off", "idle", "active"
const char *vrrCauseLabel(VrrCause cause);               // e.g. "uneven on screen, game steady"; "" for None
// 0 fine (stable, varying, at max, fixed, no new frames), 1 worth a look (jittery, stutter, below range),
// 2 bad (unstable), -1 none.
int vrrStabilitySeverity(VrrStability stability);

} // namespace pml::metrics
