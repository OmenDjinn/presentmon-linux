// SPDX-License-Identifier: MIT
// Unit tests for TimingBroker against a fake driver (no GPU).
//
// The fake follows the VK_EXT_present_timing semantics as documented and observed on the NVIDIA driver:
// reports are removed from the queue when returned complete, the queue refuses new timed presents
// when full, ALLOW_PARTIAL / ALLOW_OUT_OF_ORDER change what is returned, the stage list of a complete
// report has every requested stage (time 0 = no result).
#include "timing_broker.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include <map>
#include <sstream>

using namespace pml;
using namespace pml::layer;

namespace {

constexpr uint32_t QOE = VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT;
constexpr uint32_t DEQ = VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT;
constexpr uint32_t FPO = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
constexpr uint32_t FPV = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT;

class FakeDriver : public TimingDriver
{
public:
	struct Slot
	{
		uint64_t id = 0;
		uint32_t requested = 0;
		uint64_t targetTime = 0;
		uint64_t times[4] = {};
		uint32_t have = 0;
		uint32_t returned = 0;
		bool complete = false;
		uint64_t domainId = 0;
	};

	// Times reported by this driver are in a domain that runs `domainOffset` ahead of CLOCK_MONOTONIC.
	VkTimeDomainKHR domain = VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT;
	uint64_t domainId = 7;
	int64_t domainOffset = 5'000'000'000;
	uint64_t now = 10'000'000'000;
	uint32_t queueSize = 8;
	uint64_t propsCounter = 1, domainsCounter = 1;
	int calibrateCalls = 0;
	int getPastCalls = 0;
	bool failCalibration = false;
	std::vector<Slot> queue;

	uint64_t tick = 0;  // added to the clock on every read (lets flush() time out in tests)
	int refreshCalls = 0;
	bool failRefresh = false;
	bool zeroForRetiredIds = false;      // calibrate() reads 0 for a domain id other than the current one (the spec allows it)
	VkResult getPastError = VK_SUCCESS;  // returned by getPast instead of results (surface lost, ...)
	uint64_t nowNs() override
	{
		uint64_t t = now;
		now += tick;
		return t;
	}

	bool refreshTimeDomain(uint64_t *id, uint64_t *counter) override
	{
		refreshCalls++;
		if (failRefresh)
			return false;
		*id = domainId;
		*counter = domainsCounter;
		return true;
	}

	bool calibrate(VkTimeDomainKHR, uint64_t id, VkPresentStageFlagsEXT, uint64_t *mono, uint64_t *dom) override
	{
		calibrateCalls++;
		if (failCalibration)
			return false;
		*mono = now;
		*dom = zeroForRetiredIds && id != domainId ? 0 : uint64_t(int64_t(now) + domainOffset);
		return true;
	}

	// Mirrors vkQueuePresentKHR with a timing request.
	VkResult present(uint64_t id, const VkPresentTimingInfoEXT &timing)
	{
		if (!timing.presentStageQueries)
			return VK_SUCCESS;
		if (queue.size() >= queueSize)
			return VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT;
		Slot s;
		s.id = id;
		s.requested = timing.presentStageQueries;
		s.targetTime = timing.targetTime;
		s.domainId = timing.timeDomainId;
		queue.push_back(s);
		return VK_SUCCESS;
	}

	Slot *find(uint64_t id)
	{
		for (Slot &s : queue)
			if (s.id == id && !s.complete)
				return &s;
		return nullptr;
	}

	// Stage times are given in MONOTONIC nanoseconds and stored in the driver's domain.
	void report(uint64_t id, uint32_t stage, uint64_t monoTime, bool complete = false)
	{
		Slot *s = find(id);
		ASSERT_NE(s, nullptr) << "no slot for id " << id;
		int idx = __builtin_ctz(stage);
		s->times[idx] = monoTime ? uint64_t(int64_t(monoTime) + domainOffset) : 0;
		if (monoTime)
			s->have |= stage;
		// The driver reports complete as soon as every requested stage has a result.
		if (complete || (s->have & s->requested) == s->requested)
			s->complete = true;
	}

	void finish(uint64_t id, uint64_t qoe, uint64_t fpo)
	{
		report(id, QOE, qoe);
		if (find(id))
			report(id, FPO, fpo, true);
	}

	VkResult getPast(VkPastPresentationTimingFlagsEXT flags, VkPastPresentationTimingPropertiesEXT *props) override
	{
		getPastCalls++;
		if (getPastError != VK_SUCCESS)
			return getPastError;
		const bool partial = flags & VK_PAST_PRESENTATION_TIMING_ALLOW_PARTIAL_RESULTS_BIT_EXT;
		const bool ooo = flags & VK_PAST_PRESENTATION_TIMING_ALLOW_OUT_OF_ORDER_RESULTS_BIT_EXT;
		props->timingPropertiesCounter = propsCounter;
		props->timeDomainsCounter = domainsCounter;

		std::vector<size_t> eligible;
		for (size_t i = 0; i < queue.size(); i++)
		{
			const Slot &s = queue[i];
			bool ready = s.complete || (partial && (s.have & ~s.returned));
			if (ready)
				eligible.push_back(i);
			else if (!ooo)
				break;
		}
		if (!props->pPresentationTimings)
		{
			props->presentationTimingCount = uint32_t(eligible.size());
			return VK_SUCCESS;
		}
		uint32_t n = std::min<uint32_t>(props->presentationTimingCount, uint32_t(eligible.size()));
		std::vector<size_t> remove;
		for (uint32_t k = 0; k < n; k++)
		{
			Slot &s = queue[eligible[k]];
			VkPastPresentationTimingEXT &el = props->pPresentationTimings[k];
			el.presentId = s.id;
			el.targetTime = s.targetTime;
			el.timeDomain = domain;
			el.timeDomainId = s.domainId ? s.domainId : domainId;   // a report is in the domain it was requested in
			el.reportComplete = s.complete;
			uint32_t cap = el.presentStageCount, w = 0;
			for (uint32_t idx = 0; idx < 4; idx++)
			{
				uint32_t bit = 1u << idx;
				if (!(s.requested & bit))
					continue;
				if (!s.complete && !(s.have & bit))
					continue;
				if (w < cap)
					el.pPresentStages[w] = {bit, s.times[idx]};
				w++;
			}
			el.presentStageCount = std::min(w, cap);
			s.returned = s.have;
			if (s.complete)
				remove.push_back(eligible[k]);
		}
		for (auto it = remove.rbegin(); it != remove.rend(); ++it)
			queue.erase(queue.begin() + long(*it));
		props->presentationTimingCount = n;
		return eligible.size() > n ? VK_INCOMPLETE : VK_SUCCESS;
	}
};

struct Fixture
{
	FakeDriver driver;
	std::vector<FrameRecord> records;
	std::unique_ptr<TimingBroker> broker;
	uint32_t appQueue = 0;

