// SPDX-License-Identifier: MIT
// Rolling statistics for the overlay. The numbers come from the same pml_metrics engine as the
// CSV: every finished FrameRecord of a swapchain goes through pml::metrics::SwapchainEngine
// (PresentMon's UnifiedSwapChain), and the rows it returns are kept in rolling windows.
//
// Threading: push() may be called from any thread (it is the layer's onRecord callback and runs
// with the broker lock held, so it only appends to a queue). Everything else runs on the
// presenting thread.
#pragma once

#include <pml/metrics/engine.hpp>
#include <pml/protocol.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace pml::layer::overlay {

inline constexpr double kNA = std::numeric_limits<double>::quiet_NaN();
inline bool isNA(double v) { return v != v; }

inline constexpr int kGraphPoints = 240;               // columns of the frame time graph
inline constexpr uint64_t kGraphBucketNs = 12'500'000; // 240 x 12.5 ms = 3 s of history
inline constexpr uint64_t kShortWindowNs = 1'000'000'000;    // fps, CPU, latency
inline constexpr uint64_t kLongWindowNs = 10'000'000'000;    // frame time percentiles, dropped %
inline constexpr size_t kMaxSamples = 8192;            // cap for the long window (very high fps)
inline constexpr uint64_t kStaleNs = 1'500'000'000;    // no new frame for this long -> idle

struct Snapshot
{
	bool valid = false;        // enough samples for the numbers below
	bool idle = false;         // the application stopped presenting
	bool hasDisplayInfo = false;  // the swapchain delivers display (screen) times
	double presentFps = kNA;   // presents per second over the last second
	double displayFps = kNA;   // distinct screen updates per second (NA without display info)
	double avgMs = kNA;        // mean MsBetweenPresents, long window
	double p99Ms = kNA;        // 99th percentile MsBetweenPresents
	double low1Fps = kNA;      // fps of the mean of the slowest 1 % of frames
	double low01Fps = kNA;     // same for 0.1 %
	double cpuBusyMs = kNA;
	double cpuWaitMs = kNA;
	double gpuBusyMs = kNA;         // PresentMon MsGPUBusy (union of the frame's GPU work), NA without GPU timing
	double gpuWaitMs = kNA;         // MsGPUWait: GPU start -> ready, minus busy
	double gpuLatencyMs = kNA;      // MsGPULatency: CPU start -> GPU start
	double displayLatencyMs = kNA;  // screen time minus CPU start (PresentMon MsDisplayLatency)
	double readyToScreenMs = kNA;   // render complete (QUEUE_OPERATIONS_END) -> screen
	double untilDisplayedMs = kNA;  // present start -> screen (PresentMon MsUntilDisplayed)
	double droppedPct = kNA;        // not displayed / frames with display info, long window
	uint32_t droppedCount = 0;
	uint32_t displayKnownCount = 0;
	uint64_t rowsTotal = 0;         // rows produced by the engine so far
	float graphPresent[kGraphPoints] = {};  // MsBetweenPresents, max per bucket, oldest first, 0 = no data
	float graphDisplay[kGraphPoints] = {};  // MsBetweenDisplayChange
	int graphCount = 0;             // valid columns at the end of the arrays
	float graphMaxMs = 0;
};

// One present, from the V1 engine (one row per present, released immediately): frame time, CPU
// time and whether it reached the screen.
struct Sample
{
	uint64_t tNs = 0;          // present start
	float betweenPresentsMs = 0;
	float cpuBusyMs = 0;
	float cpuWaitMs = 0;
	float gpuBusyMs = -1;      // < 0: the frame has no GPU data
	float gpuWaitMs = 0;
	float gpuLatencyMs = 0;
	pml::metrics::DisplayState display = pml::metrics::DisplayState::Unknown;
};

// One displayed frame, from the V2 engine (the default CSV semantics; a displayed frame is released
// when the next one reaches the screen).
struct DisplaySample
{
	uint64_t tNs = 0;          // present start of the frame
	uint64_t screenNs = 0;
	float betweenDisplayMs = 0;  // 0 = not available (first displayed frame)
	float untilDisplayedMs = 0;
	float displayLatencyMs = 0;
	float readyToScreenMs = -1;  // < 0: not available
};

class OverlayStats
{
public:
	OverlayStats(const pml::SwapchainInfo &info, uint32_t pid, uint64_t sessionStartNs);

	// Thread-safe; drops the record when collecting is off.
	void push(const pml::FrameRecord &record);
	void setCollecting(bool on);
	bool collecting() const;

	// Presenting thread: runs queued records through the engine, refreshes the snapshot's text
	// numbers every 250 ms and the graph every call.
	void update(uint64_t nowNs);
	const Snapshot &snapshot() const { return snapshot_; }
	// VkPresentModeKHR of the newest record (the mode can change per present), UINT32_MAX = none yet.
	std::atomic<uint32_t> lastPresentMode{UINT32_MAX};

	// Optional observers, called inside update() for every row the two engines release (V1: one per
	// present, at once; V2: displayed frames, held until the next one is displayed), before the row
	// is filtered. The layer leaves them empty; the desktop GUI keeps its longer per-frame history
	// with them, from the same engines as the numbers.
	std::function<void(const pml::metrics::Row &)> onPresentRow;
	std::function<void(const pml::metrics::Row &)> onDisplayRow;

	// Rows from the two engines (public for tests; update() calls them).
	void addPresentRow(const pml::metrics::Row &row);
	void addDisplayRow(const pml::metrics::Row &row);
	void recompute(uint64_t nowNs);
	// Forget everything (visibility turned on again, swapchain info changed).
	void reset();

private:
	void rebuildGraph();

	pml::SwapchainInfo info_;
	uint32_t pid_;
	uint64_t sessionStartNs_;
	// Two engines on the same records. V1 yields a row per present immediately: it drives everything
	// about presents (fps, frame time, CPU, drops), so the numbers keep flowing even when frames are
	// rarely displayed (IMMEDIATE/MAILBOX at kHz rates, where V2 holds rows back until the next
	// displayed frame). V2 is the CSV's default and drives the display metrics.
	std::unique_ptr<pml::metrics::SwapchainEngine> engineV1_;
	std::unique_ptr<pml::metrics::SwapchainEngine> engineV2_;

	std::mutex queueMutex_;
	std::vector<pml::FrameRecord> queue_;
	// update() swaps queue_ with drain_ and clears drain_ after use, so both keep their capacity and
	// push() (called inside the present) stops allocating once they have grown to a frame's records.
	std::vector<pml::FrameRecord> drain_;
	std::atomic<bool> collecting_{true};

	std::deque<Sample> samples_;
	std::deque<DisplaySample> displayed_;
	struct Bucket
	{
		int64_t id = -1;
		float present = 0;
		float display = 0;
	};
	Bucket buckets_[kGraphPoints];
	int64_t newestBucket_ = -1;
	uint64_t newestNs_ = 0;
	uint64_t lastRecomputeNs_ = 0;
	uint64_t rows_ = 0;           // V1 rows so far
	Snapshot snapshot_;
};

} // namespace pml::layer::overlay
