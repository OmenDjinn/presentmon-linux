// SPDX-License-Identifier: MIT
// Reads a PresentMon-style CSV (ours, Intel PresentMon 1.x/2.x, v1 and v2 column sets) into
// per-frame arrays, and summarizes it with the same definitions as the in-game overlay
// (pml/metrics/summary.hpp). Plain C++, no Qt: tested on its own.
#pragma once

#include <pml/metrics/summary.hpp>

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace pml::gui {

struct CaptureData
{
	std::string application;   // Application of the first row
	std::string runtime;       // most common PresentRuntime ("" if the column is missing)
	std::string presentMode;   // most common PresentMode
	std::string frameColumn;   // the column frameMs came from (e.g. "MsBetweenPresents", "FrameTime")
	std::string timeColumn;    // the column timeSec came from ("" = cumulative frame times)
	size_t processCount = 0;   // distinct ProcessID values
	size_t swapchainCount = 0; // distinct (ProcessID, SwapChainAddress) pairs

	// One entry per CSV row (frame), in file order.
	std::vector<double> timeSec;   // seconds since the first frame
	std::vector<float> frameMs;    // frame time; NaN where the row has none (NA)
	std::vector<float> displayMs;  // time between display changes (MsBetweenDisplayChange, or the
	                               // v2 DisplayedTime of the previous frame); NaN = not displayed / NA
	std::vector<float> animationErrorMs;  // MsAnimationError / v2 AnimationError; NaN = NA or no column
	std::vector<float> latencyMs;  // display latency: v2 DisplayLatency, else MsUntilDisplayed + MsCPUBusy; NaN = NA
	bool hasDisplay = false;       // a display column exists and has values
	size_t droppedFrames = 0;      // rows whose display column is NA or 0 while the column has values
	std::vector<std::string> warnings;  // e.g. several processes in one file
};

// False + error for unreadable files or files without a usable frame-time column. `cancel` (optional)
// stops the read early with false and the error "cancelled" once it becomes true; it is checked every
// few thousand rows (the worker of a CaptureAnalysis that was re-pathed or destroyed).
bool readCaptureCsv(const std::string &path, CaptureData *out, std::string *error,
                    const std::atomic<bool> *cancel = nullptr);
// The same for CSV text in memory (tests).
bool parseCaptureCsv(const std::string &text, CaptureData *out, std::string *error,
                     const std::atomic<bool> *cancel = nullptr);

struct CaptureStats
{
	size_t frames = 0;
	double durationSec = 0;    // last time - first time (or the sum of frame times)
	double avgFps = 0;         // 1000 / avgMs (as PresentMon's average fps)
	pml::metrics::FrameTimeSummary times;   // avg, min, max, p50, p95, p99, 1 % and 0.1 % lows
	double displayFps = 0;     // displayed frames / duration, 0 without display data
	double droppedPct = 0;     // dropped / frames, 0 without display data
};

CaptureStats computeCaptureStats(const CaptureData &data);

// Share of frames (percent) per bin of binMs, from 0 to maxMs; the last bin also takes everything
// above maxMs. At most 1000 bins, whatever the arguments (hostile files).
std::vector<double> frameTimeHistogram(const std::vector<float> &frameMs, double binMs, double maxMs);

} // namespace pml::gui