	explicit Fixture(IdMode mode = IdMode::Layer, uint32_t stages = QOE | FPO, uint32_t headroom = 16)
	{
		BrokerConfig cfg;
		cfg.swapchainSerial = 1;
		cfg.swapchainHandle = 0xabc;
		cfg.idMode = mode;
		cfg.ourStages = stages;
		cfg.ourTimeDomainId = driver.domainId;
		cfg.driverHeadroom = headroom;
		broker = std::make_unique<TimingBroker>(cfg, driver, [this](const FrameRecord &r) { records.push_back(r); });
		driver.queueSize = broker->initialDriverQueueSize();
	}

	void setAppQueue(uint32_t size)
	{
		appQueue = size;
		driver.queueSize = broker->driverQueueSizeFor(size);
		broker->commitAppQueueSize(size, driver.queueSize);
	}

	struct Result
	{
		PresentPlan plan;
		VkResult result;
		uint64_t id;  // id the driver saw
	};

	Result present(uint64_t appId = 0, const VkPresentTimingInfoEXT *appTiming = nullptr, bool canInjectId = true)
	{
		PresentParams p;
		p.presentStartNs = driver.now;
		p.presentMode = VK_PRESENT_MODE_FIFO_KHR;
		p.appPresentId = appId;
		p.appTiming = appTiming;
		p.canInjectId = canInjectId;
		Result r;
		r.plan = broker->beginPresent(p);
		r.id = r.plan.inject ? r.plan.injectedId : appId;
		if (r.plan.forcedResult != VK_SUCCESS)
		{
			r.result = r.plan.forcedResult;
			broker->endPresent(r.plan.token, driver.now, r.result);
			return r;
		}
		r.result = driver.present(r.id, r.plan.timing);
		broker->endPresent(r.plan.token, driver.now + 10, r.result);
		return r;
	}
};

VkPresentTimingInfoEXT appInfo(uint32_t stages, uint64_t domainId = 7, uint64_t target = 0)
{
	VkPresentTimingInfoEXT t = {VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT};
	t.presentStageQueries = stages;
	t.timeDomainId = domainId;
	t.targetTime = target;
	return t;
}

// What the app received from one call, as text, so two runs can be compared.
struct AppView
{
	VkResult result = VK_SUCCESS;
	uint32_t count = 0;
	uint64_t propsCounter = 0, domainsCounter = 0;
	std::string text;
};

AppView appPoll(const std::function<VkResult(const VkPastPresentationTimingInfoEXT *, VkPastPresentationTimingPropertiesEXT *)> &call,
                uint32_t capacity, uint32_t flags, uint32_t stageCapacity = 4, bool countOnly = false)
{
	std::vector<VkPastPresentationTimingEXT> reports(capacity);
	std::vector<VkPresentStageTimeEXT> stages(capacity * 4);
	for (uint32_t i = 0; i < capacity; i++)
	{
		reports[i] = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT};
		reports[i].pPresentStages = &stages[i * 4];
		reports[i].presentStageCount = stageCapacity;
	}
	VkPastPresentationTimingInfoEXT info = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT};
	info.flags = flags;
	VkPastPresentationTimingPropertiesEXT props = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT};
	props.presentationTimingCount = capacity;
	props.pPresentationTimings = countOnly ? nullptr : reports.data();
	AppView v;
	v.result = call(&info, &props);
	v.count = props.presentationTimingCount;
	v.propsCounter = props.timingPropertiesCounter;
	v.domainsCounter = props.timeDomainsCounter;
	std::ostringstream os;
	os << "rc=" << v.result << " n=" << v.count << " ctr=" << v.propsCounter << "/" << v.domainsCounter;
	if (!countOnly)
	{
		for (uint32_t i = 0; i < v.count; i++)
		{
			const auto &r = reports[i];
			os << " [id=" << r.presentId << " tgt=" << r.targetTime << " dom=" << r.timeDomain << "/" << r.timeDomainId
			   << " done=" << r.reportComplete << " stages=";
			for (uint32_t k = 0; k < r.presentStageCount; k++)
				os << r.pPresentStages[k].stage << ":" << r.pPresentStages[k].time << ",";
			os << "]";
		}
	}
	v.text = os.str();
	return v;
}

AppView pollBroker(Fixture &f, uint32_t capacity, uint32_t flags, uint32_t stageCapacity = 4, bool countOnly = false)
{
	return appPoll([&](auto *i, auto *p) { return f.broker->appGetPast(i, p); }, capacity, flags, stageCapacity, countOnly);
}

AppView pollDriver(FakeDriver &d, uint32_t capacity, uint32_t flags, uint32_t stageCapacity = 4, bool countOnly = false)
{
	return appPoll([&](auto *i, auto *p) { return d.getPast(i->flags, p); }, capacity, flags, stageCapacity, countOnly);
}

} // namespace

// ---------------------------------------------------------------------------------------
// Layer side
// ---------------------------------------------------------------------------------------

TEST(Broker, InOrderRecordsConvertToMonotonic)
{
	Fixture f;
	for (int i = 0; i < 5; i++)
	{
		f.driver.now += 1'000'000;
		f.present();
	}
	EXPECT_TRUE(f.records.empty());  // nothing reported yet, everything outstanding
	for (uint64_t id = 1; id <= 5; id++)
		f.driver.finish(id, 20'000'000 + id * 1000, 30'000'000 + id * 4'166'667);
	f.broker->poll();
	ASSERT_EQ(f.records.size(), 5u);
	for (uint64_t i = 0; i < 5; i++)
	{
		const FrameRecord &r = f.records[i];
		EXPECT_EQ(r.frameIndex, i + 1);
		EXPECT_EQ(r.presentId, i + 1);
		EXPECT_EQ(r.swapchainSerial, 1u);
		EXPECT_EQ(r.readyNs, 20'000'000 + (i + 1) * 1000) << "converted back to CLOCK_MONOTONIC";
		EXPECT_EQ(r.screenNs, 30'000'000 + (i + 1) * 4'166'667);
		EXPECT_EQ(r.screenStage, uint32_t(FPO));
		EXPECT_EQ(r.flags & (kFrameTimingRequested | kFrameTimingComplete | kFrameDisplayed),
		          kFrameTimingRequested | kFrameTimingComplete | kFrameDisplayed);
		EXPECT_EQ(r.flags & (kFrameTimingLost | kFrameNotDisplayed | kFrameAppPresentId), 0u);
	}
	EXPECT_EQ(f.broker->stats().driverOutstanding, 0u);
}

TEST(Broker, ReplacedImageIsCompleteButNotDisplayed)
{
	// MAILBOX on this driver: the report is complete, the first-pixel-out time is 0.
	Fixture f;
	f.present();
	f.driver.finish(1, 20'000'000, 0);
	f.broker->poll();
	ASSERT_EQ(f.records.size(), 1u);
	EXPECT_TRUE(f.records[0].flags & kFrameTimingComplete);
	EXPECT_TRUE(f.records[0].flags & kFrameNotDisplayed);
	EXPECT_FALSE(f.records[0].flags & kFrameDisplayed);
	EXPECT_EQ(f.records[0].screenNs, 0u);
	EXPECT_EQ(f.records[0].readyNs, 20'000'000u);
}

