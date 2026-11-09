// SPDX-License-Identifier: MIT
// See timing_broker.hpp. Calibration approach (CLOCK_MONOTONIC <-> present-stage-local time) is based on
// Swapchain::updateFeedback() in pyrofling's latency_measurement_layer.cpp (MIT, Hans-Kristian Arntzen).
#include "timing_broker.hpp"

#include "log.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace pml::layer {

namespace {

constexpr size_t kReportCapacity = 128;
constexpr size_t kMaxLeakedIds = 256;

inline uint32_t stageIndex(uint32_t stageBit)
{
	return uint32_t(__builtin_ctz(stageBit));
}

} // namespace

TimingBroker::TimingBroker(const BrokerConfig &config, TimingDriver &driver, Sink sink)
	: cfg_(config), driver_(driver), sink_(std::move(sink))
{
	driverQueueSize_ = cfg_.driverHeadroom;
	knownDomainsCounter_ = timeDomainsCounter_ = cfg_.timeDomainsCounter;
	reports_.resize(kReportCapacity);
	stageStorage_.resize(kReportCapacity * kMaxStages);
}

uint32_t TimingBroker::initialDriverQueueSize()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return driverQueueSize_;
}

uint32_t TimingBroker::driverQueueSizeFor(uint32_t appSize) const
{
	return appSize + cfg_.driverHeadroom;
}

void TimingBroker::commitAppQueueSize(uint32_t appSize, uint32_t driverSize)
{
	std::lock_guard<std::mutex> lock(mutex_);
	appQueueSize_ = appSize;
	driverQueueSize_ = std::max(driverSize, appSize);
}

void TimingBroker::setOurTimeDomainId(uint64_t id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	cfg_.ourTimeDomainId = id;
}

uint64_t TimingBroker::timeDomainsCounter() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return timeDomainsCounter_;
}

// ---------------------------------------------------------------------------------------
// Present registration
// ---------------------------------------------------------------------------------------

