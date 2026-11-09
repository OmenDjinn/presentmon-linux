// SPDX-License-Identifier: MIT
// Frame-time summary shared by the in-game overlay (rolling window) and the GUI's capture
// analysis (a whole CSV), so both report the same average, percentiles and lows for the same
// frames.
#pragma once

#include <cstddef>
#include <limits>
#include <span>

namespace pml::metrics {

struct FrameTimeSummary
{
	static constexpr double kNA = std::numeric_limits<double>::quiet_NaN();

	size_t count = 0;       // frame times used (values > 0)
	double avgMs = kNA;     // mean frame time
	double minMs = kNA;
	double maxMs = kNA;
	// Nearest-rank percentiles: the value at index ceil(p * count) - 1 of the ascending times.
	double p50Ms = kNA;
	double p95Ms = kNA;
	double p99Ms = kNA;
	// Lows: 1000 / mean of the slowest ceil(1 % of count) frames (at least one frame); 0.1 % the same.
	double low1Fps = kNA;
	double low01Fps = kNA;
	// Gaming Quality Index (Blackbird PC Tech, 2025): (1% low / 60 fps) * (1 - (avg - 1% low) / avg)
	// = 1% low^2 / (60 * avg fps). Its 1% low is time-weighted: frame times sorted slowest first and summed until
	// they reach 1 % of the total time; the fps of the frame that gets there (as in PresentMon's and Afterburner's
	// "1% low"). >= 1: 1% lows at or above 60 fps with little spread; the 60 fps reference is fixed, so it keeps
	// growing with frame rate (no upper bound).
	double low1TimeFps = kNA;
	double gqi = kNA;
};

// Frame times in milliseconds (e.g. MsBetweenPresents). Values that are not > 0 (0, negative, NaN)
// are ignored. O(n log n).
FrameTimeSummary summarizeFrameTimes(std::span<const float> frameTimesMs);

} // namespace pml::metrics