TEST(Broker, QueueOpsOnlySurfaceHasNoDisplayInfo)
{
	// Xwayland: only QUEUE_OPERATIONS_END exists. Complete, but neither displayed nor "not displayed".
	Fixture f(IdMode::Layer, QOE);
	f.present();
	f.driver.report(1, QOE, 20'000'000, true);
	f.broker->poll();
	ASSERT_EQ(f.records.size(), 1u);
	EXPECT_EQ(f.records[0].readyNs, 20'000'000u);
	EXPECT_EQ(f.records[0].screenNs, 0u);
	EXPECT_TRUE(f.records[0].flags & kFrameTimingComplete);
	EXPECT_FALSE(f.records[0].flags & (kFrameDisplayed | kFrameNotDisplayed | kFrameTimingLost));
}

TEST(Broker, OutOfOrderCompletionStillEmitsInPresentOrder)
{
	Fixture f;
	for (int i = 0; i < 3; i++)
		f.present();
	f.driver.finish(3, 20'000'000, 30'000'000);
	f.broker->poll();
	EXPECT_TRUE(f.records.empty()) << "present 1 and 2 are still waiting";
	f.driver.finish(2, 21'000'000, 31'000'000);
	f.broker->poll();
	EXPECT_TRUE(f.records.empty());
	f.driver.finish(1, 22'000'000, 32'000'000);
	f.broker->poll();
	ASSERT_EQ(f.records.size(), 3u);
	EXPECT_EQ(f.records[0].frameIndex, 1u);
	EXPECT_EQ(f.records[1].frameIndex, 2u);
	EXPECT_EQ(f.records[2].frameIndex, 3u);
}

TEST(Broker, PartialResultsAreMergedBeforeEmission)
{
	Fixture f;
	f.present();
	f.driver.report(1, QOE, 20'000'000);  // QOE arrives first, FPO later
	f.broker->poll();
	EXPECT_TRUE(f.records.empty());
	f.driver.report(1, FPO, 30'000'000, true);
	f.broker->poll();
	ASSERT_EQ(f.records.size(), 1u);
	EXPECT_EQ(f.records[0].readyNs, 20'000'000u);
	EXPECT_EQ(f.records[0].screenNs, 30'000'000u);
}

TEST(Broker, LostAfterTimeoutKeepsStreamMoving)
{
	Fixture f;
	f.present();  // id 1 never reported
	f.present();
	f.driver.finish(2, 20'000'000, 30'000'000);
	f.broker->poll();
	EXPECT_TRUE(f.records.empty());
	f.driver.now += 400'000'000;
	f.broker->poll();
	EXPECT_TRUE(f.records.empty());
	f.driver.now += 200'000'000;  // > 500 ms since the present
	f.broker->poll();
	ASSERT_EQ(f.records.size(), 2u);
	EXPECT_TRUE(f.records[0].flags & kFrameTimingLost);
	EXPECT_FALSE(f.records[0].flags & kFrameTimingComplete);
	EXPECT_EQ(f.records[0].frameIndex, 1u);
	EXPECT_TRUE(f.records[1].flags & kFrameTimingComplete);
	EXPECT_EQ(f.broker->stats().leaked, 1u);

	// The report shows up after all: the leaked slot is accounted for again.
	f.driver.finish(1, 20'000'000, 30'000'000);
	f.broker->poll();
	EXPECT_EQ(f.broker->stats().leaked, 0u);
	EXPECT_EQ(f.records.size(), 2u);
}

TEST(Broker, LostWhenManyNewerPresentsCompleted)
{
	Fixture f;
	f.setAppQueue(0);
	f.present();  // id 1 lost
	for (int i = 0; i < 70; i++)
	{
		f.driver.now += 2'000'000;  // 70 frames take 140 ms: old enough
		f.present();
		f.driver.finish(f.driver.queue.back().id, 20'000'000, 30'000'000);
		f.broker->poll();
	}
	ASSERT_GE(f.records.size(), 1u);
	EXPECT_TRUE(f.records[0].flags & kFrameTimingLost);
}

TEST(Broker, NewerCompletionsAloneDoNotDeclareAPresentLostInsideABurst)
{
	Fixture f;
	f.setAppQueue(0);
	f.present();  // id 1: its report comes late, but within the same millisecond burst
	for (int i = 0; i < 70; i++)
	{
		f.present();
		f.driver.finish(f.driver.queue.back().id, 20'000'000, 30'000'000);
		f.broker->poll();
	}
	EXPECT_TRUE(f.records.empty()) << "all 70 newer ones wait for present 1";
	f.driver.finish(1, 20'000'000, 30'000'000);
	f.broker->poll();
	ASSERT_EQ(f.records.size(), 71u);
	EXPECT_FALSE(f.records[0].flags & kFrameTimingLost);
}

TEST(Broker, WithoutIdsPresentsGetNoLayerTiming)
{
	// The app owns the ids (IdMode::App), passes none, and the present may not carry an injected one (a v1-only
	// app, or another swapchain of the present lacks PRESENT_ID_2): no layer request, CPU data only, immediate.
	Fixture f(IdMode::App);
	f.present(0, nullptr, false);
	ASSERT_EQ(f.records.size(), 1u);
	EXPECT_FALSE(f.records[0].flags & kFrameTimingRequested);
	EXPECT_EQ(f.records[0].readyNs, 0u);
	EXPECT_TRUE(f.driver.queue.empty());
	// With an id it is timed, and the app's id is used unchanged.
	auto r = f.present(42, nullptr, false);
	EXPECT_FALSE(r.plan.inject);
	EXPECT_EQ(r.id, 42u);
	f.driver.finish(42, 20'000'000, 30'000'000);
	f.broker->poll();
	ASSERT_EQ(f.records.size(), 2u);
	EXPECT_TRUE(f.records[1].flags & kFrameAppPresentId);
	EXPECT_EQ(f.records[1].presentId, 42u);
}

TEST(Broker, AppOwnedIdsGetLayerIdsOnPresentsWithout)
{
	// vkd3d-proton creates its swapchains with PRESENT_ID_2 but sends ids only on FIFO presents (V-Sync on),
	// as max(previous id + 1, present count). With tearing it sends none: the layer injects its own, above
	// every id so far, and the app's later ids stay above them (one present count per present).
	Fixture f(IdMode::App);
	uint64_t presentCount = 100;  // DXGI presents before this Vulkan swapchain
	uint64_t appId = 0;
	std::vector<uint64_t> sent;
	for (int i = 0; i < 12; i++)
	{
		presentCount++;
		const bool vsync = i < 3 || i >= 8;
		if (vsync)
			appId = std::max(appId + 1, presentCount);
		auto r = f.present(vsync ? appId : 0);
		EXPECT_EQ(r.plan.inject, !vsync) << i;
		EXPECT_TRUE(r.plan.timingRequested) << i;
		sent.push_back(r.id);
		f.driver.finish(r.id, f.driver.now + 1'000'000, f.driver.now + 2'000'000);
		f.driver.now += 7'000'000;
	}
	for (size_t i = 1; i < sent.size(); i++)
		EXPECT_GT(sent[i], sent[i - 1]) << "ids must increase: " << i;
	f.broker->poll();
	ASSERT_EQ(f.records.size(), 12u);
	for (const FrameRecord &r : f.records)
	{
		EXPECT_TRUE(r.flags & kFrameTimingComplete) << r.frameIndex;
		EXPECT_NE(r.screenNs, 0u) << r.frameIndex;
		EXPECT_NE(r.readyNs, 0u) << r.frameIndex;
	}
	EXPECT_TRUE(f.records[0].flags & kFrameAppPresentId);
	EXPECT_FALSE(f.records[4].flags & kFrameAppPresentId) << "an injected id is the layer's, not the app's";
}

