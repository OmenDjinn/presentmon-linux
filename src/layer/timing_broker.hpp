// SPDX-License-Identifier: MIT
// Per-swapchain present-timing broker: the layer's side of VK_EXT_present_timing.
//
// The driver keeps ONE feedback queue per swapchain, and whoever calls
// vkGetPastPresentationTimingEXT first takes the results out of it. Both the layer (for its
// FrameRecords) and the application (DXVK polls every frame) need them, so the layer owns the
// queue: every driver read is done by the broker, which keeps the reports until both consumers
// have taken what they need:
//
//   * layer side: one entry per present, turned into a FrameRecord strictly in present order once
//     its report is final (or given up on with kFrameTimingLost);
//   * app side: reports for presents the APPLICATION requested timing for are replayed through the
//     application's own vkGetPastPresentationTimingEXT with exactly the stages, time domain, array
//     sizes, flags and VK_INCOMPLETE behaviour the driver would have produced for it, and the
//     application's queue size is emulated (VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT).
//
// This file is pure logic: the driver calls sit behind TimingDriver, so the unit tests run against
// a fake without a GPU. All public methods are thread safe (one mutex per broker = per swapchain);
// the record sink is called with that mutex held.
#pragma once

#include <vulkan/vulkan.h>

#include "pml/protocol.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace pml::layer {

inline constexpr VkPresentStageFlagsEXT kDisplayStageMask =
	VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT | VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT |
	VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT;
// The stages that produce a screen time (REQUEST_DEQUEUED only yields latchNs).
inline constexpr VkPresentStageFlagsEXT kScreenStageMask =
	VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT | VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT;
inline constexpr uint32_t kMaxStages = 4;  // one per VkPresentStageFlagBitsEXT bit

// The driver calls the broker needs. `swapchain` and `device` are bound by the implementation.
class TimingDriver
{
public:
	virtual ~TimingDriver() = default;

	// vkGetPastPresentationTimingEXT for the broker's swapchain.
	virtual VkResult getPast(VkPastPresentationTimingFlagsEXT flags,
	                         VkPastPresentationTimingPropertiesEXT *props) = 0;

	// Reads CLOCK_MONOTONIC and `domain` (for the present-stage-local / swapchain-local domains:
	// the given stage and domain id) at the same instant. Times are in nanoseconds.
	virtual bool calibrate(VkTimeDomainKHR domain, uint64_t domainId, VkPresentStageFlagsEXT stage,
	                       uint64_t *monotonicNs, uint64_t *domainNs) = 0;

	// CLOCK_MONOTONIC in nanoseconds (injectable for tests).
	virtual uint64_t nowNs() = 0;

	// The swapchain's time domains changed (timeDomainsCounter moved): picks the domain id the layer
	// asks for again. Returns false when it cannot (the old id stays). Called with the broker's lock held.
	virtual bool refreshTimeDomain(uint64_t *domainId, uint64_t *counter)
	{
		(void)domainId;
		(void)counter;
		return false;
	}
};

// How present IDs reach the driver on this swapchain.
enum class IdMode
{
	None,      // the swapchain has no present-id support: reports carry id 0 and are matched by order
	App,       // the app supplies the ids (VkPresentId2KHR on a PRESENT_ID_2 swapchain, or VkPresentIdKHR:
	           // the driver reports v1 ids too); on a present without one the layer injects its own when the
	           // present may carry a VkPresentId2KHR (PresentParams::canInjectId), else it gets no layer timing
	Layer,     // the layer added PRESENT_ID_2 and injects its own increasing ids
};

struct BrokerConfig
{
	uint64_t swapchainSerial = 0;
	uint64_t swapchainHandle = 0;
	uint32_t defaultPresentMode = 0;
	IdMode idMode = IdMode::None;
	VkPresentStageFlagsEXT ourStages = 0;   // stages requested for the layer's own records; 0 = none (IdMode::None
	                                        // never requests them: late reports could not be told apart)
	uint64_t ourTimeDomainId = 0;           // time domain the layer asks for when the app gave none
	uint64_t timeDomainsCounter = 0;        // swapchain time domains counter ourTimeDomainId was chosen at
	uint64_t lostAfterNs = 500'000'000ull;  // a report not seen this long after the present is "lost"
	uint32_t lostAfterNewer = 64;           // ... or when this many newer presents are complete
	uint64_t lostAfterNewerMinAgeNs = 50'000'000ull;  // ... and the present is at least this old
	uint32_t driverHeadroom = 128;          // driver queue size = app's queue size + this (the driver accepts 65536+)
	int64_t calibrationIntervalNs = 1'000'000'000;
};

