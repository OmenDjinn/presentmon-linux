// SPDX-License-Identifier: MIT
// VRR readout, shared by the in-game overlay, the GUI's live view (with the driver's VRR state from DRM) and the
// GUI's capture analysis (a whole CSV), so all three judge the same frames the same way.
//
// Modelled on a monitor's refresh-rate OSD: the rate the panel refreshes at now, how far it swings, and one word
// for what kind of swing it is. Under VRR the panel refreshes when a frame arrives, so 1000 / (time since the
// previous screen update) is the panel's refresh rate for that frame. Below the panel's VRR floor the driver
// repeats frames (LFC) and the panel runs at a multiple of that; the readout shows the rate frames arrive at.
//
// Per displayed frame it looks at MsBetweenDisplayChange (the screen interval) and MsBetweenPresents (the game's).
//
// Numbers:
//   hz        the refresh rate now: screen updates over the last nowMs (250) of screen time. A monitor's OSD
//             averages a few frames too. With VRR off, the output's fixed rate.
//   low/high  5th / 95th percentile of the per-frame rates over the judged frames; swing = (high - low) / 2,
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
//   stutter  a frame stayed on screen more than 2.5x the median
//   LFC      the screen interval crossed the panel's lowest VRR rate (needs vrrMinHz): a big refresh-rate jump
//            each time, a classic cause of VRR flicker
// Verdict, first that applies:
//   fixed     VRR is off (state below)
//   unstable  >= 15 % uneven pairs           (Lies of P with the other screen's 15 fps wallpaper: 21 %)
//   stutter   >= 0.5 stutters per second     (one in the live 2 s)
//   LFC       >= 0.5 floor crossings per second
//   jittery   >= 5 % uneven pairs            (a steady test app with that wallpaper: 10 %)
//   at max    the game runs at the refresh cap (state idle): VRR has nothing to vary
//   varying   the 250 ms rate moved by >= 15 % of the median within the judged frames: VRR follows a frame rate
//             that changes, smoothly (not a fault; a cap holds it still)
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
};

enum class DriverVrr : uint8_t
{
	Unknown,
	On,
	Off,
};

struct VrrContext
{
	double refreshHz = 0;   // the output's current mode refresh (the VRR maximum); 0 = unknown
	double vrrMinHz = 0;    // the panel's lowest VRR rate (EDID range); 0 = unknown
	DriverVrr driver = DriverVrr::Unknown;   // the CRTC's VRR_ENABLED, when the caller can read it
};

struct VrrOptions
{
	double recentMs = 0;   // judge the last recentMs of screen time (at least kMinFrames frames); 0 = every frame
	double nowMs = 250;    // `hz` = the rate over the last nowMs of screen time
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
	Lfc,       // crossing the VRR floor
	Unstable,
};

enum class VrrCause : uint8_t
{
	None,      // not jittery or unstable
	Display,   // uneven on screen while the game paced evenly
	Game,      // the game's own frame times swing
};

struct VrrReadout
{
	static constexpr double kNA = std::numeric_limits<double>::quiet_NaN();
	static constexpr size_t kMinFrames = 30;

	VrrState state = VrrState::NoData;
	VrrStability stability = VrrStability::None;
	VrrCause cause = VrrCause::None;   // where the unevenness comes from, when jittery or unstable
	double hz = kNA;                   // now (the fixed rate when VRR is off)
	double swingHz = kNA;              // ± around it: (highHz - lowHz) / 2; 0 when VRR is off
	double lowHz = kNA;                // 5th percentile of the per-frame rates, judged frames
	double highHz = kNA;               // 95th percentile
	double medianHz = kNA;             // median per-frame rate, judged frames
	double driftPct = kNA;             // spread of the 250 ms rates within the judged frames, percent of the median
	size_t frames = 0;                 // displayed frames given
	size_t judgedFrames = 0;
	double judgedSeconds = 0;          // screen time they cover
	double unevenPct = kNA;            // judged pairs with a spacing jump, percent
	double unevenDisplayPct = kNA;     // ... of them with the game steady
	size_t stutters = 0;
	double lfcCrossings = kNA;         // count; NA without vrrMinHz
	double belowRangePct = kNA;        // judged spacings longer than 1/vrrMinHz, percent; NA without vrrMinHz
	double onGridPct = kNA;            // spacings on multiples of the refresh period, percent (all frames); NA without one
	double refreshHz = kNA;            // the period used: the output's, or the common rate the spacings snapped to
};

// Frames in order, oldest first (dropped ones included: they are skipped). O(n log n).
VrrReadout analyzeVrr(std::span<const VrrFrame> frames, const VrrContext &context = {}, const VrrOptions &options = {});

// Short English labels for UIs.
const char *vrrStabilityLabel(VrrStability stability);   // "stable", "jittery", ...; "" for None
const char *vrrStateLabel(VrrState state);               // "no data", "unknown", "off", "idle", "active"
const char *vrrCauseLabel(VrrCause cause);               // e.g. "uneven on screen, game steady"; "" for None
// 0 fine (stable, varying, at max, fixed), 1 worth a look (jittery, stutter, LFC), 2 bad (unstable), -1 none.
int vrrStabilitySeverity(VrrStability stability);

} // namespace pml::metrics
