// SPDX-License-Identifier: MIT
// GPU work of a frame, from the layer's timestamps around queue submissions (no Vulkan in here, so it is
// unit-tested without a GPU; gpu_timer.cpp feeds it).
//
// Every timed submission batch gives one interval [start, end] on CLOCK_MONOTONIC. A frame's GPU work is
// every interval that ended at or before its present's QUEUE_OPERATIONS_END time (readyNs): the present
// waits for the frame's rendering, so that work is done by then, and work of the next frame that is
// already queued (vkd3d-proton and DXVK submit and present on different threads) ends later and goes to
// the next frame. The GPU timestamps and the ready time reach CLOCK_MONOTONIC through two separate
// calibrations that disagree by some 10-20 us (measured with vk_truth), so the frame's last batch can seem
// to end just after its ready time: an interval up to kReadySlackNs after it still belongs to the frame
// when it was submitted before the frame's present call (later submissions are the next frame's). Per frame:
//   gpuStartNs = the earliest start of those intervals
//   gpuBusyNs  = the length of their union (overlapping work on several queues counts once, idle gaps
//                not at all), as PresentMon's GPUBusy
// PresentMon then derives GPULatency = gpuStart - CPU start and GPUWait = (ready - gpuStart) - busy.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pml::layer {

struct GpuInterval
{
	uint64_t startNs = 0;
	uint64_t endNs = 0;
	uint64_t submitNs = 0;   // CPU time of the vkQueueSubmit call
};

struct GpuFrameWork
{
	bool valid = false;     // false: no interval belonged to the frame (gpuStartNs stays 0 = NA)
	uint64_t startNs = 0;
	uint64_t busyNs = 0;
	uint32_t intervals = 0;
};

class GpuFrameAttributor
{
public:
	// Completed intervals not yet given to a frame are capped (a swapchain without ready times, or one
	// that stopped presenting); at the cap the older-ending half goes.
	static constexpr size_t kMaxPending = 16384;
	static constexpr uint64_t kReadySlackNs = 500'000;
	static constexpr uint64_t kMaxFrameSpanNs = 1'000'000'000;  // work ending longer before a ready time is no frame's

	// A completed interval, in any order. start > end is clamped to end.
	void add(GpuInterval interval);
	// The frame whose present's queue operations ended at readyNs (> 0) and whose present call started at
	// presentStartNs: takes every pending interval that ended at or before readyNs, and those submitted
	// before presentStartNs that ended up to kReadySlackNs after it. Frames must be taken in increasing
	// readyNs order. Intervals that ended by the previous frame's ready time (read back too late for it) or
	// more than kMaxFrameSpanNs before this one (work before the first frame, a loading screen) are dropped,
	// not charged to this frame.
	GpuFrameWork take(uint64_t readyNs, uint64_t presentStartNs);
	// A frame without a ready time: its work cannot be told apart, so it gets no GPU numbers, and
	// intervals that ended before cutNs (the frame's present end) are dropped instead of inflating the
	// next frame's numbers.
	void discardBefore(uint64_t cutNs);

	size_t pending() const { return pending_.size(); }
	uint64_t dropped() const { return dropped_; }

private:
	std::vector<GpuInterval> pending_;
	std::vector<GpuInterval> scratch_;
	uint64_t dropped_ = 0;
	uint64_t lastReadyNs_ = 0;
};

// GPU timestamp ticks -> CLOCK_MONOTONIC, from a calibration pair (vkGetCalibratedTimestampsKHR with
// VK_TIME_DOMAIN_DEVICE_KHR and VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR). Timestamps with fewer than 64 valid
// bits are unwrapped around the calibration tick (valid within half the counter range of it).
struct GpuClock
{
	uint64_t calTicks = 0;
	uint64_t calMonoNs = 0;
	double periodNs = 1.0;
	uint32_t validBits = 64;
	bool calibrated = false;

	uint64_t toMonotonic(uint64_t ticks) const;
};

} // namespace pml::layer
