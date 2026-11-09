// SPDX-License-Identifier: MIT
// Experience score: one 0-100 number for how a game plays, from what reached the screen. Shared by the overlay
// (last 10 s), the GUI's Live page and its capture analysis (a whole CSV). Design, sources and validation:
// docs/ARCHITECTURE.md 3.4.3.
//
//   score = 100 - rate - motion - stutter - latency     (additive impairments as in ITU-T G.1072), clipped 0..100
//
//   rate     motion rate from the mean time between screen updates t: min(60, d^2 / (d + 3)), d = max(0, t - 4 ms).
//            Linear in frame time, flattening above ~75-90 fps (Tokey et al. 2025): 120 fps 2.6, 90 fps 5.0,
//            60 fps 10.2, 30 fps 26. Uses displayed frames: frames the screen never showed do not count.
//   motion   judder and jitter: RMS of PresentMon's animation error (screen-time step vs the game's own step)
//            over frames that are not hitches, 1 ms dead zone (~2 ms swings unnoticed, Klein et al. 2024):
//            min(30, 3 * max(0, rms - 1)).
//   stutter  hitches: per frame the visible jump = max(screen interval beyond the median, |animation error|); a hitch
//            exceeds max(2 x median, median + 8 ms) + 1 ms (one missed refresh is judder, not a hitch).
//            0.1 x interruption (hitch ms per second; Xu & Claypool 2024) + 25 x clamp((worst - 50 ms) / 100 ms,
//            0, 1.75) (spikes < 50 ms acceptable, >= 150 ms hurt: Tokey et al. 2026); at most 50.
//   latency  frame start to first pixel (PresentMon DisplayLatency, median), ITU-T G.1072 input-delay impairment for
//            high-sensitivity games, weight 0.848, relative to 0 ms: 20 ms 3, 40 ms 6, 100 ms 16. It excludes the
//            mouse and OS input path, so it reads lower than click-to-photon studies.
//
// The weights between the terms are a design choice: no study measures smoothness against latency.
// Presented like the VRR verdict (pml/metrics/vrr.hpp): 0-100, the same bands (>= 85 smooth, >= 65 minor issues,
// >= 40 rough, below: very rough) and the largest impairment as the cause when the score is below 85; frame rate is
// named only below 58 fps shown (60 fps is not a defect).
// Pacing score: 100 - 3 x (motion + stutter), the consistency part alone: the same for a steady 60 fps and a steady
// 240 fps. Inside the overall score judder and stutters are weighed against frame rate and latency; shown on their
// own they need 3x the scale for the same bands (a design choice, calibrated on recordings: a 12/21 ms alternation
// that players call juddery scores 67, the same game paced evenly 89-95, a 100 ms freeze every 4 s 53).
#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <string>

namespace pml::metrics {

struct ExperienceFrame
{
	float displayIntervalMs = 0;   // time since the previous screen update (MsBetweenDisplayChange); <= 0 or NaN: skip
	float animationErrorMs = std::numeric_limits<float>::quiet_NaN();  // MsAnimationError; NaN = unknown
	float latencyMs = std::numeric_limits<float>::quiet_NaN();         // DisplayLatency; NaN or <= 0 = unknown
};

enum class ExperienceCause : uint8_t
{
	None,      // score >= 85
	Rate,      // low displayed frame rate
	Motion,    // judder / uneven frame times
	Stutter,   // hitches
	Latency,   // input-to-screen delay
};

struct ExperienceVerdict
{
	static constexpr double kNA = std::numeric_limits<double>::quiet_NaN();
	static constexpr size_t kMinFrames = 50;

	int score = -1;                // 0-100, -1 = none (fewer than kMinFrames displayed frames)
	int pacingScore = -1;          // 0-100: judder and stutters only (frame rate and latency left out), same bands; -1 = none
	ExperienceCause cause = ExperienceCause::None;   // the largest impairment when the score is below 85
	size_t frames = 0;             // displayed frames used
	double displayFps = kNA;       // screen updates per second
	double rate = 0, motion = 0, stutter = 0, latency = 0;   // the impairments (points taken off)
	double animationRmsMs = kNA;   // RMS animation error of the non-hitch frames (NA without animation error)
	double interruptMsPerS = 0;    // hitch time per second
	double worstHitchMs = 0;       // longest hitch, as a screen interval (0 = none)
	uint32_t hitches = 0;
	double latencyMs = kNA;        // median display latency (NA when unknown: no latency impairment)
};

// Frames in order (frames without a screen interval are skipped). O(n).
ExperienceVerdict analyzeExperience(std::span<const ExperienceFrame> frames);

// Short English labels for UIs.
const char *experienceVerdictLabel(int score);              // "smooth", "minor issues", "rough", "very rough", "" for -1
const char *experienceCauseLabel(ExperienceCause cause);    // e.g. "stutters"; "" for None
// Below this displayed frame rate, a low frame rate can be named as the cause.
inline constexpr double kExperienceLowFps = 58.0;

// The numbers behind the score, points taken off per term: "120 fps shown: rate -2.6 · judder -0.0 (1.9 ms RMS) ·
// stutters -14.5 (1 hitch, worst 108 ms) · latency -2.8 (16 ms) · 900 frames"; "" for score -1.
std::string experienceDetail(const ExperienceVerdict &verdict);

} // namespace pml::metrics
