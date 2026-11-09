// SPDX-License-Identifier: MIT
// Per-swapchain metrics engine: feeds PresentMon's UnifiedSwapChain exactly the way the
// console app does (PresentMon/OutputThread.cpp, ProcessEvents), from pml::FrameRecord.
// The CLI's CSV writer and the in-game overlay both use it, so their numbers are identical.
//
// One engine per swapchain (key: FrameRecord::swapchainSerial). Not thread-safe.
#pragma once

#include <pml/metrics/adapter.hpp>

#include <memory>
#include <unordered_map>
#include <vector>

namespace pml::metrics {

enum class MetricsMode
{
	V2,   // default: UnifiedSwapChain::ProcessPresent; one row per display instance, rows are
	      // released late (a displayed frame waits for the NEXT displayed one to know its
	      // MsDisplayedTime), possibly several at once, possibly none
	V1,   // --v1_metrics: ComputeMetricsForPresent; exactly one row per present, immediately
};

// One row to print / aggregate. Everything CsvOutput.cpp's WriteCsvRow needs.
struct Row
{
	FrameData frame;                // the present (processId, swapChainAddress, finalState, frameId, ...)
	FrameMetrics metrics;           // all computed metrics; metrics.frameType is the FrameType column
	FrameType frameType = FrameType::NotSet;
	bool isAppFrame = true;         // frameType is Application or NotSet; CPU/GPU columns are valid only then
	DisplayState display = DisplayState::Unknown;
	bool dropped = false;           // display == NotDisplayed (== metrics.isDroppedFrame after the engine's fix-up)
	bool hasDisplayMetrics = false; // display == Displayed: MsUntilDisplayed, MsBetweenDisplayChange, display latency are meaningful
	uint32_t vkPresentMode = 0;     // VkPresentModeKHR of this present
	const char *runtime = "Vulkan";         // Runtime column
	const char *presentMode = "Unknown";    // PresentMode column (presentModeName)
	int32_t syncInterval = 0;               // == frame.syncInterval
	bool allowsTearing = false;             // == metrics.allowsTearing
	uint64_t swapchainSerial = 0;
	uint64_t frameIndex = 0;                // FrameRecord::frameIndex
};

class SwapchainEngine
{
public:
	// `sessionStartNs`: QpcConverter start time (CLOCK_MONOTONIC ns), the zero of relative times.
	SwapchainEngine(SwapchainContext context, MetricsMode mode, uint64_t sessionStartNs);
	~SwapchainEngine();
	SwapchainEngine(SwapchainEngine &&) noexcept;
	SwapchainEngine &operator=(SwapchainEngine &&) noexcept;

	// Feed one record, in present order. Returns the rows that became ready.
	//   * The first present of a swapchain only seeds the state and returns nothing (both modes,
	//     as in PresentMon).
	//   * Failed presents (isFailedPresent) are ignored, as PresentMon ignores failed/lost ones.
	//   * V2: rows come back late (see MetricsMode::V2); call finish() at the end of the capture.
	std::vector<Row> process(const pml::FrameRecord &record);

	// End of capture: releases the rows still held for lookahead. Their MsDisplayedTime is 0 ("NA")
	// since no later screen time exists; nothing else is invented. PresentMon's console app does
	// not do this (its tail rows are simply never written); calling it is optional. No-op in V1
	// and after the first call.
	std::vector<Row> finish();

	// Update metadata that may change while running (e.g. engine name learned late).
	void setContext(SwapchainContext context) { ctx_ = std::move(context); }
	const SwapchainContext &context() const { return ctx_; }
	MetricsMode mode() const { return mode_; }
	const QpcConverter &qpc() const { return qpc_; }

	// Records accepted so far / presents that produced no rows because they were ignored.
	uint64_t recordsProcessed() const { return processed_; }
	uint64_t recordsIgnored() const { return ignored_; }

	// Upstream state (e.g. lastPresent), read-only.
	const pmon::util::metrics::UnifiedSwapChain &unified() const { return *unified_; }

private:
	struct Pending
	{
		DisplayState display;
		uint32_t vkPresentMode;
		uint64_t serial;
		uint64_t frameIndex;
	};

	Row makeRow(const FrameData &frame, const pmon::util::metrics::FrameMetrics &metrics);
	void takePending(uint32_t frameId, Pending &out);
	std::vector<Row> flushHeld();
	std::vector<Row> recoverFromStall(uint64_t nowNs);

	SwapchainContext ctx_;
	MetricsMode mode_;
	QpcConverter qpc_;
	std::unique_ptr<pmon::util::metrics::UnifiedSwapChain> unified_;
	std::unordered_map<uint32_t, Pending> pending_;   // by FrameData::frameId, until the row is emitted
	uint32_t nextAutoFrameId_ = 0;
	uint64_t processed_ = 0;
	uint64_t ignored_ = 0;
	uint64_t lastScreenNs_ = 0;
	uint64_t lastPresentStartNs_ = 0;
	uint64_t lastReleaseNs_ = 0;   // present time at which V2 last released rows (stall detection)
	uint64_t heldSinceRelease_ = 0;   // V2: presents in the current run without a release (see process())
	uint64_t firstHeldNs_ = 0;        // V2: present time of the first of those
	uint64_t lastIngestedNs_ = 0;     // V2: present time of the previous present
	bool finished_ = false;
};

} // namespace pml::metrics