TEST(Broker, InjectionStopsWhenAnAppIdIsNotAboveTheLayers)
{
	// An app whose own ids would fall at or below the layer's: the layer cannot fix that present, but it must
	// not make it worse, so it stops injecting on this swapchain.
	Fixture f(IdMode::App);
	EXPECT_TRUE(f.present(0).plan.inject);   // layer id 1
	EXPECT_TRUE(f.present(0).plan.inject);   // layer id 2
	auto r = f.present(2);                    // the app's id 2: not above the layer's 2
	EXPECT_EQ(r.id, 2u);
	auto after = f.present(0);
	EXPECT_FALSE(after.plan.inject);
	EXPECT_FALSE(after.plan.timingRequested) << "no id, no layer request";
	EXPECT_EQ(f.present(9).id, 9u) << "the app's own ids still get timing";
}

TEST(Broker, CancelledInjectedPresentGivesItsIdBack)
{
	Fixture f(IdMode::App);
	PresentParams p;
	p.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	PresentPlan a = f.broker->beginPresent(p);
	ASSERT_TRUE(a.inject);
	EXPECT_EQ(a.injectedId, 1u);
	f.broker->cancelPresent(a.token);
	// The app's id 1 is now above everything injected and sent.
	auto r = f.present(1);
	EXPECT_EQ(r.id, 1u);
	EXPECT_TRUE(f.present(0).plan.inject) << "injection still on: the cancelled id never reached the driver";
}

TEST(Broker, IdLessSwapchainsGetNoLayerTiming)
{
	// Without ids a report cannot be attributed to its present once one was given up on, so the layer
	// does not ask for display timing there (the app's own requests are matched by order, see
	// BrokerApp.ZeroIdPresentsOfAnAppUseDriverOrder).
	Fixture f(IdMode::None);
	auto r = f.present();
	EXPECT_FALSE(r.plan.timingRequested);
	EXPECT_TRUE(f.driver.queue.empty());
	ASSERT_EQ(f.records.size(), 1u);
	EXPECT_FALSE(f.records[0].flags & kFrameTimingRequested);
	EXPECT_EQ(f.broker->stats().driverOutstanding, 0u);
}

TEST(Broker, RequestDequeuedIsNotAScreenStage)
{
	// A surface that offers QUEUE_OPERATIONS_END + REQUEST_DEQUEUED but no screen stage: the frames are
	// shown, so they must not be flagged NotDisplayed just because no screen time can arrive.
	Fixture f(IdMode::Layer, QOE | DEQ);
	f.present();
	f.driver.report(1, QOE, 20'000'000);
	f.driver.report(1, DEQ, 21'000'000, true);
	f.broker->poll();
	ASSERT_EQ(f.records.size(), 1u);
	EXPECT_TRUE(f.records[0].flags & kFrameTimingComplete);
	EXPECT_FALSE(f.records[0].flags & kFrameNotDisplayed);
	EXPECT_FALSE(f.records[0].flags & kFrameDisplayed);
	EXPECT_EQ(f.records[0].latchNs, 21'000'000u);
	EXPECT_EQ(f.records[0].screenNs, 0u);
}

TEST(Broker, ChangedTimeDomainsMoveTheLayersDomainIdWithoutTheAppPolling)
{
	Fixture f;
	auto first = f.present();
	EXPECT_EQ(first.plan.timing.timeDomainId, 7u);
	// The window moves to another display: new domain ids, counter bumps. Only the layer polls.
	f.driver.domainId = 9;
	f.driver.domainsCounter = 2;
	f.driver.finish(first.id, 20'000'000, 30'000'000);
	f.broker->poll();
	EXPECT_GE(f.driver.refreshCalls, 1);
	auto second = f.present();
	EXPECT_EQ(second.plan.timing.timeDomainId, 9u) << "the new domain id is sent";
	int calls = f.driver.refreshCalls;
	f.driver.finish(second.id, 21'000'000, 31'000'000);
	f.broker->poll();
	EXPECT_EQ(f.driver.refreshCalls, calls) << "an unchanged counter does not re-query";
}

TEST(Broker, OverlayResultIsPatchedIntoTheRecord)
{
	Fixture f;
	PresentParams p;
	p.presentStartNs = f.driver.now;
	PresentPlan plan = f.broker->beginPresent(p);
	f.broker->noteOverlay(plan.token, true, 1234);
	f.broker->endPresent(plan.token, f.driver.now + 5, f.driver.present(plan.injectedId, plan.timing));
	f.driver.finish(plan.injectedId, 20'000'000, 30'000'000);
	f.broker->poll();
	ASSERT_EQ(f.records.size(), 1u);
	EXPECT_TRUE(f.records[0].flags & kFrameOverlayDrawn);
	EXPECT_EQ(f.records[0].overlayCpuNs, 1234u);
}

TEST(Broker, FlushWaitsWithoutHoldingTheLock)
{
	Fixture f;
	f.driver.tick = 1'000'000;  // 1 ms of fake time per clock read: flush(40) gives up after ~40 iterations
	f.present();                 // never reported
	std::atomic<bool> started{false}, done{false};
	std::thread t([&] {
		started = true;
		f.broker->flush(40);
		done = true;
	});
	while (!started)
		std::this_thread::yield();
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	f.broker->stats();  // would wait for the whole flush if it held the lock while sleeping
	const bool flushStillRunning = !done;
	t.join();
	EXPECT_TRUE(flushStillRunning);
	ASSERT_EQ(f.records.size(), 1u);
	EXPECT_TRUE(f.records[0].flags & kFrameTimingLost);
}

TEST(Broker, FailedPresentIsEmittedWithoutWaitingForFeedback)
{
	Fixture f;
	f.driver.queueSize = 0;  // the driver refuses timed presents
	auto r = f.present();
	EXPECT_EQ(r.result, VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT);
	ASSERT_EQ(f.records.size(), 1u);
	EXPECT_TRUE(f.records[0].flags & kFramePresentFailed);
	EXPECT_EQ(f.broker->stats().driverOutstanding, 0u);
}

TEST(Broker, CalibrationIsCachedAndRefreshedOncePerSecond)
{
	Fixture f;
	for (uint64_t id = 1; id <= 20; id++)
	{
		f.present();
		f.driver.finish(id, f.driver.now + 1000, f.driver.now + 2000);
		f.broker->poll();
	}
	// Two stages with a local-domain clock each: one calibration per stage, not per record.
	EXPECT_EQ(f.driver.calibrateCalls, 2);
	f.driver.now += 1'500'000'000;
	f.present();
	f.driver.finish(21, f.driver.now + 1000, f.driver.now + 2000);
	f.broker->poll();
	EXPECT_EQ(f.driver.calibrateCalls, 4);
	EXPECT_EQ(f.records.size(), 21u);
}

