// SPDX-License-Identifier: MIT
// GPU timestamps around the application's queue submissions (docs/ARCHITECTURE.md, 3.1), per device.
//
// Every submission batch with command buffers gets one pre-recorded command buffer of the layer in front
// (resets its two queries, writes a TOP_OF_PIPE timestamp) and one behind (BOTTOM_OF_PIPE timestamp). The
// application's command buffers are never touched or re-recorded. Results are read without waiting, when a
// frame record is finished (fill()), converted to CLOCK_MONOTONIC through VK_KHR_calibrated_timestamps and
// attributed to frames by GpuFrameAttributor (gpu_intervals.hpp).
//
// Not timed: batches without command buffers, protected submissions, batches with a VkDeviceGroupSubmitInfo
// (its per-command-buffer arrays would need extending), queue families without timestamps or without
// graphics/compute (vkCmdResetQueryPool), and batches when every slot of the family is in flight.
//
// Threading: submit*() run on the application's submitting threads (a queue is externally synchronized),
// fill() under a swapchain broker's lock; one mutex inside guards the slots, in-flight list and attributor.
#pragma once

#include "gpu_intervals.hpp"
#include "overlay_seam.hpp"

#include <pml/protocol.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

namespace pml::layer {

struct GpuTimerStats
{
	uint64_t timedBatches = 0;
	uint64_t untimedBatches = 0;     // no free slot, protected, device group, unusable family
	uint64_t completed = 0;          // intervals read back
	uint64_t abandoned = 0;          // never completed within the timeout (slot left unused for good)
	uint64_t framesWithGpu = 0;
	uint64_t framesWithoutGpu = 0;
};

class GpuTimer
{
public:
	// nullptr when the device cannot do it (no calibrated DEVICE <-> CLOCK_MONOTONIC timestamps, no
	// loader-data callback) or PRESENTMON_GPU=0.
	static std::unique_ptr<GpuTimer> create(const DeviceContext &ctx);
	~GpuTimer();

	VkResult submit(PFN_vkQueueSubmit next, VkQueue queue, uint32_t family, uint32_t count, const VkSubmitInfo *submits,
	                VkFence fence);
	VkResult submit2(PFN_vkQueueSubmit2 next, VkQueue queue, uint32_t family, uint32_t count,
	                 const VkSubmitInfo2 *submits, VkFence fence);

	// A finished frame record: sets gpuStartNs / gpuBusyNs from the GPU work that ended by its readyNs. One
	// swapchain per device gets GPU numbers (see Impl::ownerSerial).
	void fill(pml::FrameRecord &record);
	// The swapchain is being destroyed (its last records are out): another one may get the GPU numbers.
	void forgetSwapchain(uint64_t serial);

	GpuTimerStats stats() const;

	struct Impl;

private:
	explicit GpuTimer(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl_;
};

} // namespace pml::layer