PresentPlan TimingBroker::beginPresent(const PresentParams &p)
{
	std::lock_guard<std::mutex> lock(mutex_);

	if (driverOutstanding_ > 0 || timeDomainsCounter_ != knownDomainsCounter_)
		pollLocked(true);
	drainLocked();

	Entry e;
	e.token = ++frameCounter_;
	PresentPlan plan;
	plan.token = e.token;

	const VkPresentTimingInfoEXT *appInfo = p.appTiming;
	const VkPresentStageFlagsEXT appRequested = appInfo ? appInfo->presentStageQueries : 0;

	// The application's own queue limit, emulated: the driver would have refused this present's
	// timing request. We still present the image (without the app's stages) and report the error.
	VkPresentStageFlagsEXT appStages = appRequested;
	if (!p.observeOnly && !p.noLimit && appRequested && (appQueueSize_ == 0 || appOutstanding_ >= appQueueSize_))
	{
		plan.forcedResult = VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT;
		appStages = 0;
	}

	// Every id sent down must be larger than the ones before it on this swapchain (VUID-VkPresentId2KHR-
	// presentIds-04999), the app's and the layer's alike. An app id at or below one the layer injected means the
	// layer broke the app's order: it never injects on this swapchain again.
	if (p.appPresentId)
	{
		if (p.appPresentId <= lastInjectedId_ && !injectDisabled_)
		{
			injectDisabled_ = true;
			PML_LOG(LogLevel::Warn, "swapchain %llu: the app's present id %llu is not above the layer's %llu; "
			        "the layer stops adding ids", (unsigned long long)cfg_.swapchainSerial,
			        (unsigned long long)p.appPresentId, (unsigned long long)lastInjectedId_);
		}
		idCounter_ = std::max(idCounter_, p.appPresentId);
	}
	// The layer's request needs an id. Without one of the app's it injects its own, which this present may not
	// carry (another swapchain in it lacks PRESENT_ID_2). An app that owns the ids may still leave presents without
	// one: vkd3d-proton sends ids only for FIFO presents (V-Sync on), none with tearing.
	const bool mayInject = p.canInjectId && !injectDisabled_;
	bool idUsable = true;
	if (cfg_.idMode != IdMode::None)
		idUsable = p.appPresentId != 0 || mayInject;

	VkPresentStageFlagsEXT ourStages = 0;
	// Without ids a report cannot be attributed once a present was given up on, so the layer asks for
	// display timing only on swapchains with ids.
	if (cfg_.ourStages && cfg_.idMode != IdMode::None && idUsable && !p.observeOnly)
	{
		const uint32_t headroom = driverQueueSize_ > appQueueSize_ ? driverQueueSize_ - appQueueSize_ : 0;
		uint32_t ourOutstanding = 0;
		for (const Entry &o : entries_)
			if (o.ourExpect && !o.complete && !o.ourEmitted)
				ourOutstanding++;
		bool budget = frameCounter_ > suppressOursUntilFrame_ && driverOutstanding_ < driverQueueSize_ &&
		              ourOutstanding < headroom && leaked_ < std::max(1u, headroom / 2);
		if (budget)
			ourStages = cfg_.ourStages;
		else
			stats_.droppedLayerTiming++;
	}

	const VkPresentStageFlagsEXT sent = appStages | ourStages;
	const uint64_t prevInjectedId = lastInjectedId_;
	uint64_t id = 0;
	if (sent && !p.observeOnly)
	{
		if (cfg_.idMode == IdMode::None)
		{
		}
		else if (p.appPresentId)
		{
			id = p.appPresentId;
		}
		else if (!mayInject)
		{
			// Only the app's own id-less request goes down (matched by order, as on an id-less swapchain).
		}
		else
		{
			id = ++idCounter_;
			lastInjectedId_ = id;
			plan.inject = true;
			plan.injectedId = id;
		}
	}

	if (appInfo)
		plan.timing = *appInfo;
	else
		plan.timing = {};
	plan.timing.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT;
	plan.timing.presentStageQueries = sent;
	// Every entry that goes down carries a domain id the driver returned, also an empty one (the timing node
	// covers every swapchain of the present once any of them needs it).
	if (!appInfo || (appInfo->timeDomainId == 0 && (ourStages || appStages == 0)))
		plan.timing.timeDomainId = cfg_.ourTimeDomainId;
	plan.timingRequested = sent != 0;
	plan.appStagesSent = appStages;
	plan.ourStagesSent = ourStages;

	FrameRecord &r = e.rec;
	r.swapchainSerial = cfg_.swapchainSerial;
	r.swapchainHandle = cfg_.swapchainHandle;
	r.frameIndex = e.token;
	r.presentId = id ? id : p.appPresentId;
	r.presentStartNs = p.presentStartNs;
	r.threadId = p.threadId;
	r.presentMode = p.presentMode;
	r.imageIndex = p.imageIndex;
	r.overlayCpuNs = p.overlayCpuNs;
	r.flags = p.extraFlags;
	if (ourStages)
		r.flags |= kFrameTimingRequested;
	if (appInfo)
		r.flags |= kFrameAppTimingRequest;
	if (p.appPresentId)
		r.flags |= kFrameAppPresentId;

	if (p.observeOnly && appStages)
		id = p.appPresentId;  // the app's own request goes down unchanged
	e.driverId = id;
	e.injected = plan.inject;
	e.prevInjectedId = prevInjectedId;
	e.slotHeld = sent != 0;
	e.ourExpect = ourStages != 0;
	e.ourStages = ourStages;
	e.appActive = appStages != 0;
	e.appStages = appStages;
	e.appVisibleId = p.appPresentId;  // the driver reports v1 and v2 ids alike
	e.targetTime = appInfo ? appInfo->targetTime : 0;
	if (e.slotHeld)
		driverOutstanding_++;
	if (e.appActive)
		appOutstanding_++;

	entries_.push_back(e);
	return plan;
}

TimingBroker::Entry *TimingBroker::findToken(uint64_t token)
{
	for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
		if (it->token == token)
			return &*it;
	return nullptr;
}

void TimingBroker::releaseSlotLocked(Entry &e)
{
	if (e.slotHeld)
	{
		e.slotHeld = false;
		if (driverOutstanding_)
			driverOutstanding_--;
	}
}

void TimingBroker::noteOverlay(uint64_t token, bool drawn, uint64_t cpuNs)
{
	std::lock_guard<std::mutex> lock(mutex_);
	Entry *e = findToken(token);
	if (!e)
		return;
	if (drawn)
		e->rec.flags |= kFrameOverlayDrawn;
	e->rec.overlayCpuNs = cpuNs;
}