TEST(Broker, MonotonicDomainNeedsNoCalibration)
{
	Fixture f;
	f.driver.domain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
	f.driver.domainOffset = 0;
	f.present();
	f.driver.finish(1, 20'000'000, 30'000'000);
	f.broker->poll();
	ASSERT_EQ(f.records.size(), 1u);
	EXPECT_EQ(f.records[0].screenNs, 30'000'000u);
	EXPECT_EQ(f.driver.calibrateCalls, 0);
}

TEST(Broker, FailedCalibrationDropsTimesButKeepsRecord)
{
	Fixture f;
	f.driver.failCalibration = true;
	f.present();
	f.driver.finish(1, 20'000'000, 30'000'000);
	f.broker->poll();
	ASSERT_EQ(f.records.size(), 1u);
	EXPECT_EQ(f.records[0].readyNs, 0u);
	EXPECT_EQ(f.records[0].screenNs, 0u);
}

TEST(Broker, FlushEmitsEverythingUnfinishedAsLost)
{
	Fixture f;
	for (int i = 0; i < 3; i++)
		f.present();
	f.driver.finish(2, 20'000'000, 30'000'000);
	f.broker->flush();
	ASSERT_EQ(f.records.size(), 3u);
	EXPECT_TRUE(f.records[0].flags & kFrameTimingLost);
	EXPECT_TRUE(f.records[1].flags & kFrameTimingComplete);
	EXPECT_TRUE(f.records[2].flags & kFrameTimingLost);
	EXPECT_EQ(f.records[2].frameIndex, 3u);
	EXPECT_EQ(f.broker->stats().entries, 0u);
}

TEST(Broker, OurBudgetNeverFillsTheDriverQueue)
{
	// Nothing ever completes: the layer must stop asking before the driver would refuse a present.
	Fixture f(IdMode::Layer, QOE | FPO, 6);
	f.setAppQueue(4);
	for (int i = 0; i < 40; i++)
	{
		f.driver.now += 1'000'000;
		auto r = f.present();
		EXPECT_EQ(r.result, VK_SUCCESS) << "present " << i;
	}
	EXPECT_LE(f.driver.queue.size(), f.driver.queueSize);
	EXPECT_GT(f.broker->stats().droppedLayerTiming, 0u);
}

TEST(Broker, TimeDomainFollowsReports)
{
	// The app's request wins over ours, reports may come back in another domain; the record uses the report's.
	Fixture f;
	f.setAppQueue(4);
	auto info = appInfo(FPO, 99);
	f.present(0, &info);
	EXPECT_EQ(f.driver.queue[0].domainId, 99u) << "the app's time domain id is kept";
	EXPECT_EQ(f.driver.queue[0].requested, uint32_t(FPO | QOE)) << "stages merged";
}

// ---------------------------------------------------------------------------------------
// App side: the app must see what it would see without the layer
// ---------------------------------------------------------------------------------------

namespace {

// Runs the same script against (a) the driver directly and (b) the broker with the layer requesting its
// own stages on top. Both return the app's views, one per poll.
struct Script
{
	enum Kind { Present, Finish, Partial, Poll, CountOnly, Advance, SetQueue } kind;
	uint64_t id = 0;
	uint32_t a = 0, b = 0, c = 0;
};

std::vector<std::string> runReference(const std::vector<Script> &script, uint32_t appQueue)
{
	FakeDriver d;
	d.queueSize = appQueue;
	std::vector<std::string> out;
	for (const Script &s : script)
	{
		switch (s.kind)
		{
		case Script::Present:
		{
			auto t = appInfo(s.a);
			VkResult r = d.present(s.id, t);
			if (r != VK_SUCCESS)
				out.push_back("present rc=" + std::to_string(r));
			break;
		}
		case Script::Finish:
			if (d.find(s.id)) d.finish(s.id, s.a, s.b);
			break;
		case Script::Partial:
			if (d.find(s.id)) d.report(s.id, s.a, s.b);
			break;
		case Script::Poll:
			out.push_back(pollDriver(d, s.a, s.b, s.c ? s.c : 4).text);
			break;
		case Script::CountOnly:
			out.push_back(pollDriver(d, s.a, s.b, 4, true).text);
			break;
		case Script::Advance:
			d.now += s.a;
			break;
		case Script::SetQueue:
			break;
		}
	}
	return out;
}

std::vector<std::string> runLayered(const std::vector<Script> &script, uint32_t appQueue, std::vector<FrameRecord> *records = nullptr)
{
	Fixture f(IdMode::App, QOE | FPO, 16);
	f.setAppQueue(appQueue);
	std::vector<std::string> out;
	for (const Script &s : script)
	{
		switch (s.kind)
		{
		case Script::Present:
		{
			auto t = appInfo(s.a);
			auto r = f.present(s.id, &t);
			if (r.result != VK_SUCCESS)
				out.push_back("present rc=" + std::to_string(r.result));
			break;
		}
		case Script::Finish:
			if (f.driver.find(s.id)) f.driver.finish(s.id, s.a, s.b);
			break;
		case Script::Partial:
			if (f.driver.find(s.id)) f.driver.report(s.id, s.a, s.b);
			break;
		case Script::Poll:
			out.push_back(pollBroker(f, s.a, s.b, s.c ? s.c : 4).text);
			break;
		case Script::CountOnly:
			out.push_back(pollBroker(f, s.a, s.b, 4, true).text);
			break;
		case Script::Advance:
			f.driver.now += s.a;
			break;
		case Script::SetQueue:
			break;
		}
	}
	if (records)
		*records = f.records;
	return out;
}

// Times in the script are MONOTONIC; both fakes use the same domain offset, so the app-visible
// (driver-domain) numbers are comparable.
void expectSame(const std::vector<Script> &script, uint32_t appQueue)
{
	auto ref = runReference(script, appQueue);
	auto got = runLayered(script, appQueue);
	ASSERT_EQ(ref.size(), got.size());
	for (size_t i = 0; i < ref.size(); i++)
		EXPECT_EQ(ref[i], got[i]) << "app view " << i;
}

} // namespace

TEST(BrokerApp, InOrderSingleStage)
{
	// DXVK style: one stage (FIRST_PIXEL_OUT), poll every frame, capacity = queue size.
	std::vector<Script> s;
	for (uint64_t id = 1; id <= 6; id++)
	{
		s.push_back({Script::Present, id, FPO});
		s.push_back({Script::Poll, 0, 8, 0, 1});
		if (id > 2)
			s.push_back({Script::Finish, id - 2, uint32_t(1'000'000 * id), uint32_t(2'000'000 * id)});
	}
	expectSame(s, 8);
}

