// SPDX-License-Identifier: MIT
// VRR stability verdict, shared by the in-game overlay (rolling window), the GUI's live view (with the
// driver's VRR state from DRM) and the GUI's capture analysis (a whole CSV), so all three judge the same
// frames the same way.
//
// What it looks at, per displayed frame: the time since the previous screen update (MsBetweenDisplayChange)
// and the game's own frame interval (MsBetweenPresents). Under VRR the panel refreshes when a frame arrives,
// so every jump in that spacing is a jump in refresh rate. Rapid refresh-rate swings are what makes VRR
// flicker on OLED and VA panels (gamma/brightness depends on the refresh rate) and what reads as uneven
// motion everywhere.
//
// Events, counted per pair of consecutive displayed frames:
//   uneven   the screen-update spacing changed by more than max(2 ms, 25 % of its median). Split by cause:
//            "display" when the game's frame interval stayed steady (within half that threshold), i.e.
//            the unevenness was added after the game (compositor, driver), else "game".
//   stutter  a frame stayed on screen more than 2.5x the median (weighs 3)
//   LFC      the spacing crossed the panel's lowest VRR rate (needs vrrMinHz; weighs 3): below it the driver
//            repeats frames (low framerate compensation), and going in and out of it is a classic flicker
// score = round(100 * exp(-weightedEvents / pairs / 0.25)): 0 events 100, 2 % ~92, 5 % ~82, 10 % ~67,
// 20 % ~45, 35 % ~25. Verdict: >= 85 stable, >= 65 minor jitter, >= 40 unstable, else very unstable.
// There is no published standard for this; the thresholds are this project's. In Lies of P (vkd3d-proton,
// 165 fps, VRR on, 2026-11-09) the game's own frame interval jumped by more than 1.2 ms between frames 13 % of
// the time, which nobody sees; the 2 ms floor keeps that out. Its screen spacing jumped by more than 2 ms 21 %
// of the time against 4 % for the game (p95 4.6 ms against 1.8 ms): score ~43, unstable, cause display.
//
// State, before the score:
//   off      the driver says VRR is off, or the screen updates snap to a refresh grid (>= 80 % of the spacings on
//            multiples of the period) while the game's own intervals do not (< 50 %): fixed refresh, so a score
//            says nothing about VRR. The period is the output's when known, else the lowest common rate
//            (48 ... 500 Hz) that shows this snap.
//   idle     the game runs at the refresh cap (its median interval within 3 % of the period or shorter): VRR has nothing to
//            vary (or the refresh is fixed: the same to the eye); the score still describes the pacing
//   active   the driver says VRR is on, or the period is known and the screen follows a game that is off its grid
//   unknown  cannot be told (the game itself runs on the grid, e.g. FIFO at the refresh rate or a 120 fps cap on
//            240 Hz, and no driver state): the score describes the pacing as if VRR were on
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

enum class VrrState : uint8_t
{
	NoData,   // fewer than kMinFrames displayed frames
	Unknown,
	Off,
	Idle,
	Active,
};

enum class VrrCause : uint8_t
{
	None,       // stable: nothing stands out
	Display,    // uneven on screen while the game paced evenly
	Game,       // the game's own frame times swing
	Stutter,
	Lfc,
};

struct VrrVerdict
{
	static constexpr double kNA = std::numeric_limits<double>::quiet_NaN();
	static constexpr size_t kMinFrames = 30;

	VrrState state = VrrState::NoData;
	int score = -1;                   // 0-100, -1 = none (no data, VRR off)
	VrrCause cause = VrrCause::None;  // the largest contributor when the score is below 85
	size_t frames = 0;                // displayed frames used
	double medianDisplayMs = kNA;
	double unevenPct = kNA;           // pairs with a spacing jump, percent
	double unevenDisplayPct = kNA;    // ... of them with the game steady
	double stutterPct = kNA;
	double lfcCrossings = kNA;        // count; NA without vrrMinHz
	double belowRangePct = kNA;       // spacings longer than 1/vrrMinHz, percent; NA without vrrMinHz
	double onGridPct = kNA;           // spacings on multiples of the refresh period, percent; NA without one
	double refreshHz = kNA;           // the period used: the output's, or the common rate the spacings snapped to
};

// Frames in order (dropped ones included: they are skipped). O(n log n).
VrrVerdict analyzeVrr(std::span<const VrrFrame> frames, const VrrContext &context = {});

// Short English labels for UIs.
const char *vrrVerdictLabel(int score);          // "stable", "minor jitter", "unstable", "very unstable", "" for -1
const char *vrrStateLabel(VrrState state);       // "no data", "unknown", "off", "idle", "active"
const char *vrrCauseLabel(VrrCause cause);       // e.g. "uneven on screen, game steady"; "" for None

} // namespace pml::metrics