void TimingBroker::endPresent(uint64_t token, uint64_t presentEndNs, VkResult result)
{
	std::lock_guard<std::mutex> lock(mutex_);
	Entry *e = findToken(token);
	if (!e)
		return;
	e->submitting = false;
	e->rec.presentEndNs = presentEndNs;
	e->rec.presentResult = result;
	if (result < 0)
		e->rec.flags |= kFramePresentFailed;
	else if (result == VK_SUBOPTIMAL_KHR)
		e->rec.flags |= kFrameSuboptimal;

	if (result < 0)
	{
		// A failed present holds no feedback slot and will never be reported.
		e->ourExpect = false;
		releaseSlotLocked(*e);
		if (e->appActive)
		{
			e->appActive = false;
			if (appOutstanding_)
				appOutstanding_--;
		}
	}
	drainLocked();
}

void TimingBroker::cancelPresent(uint64_t token)
{
	std::lock_guard<std::mutex> lock(mutex_);
	// Only beginPresent appends (presents of one swapchain are externally synchronized), so the newest
	// entry is the one to undo.
	if (entries_.empty() || entries_.back().token != token || !entries_.back().submitting)
		return;
	const Entry &e = entries_.back();
	if (e.slotHeld && driverOutstanding_)
		driverOutstanding_--;
	if (e.appActive && appOutstanding_)
		appOutstanding_--;
	if (e.injected && idCounter_ == e.driverId)
	{
		idCounter_--;
		lastInjectedId_ = e.prevInjectedId;
	}
	if (frameCounter_ == e.token)
		frameCounter_--;
	entries_.pop_back();
}

void TimingBroker::dropLayerTiming(uint64_t token)
{
	std::lock_guard<std::mutex> lock(mutex_);
	Entry *e = findToken(token);
	if (e && e->ourExpect)
	{
		e->ourExpect = false;
		e->ourStages = 0;
		e->rec.flags &= ~uint32_t(kFrameTimingRequested);
		if (!e->appActive)
			releaseSlotLocked(*e);
	}
	if (e && e->injected)
	{
		// The injected id never reached the driver; reports for this present will carry id 0.
		e->injected = false;
		e->driverId = 0;
	}
	suppressOursUntilFrame_ = frameCounter_ + 16;
}

// ---------------------------------------------------------------------------------------
// Driver feedback
// ---------------------------------------------------------------------------------------

uint32_t TimingBroker::zeroIdOutstanding() const
{
	uint32_t n = 0;
	for (const Entry &e : entries_)
		if (e.driverId == 0 && e.slotHeld && !e.complete)
			n++;
	return n;
}

void TimingBroker::poll()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (driverOutstanding_ > 0 || timeDomainsCounter_ != knownDomainsCounter_)
		pollLocked(true);
	drainLocked();
}

VkResult TimingBroker::pollLocked(bool mayRefreshDomains)
{
	VkPastPresentationTimingFlagsEXT flags = VK_PAST_PRESENTATION_TIMING_ALLOW_PARTIAL_RESULTS_BIT_EXT;
	// Presents without ids are matched by order, which needs the driver's in-order delivery.
	if (zeroIdOutstanding() == 0)
		flags |= VK_PAST_PRESENTATION_TIMING_ALLOW_OUT_OF_ORDER_RESULTS_BIT_EXT;

	VkResult last = VK_SUCCESS;
	for (int iteration = 0; iteration < 8; iteration++)
	{
		for (size_t i = 0; i < kReportCapacity; i++)
		{
			VkPastPresentationTimingEXT &r = reports_[i];
			r = {};
			r.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
			r.pPresentStages = &stageStorage_[i * kMaxStages];
			r.presentStageCount = kMaxStages;
		}
		VkPastPresentationTimingPropertiesEXT props = {};
		props.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;
		props.presentationTimingCount = uint32_t(kReportCapacity);
		props.pPresentationTimings = reports_.data();

		stats_.driverPolls++;
		last = driver_.getPast(flags, &props);
		if (last < 0)
			return last;
		timingPropertiesCounter_ = props.timingPropertiesCounter;
		timeDomainsCounter_ = props.timeDomainsCounter;
		if (timeDomainsCounter_ != knownDomainsCounter_ && mayRefreshDomains)
			refreshDomainsLocked();
		for (uint32_t i = 0; i < props.presentationTimingCount && i < kReportCapacity; i++)
			processReport(reports_[i]);
		if (last != VK_INCOMPLETE || props.presentationTimingCount == 0)
			break;
	}
	return last < 0 ? last : VK_SUCCESS;
}