TEST(BrokerApp, AppPollsBeforeAndAfterTheLayer)
{
	std::vector<Script> s;
	s.push_back({Script::Present, 1, FPO});
	s.push_back({Script::Present, 2, FPO});
	s.push_back({Script::Present, 3, FPO});
	s.push_back({Script::Finish, 1, 1'000'000, 2'000'000});
	s.push_back({Script::Poll, 0, 4, 0});       // app polls: gets 1
	s.push_back({Script::Present, 4, FPO});      // layer polls in beginPresent: drains the driver
	s.push_back({Script::Finish, 3, 3'000'000, 4'000'000});
	s.push_back({Script::Present, 5, FPO});      // layer polls and sees 3 completed before 2
	s.push_back({Script::Poll, 0, 4, 0});        // in order: 2 not done -> nothing
	s.push_back({Script::Finish, 2, 5'000'000, 6'000'000});
	s.push_back({Script::Poll, 0, 4, 0});        // now 2 and 3
	s.push_back({Script::Poll, 0, 4, 0});
	expectSame(s, 8);
}

TEST(BrokerApp, OutOfOrderFlagReturnsReadyOnes)
{
	std::vector<Script> s;
	for (uint64_t id = 1; id <= 4; id++)
		s.push_back({Script::Present, id, FPO});
	s.push_back({Script::Finish, 3, 1'000'000, 2'000'000});
	s.push_back({Script::Poll, 0, 4, VK_PAST_PRESENTATION_TIMING_ALLOW_OUT_OF_ORDER_RESULTS_BIT_EXT});
	s.push_back({Script::Poll, 0, 4, 0});
	s.push_back({Script::Finish, 1, 3'000'000, 4'000'000});
	s.push_back({Script::Poll, 0, 4, 0});
	expectSame(s, 8);
}

TEST(BrokerApp, IncompleteAndCountOnly)
{
	std::vector<Script> s;
	for (uint64_t id = 1; id <= 5; id++)
	{
		s.push_back({Script::Present, id, FPO});
		s.push_back({Script::Finish, id, uint32_t(id * 1'000'000), uint32_t(id * 2'000'000)});
	}
	s.push_back({Script::CountOnly, 0, 8, 0});
	s.push_back({Script::Poll, 0, 2, 0});  // 5 ready, room for 2: VK_INCOMPLETE
	s.push_back({Script::Poll, 0, 2, 0});
	s.push_back({Script::Poll, 0, 2, 0});
	s.push_back({Script::Poll, 0, 2, 0});
	expectSame(s, 8);
}

TEST(BrokerApp, StageCountQueryAndTooSmallStageArray)
{
	Fixture f;
	f.setAppQueue(4);
	auto t = appInfo(QOE | FPO);
	f.present(5, &t);
	f.driver.finish(5, 1'000'000, 2'000'000);
	f.broker->poll();

	// First pass of the two-call idiom: no stage array, the entry stays for the second call.
	auto query = [&](VkPastPresentationTimingEXT &el, uint32_t stageCapacity, VkPresentStageTimeEXT *stages) {
		el = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT};
		el.pPresentStages = stages;
		el.presentStageCount = stageCapacity;
		VkPastPresentationTimingInfoEXT info = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT};
		VkPastPresentationTimingPropertiesEXT props = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT};
		props.presentationTimingCount = 1;
		props.pPresentationTimings = &el;
		VkResult r = f.broker->appGetPast(&info, &props);
		return std::make_pair(r, props.presentationTimingCount);
	};
	VkPastPresentationTimingEXT el;
	auto r = query(el, 4, nullptr);
	EXPECT_EQ(r.first, VK_SUCCESS);
	EXPECT_EQ(r.second, 1u);
	EXPECT_EQ(el.presentStageCount, 2u) << "the count the second pass needs";
	EXPECT_EQ(f.broker->stats().appOutstanding, 1u) << "not consumed by a count-only pass";

	// A stage array that is too small: VK_INCOMPLETE, the stages that fit are written.
	VkPresentStageTimeEXT one[1] = {};
	r = query(el, 1, one);
	EXPECT_EQ(r.first, VK_INCOMPLETE);
	EXPECT_EQ(el.presentStageCount, 1u);
	EXPECT_EQ(f.broker->stats().appOutstanding, 0u);
}

TEST(BrokerApp, OnlyRequestedStagesAreReturned)
{
	// The app asks for FIRST_PIXEL_OUT only although the layer also requests QUEUE_OPERATIONS_END.
	std::vector<Script> s;
	s.push_back({Script::Present, 1, FPO});
	s.push_back({Script::Present, 2, QOE | FPO});
	s.push_back({Script::Finish, 1, 1'000'000, 2'000'000});
	s.push_back({Script::Finish, 2, 3'000'000, 4'000'000});
	s.push_back({Script::Poll, 0, 4, 0});
	expectSame(s, 8);
}

TEST(BrokerApp, ReplacedImageReportsZeroTimeLikeTheDriver)
{
	std::vector<Script> s;
	s.push_back({Script::Present, 1, FPO});
	s.push_back({Script::Finish, 1, 1'000'000, 0});
	s.push_back({Script::Poll, 0, 4, 0});
	expectSame(s, 8);
}

TEST(BrokerApp, PartialResultsFlag)
{
	std::vector<Script> s;
	s.push_back({Script::Present, 1, QOE | FPO});
	s.push_back({Script::Partial, 1, QOE, 1'000'000});
	s.push_back({Script::Poll, 0, 4, 0});                                              // not complete: nothing
	s.push_back({Script::Poll, 0, 4, VK_PAST_PRESENTATION_TIMING_ALLOW_PARTIAL_RESULTS_BIT_EXT});  // partial entry
	s.push_back({Script::Poll, 0, 4, VK_PAST_PRESENTATION_TIMING_ALLOW_PARTIAL_RESULTS_BIT_EXT});  // nothing new
	s.push_back({Script::Partial, 1, FPO, 2'000'000});
	s.push_back({Script::Poll, 0, 4, VK_PAST_PRESENTATION_TIMING_ALLOW_PARTIAL_RESULTS_BIT_EXT});  // new stage
	s.push_back({Script::Poll, 0, 4, 0});
	expectSame(s, 8);
}

TEST(BrokerApp, TargetTimeAndTimeDomainAreEchoed)
{
	Fixture f(IdMode::App);
	f.setAppQueue(4);
	auto t = appInfo(FPO, 7, 123456789);
	f.present(5, &t);
	f.driver.finish(5, 1'000'000, 2'000'000);
	auto v = pollBroker(f, 4, 0);
	EXPECT_NE(v.text.find("tgt=123456789"), std::string::npos) << v.text;
}

TEST(BrokerApp, CountersComeFromTheDriver)
{
	Fixture f(IdMode::App);
	f.setAppQueue(4);
	f.driver.propsCounter = 3;
	f.driver.domainsCounter = 9;
	auto v = pollBroker(f, 4, 0);
	EXPECT_EQ(v.propsCounter, 3u);
	EXPECT_EQ(v.domainsCounter, 9u);
	EXPECT_EQ(v.count, 0u);
	EXPECT_EQ(v.result, VK_SUCCESS);
}