// Everything the layer knows about one present of this swapchain before calling the driver.
struct PresentParams
{
	uint64_t presentStartNs = 0;
	uint32_t threadId = 0;
	uint32_t imageIndex = 0;
	uint32_t presentMode = 0;               // VkPresentModeKHR in effect
	uint64_t appPresentId = 0;              // id the app supplied for this swapchain (0 = none), v1 or v2
	const VkPresentTimingInfoEXT *appTiming = nullptr;  // the app's entry for this swapchain, if any
	uint32_t extraFlags = 0;                // FrameFlag bits to add (e.g. kFrameOverlayDrawn)
	bool observeOnly = false;               // the app's chain goes down untouched: no layer request, no id, no queue-full emulation
	bool noLimit = false;                   // send the app's request even over its emulated queue size (see cancelPresent)
	bool canInjectId = true;                // a VkPresentId2KHR may be added (every swapchain of the present has PRESENT_ID_2)
	uint64_t overlayCpuNs = 0;
};

// What to send down for this swapchain.
struct PresentPlan
{
	uint64_t token = 0;                     // pass to endPresent
	VkPresentTimingInfoEXT timing = {};     // entry for VkPresentTimingsInfoEXT (stages may be 0)
	bool timingRequested = false;           // timing.presentStageQueries != 0
	bool inject = false;                    // add injectedId to a VkPresentId2KHR
	uint64_t injectedId = 0;
	VkResult forcedResult = VK_SUCCESS;     // emulated error for this swapchain (queue full)
	VkPresentStageFlagsEXT appStagesSent = 0;
	VkPresentStageFlagsEXT ourStagesSent = 0;
};

struct BrokerStats
{
	uint32_t entries = 0;             // presents still tracked
	uint32_t appOutstanding = 0;      // app-timed presents the app has not collected yet
	uint32_t driverOutstanding = 0;   // driver queue slots held
	uint32_t leaked = 0;              // slots whose report never arrived
	uint32_t driverQueueSize = 0;
	uint32_t appQueueSize = 0;
	uint64_t emitted = 0;
	uint64_t emittedLost = 0;
	uint64_t droppedLayerTiming = 0;  // presents where the layer skipped its own request (budget)
	uint64_t unmatchedReports = 0;
	uint64_t driverPolls = 0;
};

class TimingBroker
{
public:
	using Sink = std::function<void(const FrameRecord &)>;

	TimingBroker(const BrokerConfig &config, TimingDriver &driver, Sink sink);

	// Queue size to give the driver right after swapchain creation (before the app sets one).
	uint32_t initialDriverQueueSize();

	// The app called vkSetSwapchainPresentTimingQueueSizeEXT(size): driverQueueSizeFor() is what to
	// pass to the driver; commitAppQueueSize() records the app's size once the driver accepted it.
	uint32_t driverQueueSizeFor(uint32_t appSize) const;
	void commitAppQueueSize(uint32_t appSize, uint32_t driverSize);
	// Size the driver queue ended up with (the layer may have had to use less headroom).

	// Time domain the layer requests for presents that carry no timing request of the app.
	void setOurTimeDomainId(uint64_t id);
	uint64_t timeDomainsCounter() const;

	// Called before vkQueuePresentKHR for this swapchain: drains finished reports, decides what to
	// attach and registers the present. Must be paired with endPresent.
	PresentPlan beginPresent(const PresentParams &params);

	// The overlay ran after beginPresent (it must not run for a present that is not sent): records
	// whether it drew and how long it took. Call before endPresent.
	void noteOverlay(uint64_t token, bool drawn, uint64_t cpuNs);

	// After the driver returned; `result` is this swapchain's result.
	void endPresent(uint64_t token, uint64_t presentEndNs, VkResult result);

	// Undoes the beginPresent that returned `token`, which must be this swapchain's newest present and must
	// not have gone down. For a present that is planned again (e.g. with noLimit).
	void cancelPresent(uint64_t token);