// The swapchain's time domains changed (window moved to another display, mode change, ...): pick the
// domain id the layer asks for again. If that fails, knownDomainsCounter_ stays behind and the next poll
// that may refresh tries again.
void TimingBroker::refreshDomainsLocked()
{
	uint64_t id = 0, counter = 0;
	if (!driver_.refreshTimeDomain(&id, &counter))
		return;
	cfg_.ourTimeDomainId = id;
	knownDomainsCounter_ = counter;
	// Every domain is calibrated again on its next use. A domain id that was retired keeps its offset for the
	// frames still reported in it: calibrating it now yields 0, which toMonotonic counts as a failure.
	for (Calibration &c : calibrations_)
		c.atNs = 0;
}

TimingBroker::Entry *TimingBroker::findEntry(uint64_t presentId)
{
	for (Entry &e : entries_)
	{
		if (e.complete || !e.slotHeld)
			continue;
		if (e.driverId == presentId)
			return &e;
	}
	return nullptr;
}

void TimingBroker::processReport(const VkPastPresentationTimingEXT &r)
{
	Entry *e = findEntry(r.presentId);
	if (!e)
	{
		if (r.reportComplete && r.presentId && leakedIds_.erase(r.presentId))
		{
			// A report we had given up on arrived after all: its slot is free again.
			if (leaked_)
				leaked_--;
			if (driverOutstanding_)
				driverOutstanding_--;
		}
		else
		{
			stats_.unmatchedReports++;
		}
		return;
	}

	for (uint32_t i = 0; i < r.presentStageCount && r.pPresentStages; i++)
	{
		const VkPresentStageTimeEXT &s = r.pPresentStages[i];
		if (!s.stage || (s.stage & (s.stage - 1)) || s.stage > VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT)
			continue;
		if (s.time)
		{
			e->times[stageIndex(s.stage)] = s.time;
			e->haveMask |= s.stage;
		}
	}
	e->timeDomain = uint32_t(r.timeDomain);
	e->timeDomainId = r.timeDomainId;
	if (r.reportComplete)
	{
		e->complete = true;
		releaseSlotLocked(*e);
	}
}

bool TimingBroker::toMonotonic(uint32_t domain, uint64_t domainId, uint32_t stage, uint64_t time, uint64_t *out)
{
	if (domain == uint32_t(VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR))
	{
		*out = time;
		return true;
	}

	const uint64_t now = driver_.nowNs();
	Calibration *c = nullptr;
	for (Calibration &cal : calibrations_)
		if (cal.domain == domain && cal.domainId == domainId && cal.stage == stage)
			c = &cal;

	if (!c || int64_t(now - c->atNs) > cfg_.calibrationIntervalNs)
	{
		uint64_t mono = 0, domainNs = 0;
		// A zero reading is what the driver may return for a domain id it no longer offers: not a calibration.
		if (driver_.calibrate(VkTimeDomainKHR(domain), domainId, stage, &mono, &domainNs) && domainNs != 0)
		{
			if (!c)
			{
				calibrations_.push_back({});
				c = &calibrations_.back();
				c->domain = domain;
				c->domainId = domainId;
				c->stage = stage;
			}
			c->offset = int64_t(mono) - int64_t(domainNs);
			c->atNs = now;
		}
		else
		{
			if (c)
				c->atNs = now;   // keep the previous offset; try again after the interval, not on every frame
			if (!calibrationFailedLogged_)
			{
				calibrationFailedLogged_ = true;
				PML_LOG(LogLevel::Warn, "calibrating time domain %u (id %llu) failed; %s", domain,
				        (unsigned long long)domainId, c ? "using the previous offset" : "timestamps dropped");
			}
		}
	}
	if (!c)
		return false;
	*out = uint64_t(int64_t(time) + c->offset);
	return true;
}

// ---------------------------------------------------------------------------------------
// Emission, in present order
// ---------------------------------------------------------------------------------------