TEST(BrokerApp, QueueFullIsEmulatedAtTheAppsSize)
{
	// App size 3, never polls. Same present results as without the layer: ok,ok,ok,full,...
	std::vector<Script> s;
	for (uint64_t id = 1; id <= 6; id++)
		s.push_back({Script::Present, id, FPO});
	expectSame(s, 3);
	auto got = runLayered(s, 3);
	ASSERT_EQ(got.size(), 3u);
	EXPECT_EQ(got[0], "present rc=" + std::to_string(VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT));
}

TEST(BrokerApp, QueueFullWhenAppNeverSetASize)
{
	Fixture f(IdMode::App);
	auto t = appInfo(FPO);
	auto r = f.present(1, &t);
	EXPECT_EQ(r.result, VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT);
}

TEST(BrokerApp, SlotsFreeUpWhenTheAppCollects)
{
	Fixture f(IdMode::App);
	f.setAppQueue(2);
	auto t = appInfo(FPO);
	EXPECT_EQ(f.present(1, &t).result, VK_SUCCESS);
	EXPECT_EQ(f.present(2, &t).result, VK_SUCCESS);
	EXPECT_EQ(f.present(3, &t).result, VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT);
	f.driver.finish(1, 1'000'000, 2'000'000);
	EXPECT_EQ(f.present(4, &t).result, VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT) << "completed but not collected: still full";
	pollBroker(f, 2, 0);
	EXPECT_EQ(f.present(5, &t).result, VK_SUCCESS);
}

TEST(BrokerApp, AppThatNeverPollsDoesNotGrowTheCache)
{
	// The app requests timing but never reads it back; the driver completes everything. Memory stays
	// bounded by the app's own queue size (it gets QUEUE_FULL like it would without the layer), and the
	// layer's records keep flowing.
	Fixture f(IdMode::App);
	f.setAppQueue(8);
	auto t = appInfo(FPO);
	uint64_t id = 0;
	for (int i = 0; i < 2000; i++)
	{
		f.driver.now += 4'000'000;
		auto r = f.present(++id, &t);
		if (r.result == VK_SUCCESS)
			f.driver.finish(id, f.driver.now + 1000, f.driver.now + 2000);
		EXPECT_LE(f.broker->stats().entries, 8u + 1u);
	}
	BrokerStats s = f.broker->stats();
	EXPECT_LE(s.entries, 9u);
	EXPECT_LE(s.appOutstanding, 8u);
	EXPECT_EQ(f.records.size(), 2000u) << "one record per present, even when the app never collects";
}

TEST(BrokerApp, LayerRecordsStillArriveWhenAppDoesPollsEveryFrame)
{
	std::vector<Script> s;
	for (uint64_t id = 1; id <= 10; id++)
	{
		s.push_back({Script::Present, id, FPO});
		s.push_back({Script::Finish, id, uint32_t(id * 1'000'000), uint32_t(id * 2'000'000)});
		s.push_back({Script::Poll, 0, 4, 0});
	}
	std::vector<FrameRecord> records;
	runLayered(s, 4, &records);
	EXPECT_GE(records.size(), 9u);
	for (const FrameRecord &r : records)
		EXPECT_TRUE(r.flags & (kFrameTimingComplete | kFrameTimingLost));
}

TEST(BrokerApp, ZeroIdPresentsOfAnAppUseDriverOrder)
{
	// An app that uses timing without ids: reports carry id 0 and arrive in order.
	Fixture f(IdMode::None);
	f.setAppQueue(4);
	auto t = appInfo(FPO);
	f.present(0, &t);
	f.present(0, &t);
	f.driver.finish(0, 1'000'000, 2'000'000);
	auto v = pollBroker(f, 4, 0);
	EXPECT_EQ(v.count, 1u);
	f.driver.finish(0, 3'000'000, 4'000'000);
	v = pollBroker(f, 4, 0);
	EXPECT_EQ(v.count, 1u);
}

TEST(BrokerApp, LayerInjectedIdsAreInvisibleToTheApp)
{
	// Layer mode: the app has no id stream; its reports must show presentId 0 as without the layer.
	Fixture f(IdMode::Layer);
	f.setAppQueue(4);
	auto t = appInfo(FPO);
	auto r = f.present(0, &t);
	EXPECT_TRUE(r.plan.inject);
	f.driver.finish(r.id, 1'000'000, 2'000'000);
	auto v = pollBroker(f, 4, 0);
	EXPECT_NE(v.text.find("[id=0 "), std::string::npos) << v.text;
}

TEST(BrokerApp, DriverErrorsAreForwarded)
{
	struct Failing : FakeDriver
	{
		VkResult getPast(VkPastPresentationTimingFlagsEXT, VkPastPresentationTimingPropertiesEXT *) override
		{
			return VK_ERROR_SURFACE_LOST_KHR;
		}
	};
	Failing d;
	BrokerConfig cfg;
	cfg.idMode = IdMode::App;
	TimingBroker b(cfg, d, nullptr);
	auto v = appPoll([&](auto *i, auto *p) { return b.appGetPast(i, p); }, 4, 0);
	EXPECT_EQ(v.result, VK_ERROR_SURFACE_LOST_KHR);
}

// ---------------------------------------------------------------------------------------
// Several swapchains, recreation
// ---------------------------------------------------------------------------------------

TEST(BrokerMulti, SwapchainsAreIndependent)
{
	Fixture a, b;
	for (int i = 0; i < 3; i++)
	{
		a.present();
		b.present();
		b.present();
	}
	for (uint64_t id = 1; id <= 3; id++)
		a.driver.finish(id, 1'000'000 * id, 2'000'000 * id);
	a.broker->poll();
	b.broker->poll();
	EXPECT_EQ(a.records.size(), 3u);
	EXPECT_EQ(b.records.size(), 0u);
	for (uint64_t id = 1; id <= 6; id++)
		b.driver.finish(id, 1'000'000 * id, 2'000'000 * id);
	b.broker->poll();
	EXPECT_EQ(b.records.size(), 6u);
}

TEST(BrokerMulti, RecreationWithOldSwapchainFlushesTheRetiredOne)
{
	Fixture oldChain;
	oldChain.present();
	oldChain.present();
	oldChain.driver.finish(1, 1'000'000, 2'000'000);
	oldChain.broker->poll();
	ASSERT_EQ(oldChain.records.size(), 1u);

	// App creates a new swapchain (oldSwapchain = old) and destroys the old one later.
	Fixture newChain;
	newChain.present();
	newChain.driver.finish(1, 5'000'000, 6'000'000);
	newChain.broker->poll();
	EXPECT_EQ(newChain.records.size(), 1u);
	EXPECT_EQ(newChain.records[0].frameIndex, 1u);

	oldChain.broker->flush();
	ASSERT_EQ(oldChain.records.size(), 2u);
	EXPECT_TRUE(oldChain.records[1].flags & kFrameTimingLost);
	EXPECT_EQ(oldChain.records[1].frameIndex, 2u);
}

// ---- round 3 -----------------------------------------------------------------------------------------