	// The driver rejected the layer's additions (queue full) and the present was repeated without
	// them: forget the layer's request for this present and stop asking for a while.
	void dropLayerTiming(uint64_t token);

	// The app's vkGetPastPresentationTimingEXT.
	VkResult appGetPast(const VkPastPresentationTimingInfoEXT *info,
	                    VkPastPresentationTimingPropertiesEXT *out);

	// Drains the driver queue and emits whatever is final. Cheap when nothing is outstanding.
	void poll();

	// Swapchain destruction: final poll (waiting up to waitMs for late reports, with the lock released
	// while sleeping), then every unfinished present goes out as lost.
	void flush(uint32_t waitMs = 0);

	BrokerStats stats() const;

private:
	struct Entry
	{
		uint64_t token = 0;
		uint64_t driverId = 0;
		FrameRecord rec = {};
		bool submitting = true;
		// driver slot / report state
		bool slotHeld = false;
		bool injected = false;  // driverId was injected by the layer
		uint64_t prevInjectedId = 0;  // lastInjectedId_ before this present (restored by cancelPresent)
		bool complete = false;
		uint32_t haveMask = 0;
		uint64_t times[kMaxStages] = {};
		uint32_t timeDomain = 0;
		uint64_t timeDomainId = 0;
		// layer side
		bool ourExpect = false;
		VkPresentStageFlagsEXT ourStages = 0;
		bool ourEmitted = false;
		// app side
		bool appActive = false;
		VkPresentStageFlagsEXT appStages = 0;
		uint64_t appVisibleId = 0;
		uint64_t targetTime = 0;
		uint32_t appSeenMask = 0;  // stages already handed to the app in a partial result
	};

	struct Calibration
	{
		uint32_t domain = 0;
		uint64_t domainId = 0;
		uint32_t stage = 0;
		int64_t offset = 0;
		uint64_t atNs = 0;
	};

	// mayRefreshDomains: the caller holds the swapchain (a present, a swapchain create or destroy), so the
	// time domains may be queried again; vkGetSwapchainTimeDomainPropertiesEXT needs that external
	// synchronization, which the application's vkGetPastPresentationTimingEXT does not give.
	VkResult pollLocked(bool mayRefreshDomains);
	void refreshDomainsLocked();
	void processReport(const VkPastPresentationTimingEXT &report);
	Entry *findEntry(uint64_t presentId);
	void drainLocked();
	void emitLocked(Entry &entry, bool lost);
	void retireLocked();
	void releaseSlotLocked(Entry &entry);
	bool toMonotonic(uint32_t domain, uint64_t domainId, uint32_t stage, uint64_t time, uint64_t *out);
	bool appReady(const Entry &e) const;
	uint32_t zeroIdOutstanding() const;
	Entry *findToken(uint64_t token);

	BrokerConfig cfg_;
	TimingDriver &driver_;
	Sink sink_;
	mutable std::mutex mutex_;

	std::deque<Entry> entries_;
	uint64_t frameCounter_ = 0;
	uint64_t idCounter_ = 0;          // largest id sent down so far, the app's or the layer's
	uint64_t lastInjectedId_ = 0;     // largest id the layer injected
	bool injectDisabled_ = false;     // an app id came at or below an injected one: no more injection
	uint32_t appQueueSize_ = 0;
	uint32_t driverQueueSize_ = 0;
	uint32_t driverOutstanding_ = 0;
	uint32_t appOutstanding_ = 0;
	uint32_t leaked_ = 0;
	std::unordered_set<uint64_t> leakedIds_;
	uint64_t timingPropertiesCounter_ = 0;
	uint64_t timeDomainsCounter_ = 0;      // last value the driver reported
	uint64_t knownDomainsCounter_ = 0;     // value ourTimeDomainId was chosen at
	uint64_t suppressOursUntilFrame_ = 0;
	std::vector<Calibration> calibrations_;
	bool calibrationFailedLogged_ = false;
	BrokerStats stats_ = {};

	// Reusable driver result buffers.
	std::vector<VkPastPresentationTimingEXT> reports_;
	std::vector<VkPresentStageTimeEXT> stageStorage_;
	std::vector<Entry *> selection_;
};

} // namespace pml::layer