void TimingBroker::emitLocked(Entry &e, bool lost)
{
	FrameRecord r = e.rec;
	if (e.ourExpect)
	{
		auto convert = [&](uint32_t stageBit) -> uint64_t {
			if (!(e.ourStages & stageBit) || !(e.haveMask & stageBit))
				return 0;
			uint64_t out = 0;
			if (!toMonotonic(e.timeDomain, e.timeDomainId, stageBit, e.times[stageIndex(stageBit)], &out))
				return 0;
			return out;
		};
		r.readyNs = convert(VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT);
		r.latchNs = convert(VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT);
		uint64_t visible = convert(VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT);
		uint64_t firstOut = convert(VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
		if (visible)
		{
			r.screenNs = visible;
			r.screenStage = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT;
		}
		else if (firstOut)
		{
			r.screenNs = firstOut;
			r.screenStage = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
		}

		if (e.complete && !lost)
			r.flags |= kFrameTimingComplete;
		else
			r.flags |= kFrameTimingLost;
		if (r.screenNs)
			r.flags |= kFrameDisplayed;
		else if (e.complete && !lost && (e.ourStages & kScreenStageMask))
			r.flags |= kFrameNotDisplayed;
	}

	e.ourEmitted = true;
	stats_.emitted++;
	if (r.flags & kFrameTimingLost)
		stats_.emittedLost++;
	if (sink_)
		sink_(r);
}

void TimingBroker::drainLocked()
{
	const uint64_t now = driver_.nowNs();
	for (size_t i = 0; i < entries_.size(); i++)
	{
		Entry &e = entries_[i];
		if (e.ourEmitted)
			continue;
		if (e.submitting)
			break;
		if (!e.ourExpect || e.complete)
		{
			emitLocked(e, false);
			continue;
		}

		bool lost = now >= e.rec.presentEndNs + cfg_.lostAfterNs;
		if (!lost)
		{
			uint32_t newerComplete = 0;
			for (size_t j = i + 1; j < entries_.size(); j++)
				if (entries_[j].ourExpect && entries_[j].complete)
					newerComplete++;
			// Reports can complete in bursts, so a large number of newer completions only counts once
			// the present is old enough to not be merely waiting for the same burst.
			lost = newerComplete >= cfg_.lostAfterNewer && now >= e.rec.presentEndNs + cfg_.lostAfterNewerMinAgeNs;
		}
		if (!lost)
			break;
		emitLocked(e, true);
	}
	retireLocked();
}

void TimingBroker::retireLocked()
{
	std::erase_if(entries_, [&](Entry &e) {
		if (!e.ourEmitted || e.appActive)
			return false;
		if (e.slotHeld)
		{
			// Given up on, but the driver may still hold the slot (and report later). Out of memory, the id
			// is not remembered and the slot stays counted as leaked; the predicate must not throw.
			leaked_++;
			if (e.driverId && leakedIds_.size() < kMaxLeakedIds)
			{
				try
				{
					leakedIds_.insert(e.driverId);
				}
				catch (...)
				{
				}
			}
		}
		return true;
	});
}

void TimingBroker::flush(uint32_t waitMs)
{
	const uint64_t start = driver_.nowNs();
	const uint64_t waitNs = uint64_t(waitMs) * 1'000'000ull;
	for (;;)
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			const VkResult polled = driverOutstanding_ > 0 ? pollLocked(true) : VK_SUCCESS;
			bool pending = false;
			uint64_t newestPendingEnd = 0;
			for (const Entry &e : entries_)
				if (e.ourExpect && !e.complete && !e.ourEmitted)
				{
					pending = true;
					newestPendingEnd = std::max(newestPendingEnd, e.rec.presentEndNs);
				}
			// Reports come within a few refreshes of their present, if at all: wait at most waitMs, and no
			// longer than waitMs after the newest pending present (a swapchain destroyed long after its last
			// present has nothing left to wait for), and not at all once the driver reports an error.
			const uint64_t now = driver_.nowNs();
			if (!pending || polled < 0 || waitMs == 0 || now - start >= waitNs ||
			    (newestPendingEnd != 0 && now >= newestPendingEnd + waitNs))
				break;
		}
		// Sleep without the lock: other threads (the app's GetPastPresentationTiming, presents) go on.
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	std::lock_guard<std::mutex> lock(mutex_);
	for (Entry &e : entries_)
	{
		if (e.ourEmitted)
			continue;
		e.submitting = false;
		emitLocked(e, e.ourExpect && !e.complete);
	}
	entries_.clear();
	driverOutstanding_ = 0;
	appOutstanding_ = 0;
}