// A failed time-domain query is tried again on the next present instead of keeping the stale id for good.
TEST(Broker, FailedDomainRefreshIsRetried)
{
	Fixture f;
	auto first = f.present();
	f.driver.domainId = 9;
	f.driver.domainsCounter = 2;
	f.driver.failRefresh = true;
	f.driver.finish(first.id, f.driver.now + 1'000'000, f.driver.now + 4'000'000);
	f.broker->poll();
	EXPECT_EQ(f.driver.refreshCalls, 1);
	auto second = f.present();
	EXPECT_EQ(f.driver.refreshCalls, 2) << "the next present tries again";
	EXPECT_EQ(second.plan.timing.timeDomainId, 7u) << "the old id until a refresh succeeds";
	f.driver.failRefresh = false;
	auto third = f.present();
	EXPECT_EQ(third.plan.timing.timeDomainId, 9u);
}

// vkGetSwapchainTimeDomainPropertiesEXT needs the swapchain externally synchronized, which the app's
// vkGetPastPresentationTimingEXT does not give: the change is only noted there, the next present queries.
TEST(Broker, AppPollDoesNotQueryTimeDomains)
{
	Fixture f;
	f.setAppQueue(4);
	VkPresentTimingInfoEXT t = appInfo(QOE | FPO);
	auto first = f.present(0, &t);
	f.driver.domainId = 9;
	f.driver.domainsCounter = 2;
	f.driver.finish(first.id, f.driver.now + 1'000'000, f.driver.now + 4'000'000);
	AppView v = pollBroker(f, 4, 0);
	EXPECT_EQ(v.domainsCounter, 2u) << "the app still sees the new counter";
	EXPECT_EQ(f.driver.refreshCalls, 0);
	auto second = f.present();
	EXPECT_EQ(f.driver.refreshCalls, 1);
	EXPECT_EQ(second.plan.timing.timeDomainId, 9u);
}

// After a domain change, a frame still reported in the retired id keeps that id's calibration: the zero
// reading the driver may return for a retired id is not a calibration (it would put the frame at +uptime).
TEST(Broker, RetiredDomainIdKeepsItsCalibrationForFramesInFlight)
{
	Fixture f;
	f.driver.zeroForRetiredIds = true;
	auto warm = f.present();
	f.driver.finish(warm.id, f.driver.now + 1'000'000, f.driver.now + 4'000'000);
	f.broker->poll();
	ASSERT_EQ(f.records.size(), 1u);
	auto inFlight = f.present();   // requested in domain id 7
	ASSERT_EQ(inFlight.plan.timing.timeDomainId, 7u);
	const uint64_t t = f.driver.now;
	f.driver.domainId = 9;         // the window moved: id 7 is retired
	f.driver.domainsCounter = 2;
	f.driver.now += 2'000'000'000; // past the calibration interval: a recalibration is attempted
	f.driver.finish(inFlight.id, t + 1'000'000, t + 4'000'000);
	f.broker->poll();
	ASSERT_EQ(f.records.size(), 2u);
	EXPECT_EQ(f.records[1].readyNs, t + 1'000'000);
	EXPECT_EQ(f.records[1].screenNs, t + 4'000'000);
}

// Swapchain destruction: no 30 ms wait when the driver reports an error, nor for presents long gone.
TEST(Broker, FlushStopsWaitingWhenTheDriverReportsAnError)
{
	Fixture f;
	f.driver.tick = 1'000'000;
	f.present();   // never reported
	f.driver.getPastError = VK_ERROR_SURFACE_LOST_KHR;
	const int before = f.driver.getPastCalls;
	f.broker->flush(40);
	EXPECT_LE(f.driver.getPastCalls - before, 1);
	ASSERT_EQ(f.records.size(), 1u);
	EXPECT_TRUE(f.records[0].flags & kFrameTimingLost);
}

TEST(Broker, FlushDoesNotWaitForPresentsLongGone)
{
	Fixture f;
	f.driver.tick = 1'000'000;
	f.present();                     // never reported
	f.driver.now += 5'000'000'000;   // the swapchain is destroyed 5 s after its last present
	const int before = f.driver.getPastCalls;
	f.broker->flush(40);
	EXPECT_LE(f.driver.getPastCalls - before, 1);
	ASSERT_EQ(f.records.size(), 1u);
	EXPECT_TRUE(f.records[0].flags & kFrameTimingLost);
}

// Multi-swapchain presents where only some swapchains hit the app's limit are planned again without the
// limit: cancelPresent undoes the first registration completely.
TEST(Broker, CancelledPresentIsPlannedAgainWithoutTheLimit)
{
	Fixture f;
	f.setAppQueue(1);
	VkPresentTimingInfoEXT t = appInfo(QOE | FPO);
	auto held = f.present(0, &t);   // takes the app's one slot
	ASSERT_EQ(held.result, VK_SUCCESS);
	PresentParams p;
	p.presentStartNs = f.driver.now;
	p.appTiming = &t;
	PresentPlan forced = f.broker->beginPresent(p);
	ASSERT_EQ(forced.forcedResult, VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT);
	const BrokerStats before = f.broker->stats();
	f.broker->cancelPresent(forced.token);
	const BrokerStats after = f.broker->stats();
	EXPECT_EQ(after.entries, before.entries - 1);
	EXPECT_EQ(after.driverOutstanding, before.driverOutstanding - (forced.timingRequested ? 1u : 0u));
	p.noLimit = true;
	PresentPlan again = f.broker->beginPresent(p);
	EXPECT_EQ(again.forcedResult, VK_SUCCESS);
	EXPECT_EQ(again.appStagesSent, uint32_t(QOE | FPO));
	EXPECT_EQ(again.token, forced.token) << "frame numbering stays contiguous";
	const uint64_t id = again.inject ? again.injectedId : 0;
	f.broker->endPresent(again.token, f.driver.now + 10, f.driver.present(id, again.timing));
	f.driver.finish(held.id, f.driver.now + 1'000'000, f.driver.now + 4'000'000);
	f.driver.finish(id, f.driver.now + 2'000'000, f.driver.now + 5'000'000);
	f.broker->poll();
	EXPECT_EQ(f.records.size(), 2u) << "one record per present that went down";
	EXPECT_EQ(f.broker->stats().appOutstanding, 2u) << "both app requests are waiting for the app";
}

// A present whose other swapchains lack PRESENT_ID_2 cannot carry an injected id: no layer request then (it
// could not be matched), and every entry still names a valid time domain.
TEST(Broker, NoInjectedIdWhenThePresentCannotCarryOne)
{
	Fixture f;   // IdMode::Layer
	PresentParams p;
	p.presentStartNs = f.driver.now;
	p.canInjectId = false;
	PresentPlan plan = f.broker->beginPresent(p);
	EXPECT_FALSE(plan.inject);
	EXPECT_EQ(plan.ourStagesSent, 0u);
	EXPECT_FALSE(plan.timingRequested);
	EXPECT_EQ(plan.timing.timeDomainId, 7u) << "an empty entry carries a domain id the driver returned";
	f.broker->endPresent(plan.token, f.driver.now + 10, VK_SUCCESS);
	f.broker->poll();
	ASSERT_EQ(f.records.size(), 1u) << "the CPU-side record is still written";
	EXPECT_FALSE(f.records[0].flags & kFrameTimingRequested);
}