// ---------------------------------------------------------------------------------------
// The application's view
// ---------------------------------------------------------------------------------------

bool TimingBroker::appReady(const Entry &e) const
{
	return e.complete || (e.haveMask & e.appStages) == e.appStages;
}

VkResult TimingBroker::appGetPast(const VkPastPresentationTimingInfoEXT *info,
                                  VkPastPresentationTimingPropertiesEXT *out)
{
	std::lock_guard<std::mutex> lock(mutex_);

	VkResult polled = pollLocked(false);   // the app's thread does not hold the swapchain: no domain query here
	if (polled < 0)
		return polled;
	drainLocked();

	out->timingPropertiesCounter = timingPropertiesCounter_;
	out->timeDomainsCounter = timeDomainsCounter_;

	const bool partial = (info->flags & VK_PAST_PRESENTATION_TIMING_ALLOW_PARTIAL_RESULTS_BIT_EXT) != 0;
	const bool outOfOrder = (info->flags & VK_PAST_PRESENTATION_TIMING_ALLOW_OUT_OF_ORDER_RESULTS_BIT_EXT) != 0;

	selection_.clear();
	for (Entry &e : entries_)
	{
		if (!e.appActive)
			continue;
		bool ready = appReady(e);
		bool partialNew = partial && !ready && ((e.haveMask & e.appStages) & ~e.appSeenMask) != 0;
		if (ready || partialNew)
			selection_.push_back(&e);
		else if (!outOfOrder)
			break;  // results are returned in present order: later ones wait for this one
	}

	if (!out->pPresentationTimings)
	{
		out->presentationTimingCount = uint32_t(selection_.size());
		return VK_SUCCESS;
	}

	const uint32_t capacity = out->presentationTimingCount;
	const uint32_t count = uint32_t(std::min<size_t>(capacity, selection_.size()));
	bool stagesIncomplete = false;
	for (uint32_t i = 0; i < count; i++)
	{
		Entry &e = *selection_[i];
		const bool ready = appReady(e);
		VkPastPresentationTimingEXT &el = out->pPresentationTimings[i];
		el.presentId = e.appVisibleId;
		el.targetTime = e.targetTime;
		el.timeDomain = VkTimeDomainKHR(e.timeDomain);
		el.timeDomainId = e.timeDomainId;
		el.reportComplete = ready ? VK_TRUE : VK_FALSE;

		// Without a stage array this is the first pass of the two-call idiom: report how many stages
		// there will be and leave the entry for the second call.
		const bool countOnly = el.pPresentStages == nullptr;
		const uint32_t stageCapacity = countOnly ? 0 : el.presentStageCount;
		uint32_t written = 0;
		for (uint32_t idx = 0; idx < kMaxStages; idx++)
		{
			const uint32_t bit = 1u << idx;
			if (!(e.appStages & bit))
				continue;
			const bool have = (e.haveMask & bit) != 0;
			if (!ready && !have)
				continue;
			if (written < stageCapacity)
				el.pPresentStages[written] = {bit, have ? e.times[idx] : 0};
			written++;
		}
		if (countOnly)
		{
			el.presentStageCount = written;
			continue;
		}
		el.presentStageCount = std::min(written, stageCapacity);
		if (written > stageCapacity)
			stagesIncomplete = true;

		if (ready)
		{
			e.appActive = false;
			if (appOutstanding_)
				appOutstanding_--;
		}
		else
		{
			e.appSeenMask = e.haveMask & e.appStages;
		}
	}
	out->presentationTimingCount = count;
	retireLocked();
	return (selection_.size() > capacity || stagesIncomplete) ? VK_INCOMPLETE : VK_SUCCESS;
}

BrokerStats TimingBroker::stats() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	BrokerStats s = stats_;
	s.entries = uint32_t(entries_.size());
	s.appOutstanding = appOutstanding_;
	s.driverOutstanding = driverOutstanding_;
	s.leaked = leaked_;
	s.driverQueueSize = driverQueueSize_;
	s.appQueueSize = appQueueSize_;
	return s;
}

} // namespace pml::layer
