// SPDX-License-Identifier: MIT
// Record -> FrameData adapter: the three display cases, field mapping, metadata helpers.
#include "test_util.hpp"

#include <vulkan/vulkan_core.h>
#include <gtest/gtest.h>

#include <cmath>

using namespace pml::metrics;
using namespace pmltest;

// The numeric present mode constants must be the Vulkan ones.
static_assert(kVkPresentModeImmediate == VK_PRESENT_MODE_IMMEDIATE_KHR);
static_assert(kVkPresentModeMailbox == VK_PRESENT_MODE_MAILBOX_KHR);
static_assert(kVkPresentModeFifo == VK_PRESENT_MODE_FIFO_KHR);
static_assert(kVkPresentModeFifoRelaxed == VK_PRESENT_MODE_FIFO_RELAXED_KHR);
static_assert(kVkPresentModeFifoLatestReady == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR);
static_assert(kVkPresentModeSharedDemandRefresh == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR);
static_assert(kVkPresentModeSharedContinuousRefresh == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR);

namespace {
SwapchainContext ctx() { return SwapchainContext{ 4242, "DXVK", 0 }; }
}

TEST(Adapter, DisplayedPresent)
{
	auto r = displayed(makeRecord(7, kT0 + 5 * kMs, 300000), kT0 + 6 * kMs, kT0 + 20 * kMs);
	EXPECT_EQ(classifyDisplay(r), DisplayState::Displayed);
	FrameData f = toFrameData(r, ctx());
	EXPECT_EQ(f.finalState, PresentResult::Presented);
	ASSERT_EQ(f.displayed.Size(), 1u);
	EXPECT_EQ(f.displayed[0].first, FrameType::Application);
	EXPECT_EQ(f.displayed[0].second, kT0 + 20 * kMs);
	EXPECT_EQ(f.presentStartTime, kT0 + 5 * kMs);
	EXPECT_EQ(f.timeInPresent, 300000u);
	EXPECT_EQ(f.readyTime, kT0 + 6 * kMs);
	EXPECT_EQ(f.processId, 4242u);
	EXPECT_EQ(f.swapChainAddress, 0xABCDu);
	EXPECT_EQ(f.frameId, 7u);
	EXPECT_EQ(f.syncInterval, 1);
	EXPECT_FALSE(f.supportsTearing);
}

// (b) A present known not to reach the screen is dropped.
TEST(Adapter, NotDisplayedPresentIsDropped)
{
	auto r = notDisplayed(makeRecord(3, kT0, 100000, kVkPresentModeMailbox), kT0 + kMs);
	EXPECT_EQ(classifyDisplay(r), DisplayState::NotDisplayed);
	FrameData f = toFrameData(r, ctx());
	EXPECT_EQ(f.finalState, PresentResult::Discarded);
	EXPECT_TRUE(f.displayed.Empty());
	EXPECT_EQ(f.syncInterval, 0);

	// Through the engine: display columns zero (CSV "NA"), counted as dropped.
	SwapchainEngine e(ctx(), MetricsMode::V2, kT0);
	std::vector<pml::FrameRecord> recs;
	for (uint64_t i = 1; i <= 4; ++i)
		recs.push_back(notDisplayed(makeRecord(i, kT0 + i * 2 * kMs, 100000, kVkPresentModeMailbox), kT0 + i * 2 * kMs + kMs));
	auto rows = feedAll(e, recs);
	ASSERT_EQ(rows.size(), 3u);
	for (auto &row : rows) {
		EXPECT_TRUE(row.dropped);
		EXPECT_TRUE(row.metrics.isDroppedFrame);
		EXPECT_FALSE(row.hasDisplayMetrics);
		EXPECT_EQ(row.frame.finalState, PresentResult::Discarded);
		EXPECT_EQ(row.metrics.msUntilDisplayed, 0.0);
		EXPECT_EQ(row.metrics.screenTimeQpc, 0u);
		EXPECT_TRUE(row.isAppFrame);
		EXPECT_FALSE(IsMissingFrameMetricValue(row.metrics.msCPUBusy));
		EXPECT_STREQ(row.presentMode, "MAILBOX");
	}
}

// (c) No display information at all (Xwayland, timing lost, timing not requested).
TEST(Adapter, NoDisplayInfoIsNeitherDisplayedNorDropped)
{
	pml::FrameRecord lost = makeRecord(1, kT0, 100000);
	lost.flags = pml::kFrameTimingRequested | pml::kFrameTimingLost;
	pml::FrameRecord notRequested = makeRecord(2, kT0, 100000);
	pml::FrameRecord claimsDisplayedButNoTime = makeRecord(3, kT0, 100000);
	claimsDisplayedButNoTime.flags = pml::kFrameDisplayed;   // screenNs == 0
	for (auto *r : { &lost, &notRequested, &claimsDisplayedButNoTime }) {
		EXPECT_EQ(classifyDisplay(*r), DisplayState::Unknown);
		FrameData f = toFrameData(*r, ctx());
		EXPECT_EQ(f.finalState, PresentResult::Presented);   // CSV Dropped = 0
		EXPECT_TRUE(f.displayed.Empty());
	}

	// Through the engine, including QUEUE_OPERATIONS_END (readyNs) as Xwayland reports it.
	SwapchainEngine e(ctx(), MetricsMode::V2, kT0);
	std::vector<pml::FrameRecord> recs;
	for (uint64_t i = 1; i <= 5; ++i)
		recs.push_back(noInfo(makeRecord(i, kT0 + i * 4 * kMs, 500000), kT0 + i * 4 * kMs + kMs));
	auto rows = feedAll(e, recs, /*finish*/ false);
	ASSERT_EQ(rows.size(), 4u) << "no display info: rows are released immediately";
	for (auto &row : rows) {
		EXPECT_EQ(row.display, DisplayState::Unknown);
		EXPECT_FALSE(row.dropped);
		EXPECT_FALSE(row.metrics.isDroppedFrame);
		EXPECT_FALSE(row.hasDisplayMetrics);
		EXPECT_EQ(row.frame.finalState, PresentResult::Presented);
		// Display columns: 0 (the CSV prints NA for 0) or NaN, never a number.
		EXPECT_EQ(row.metrics.msUntilDisplayed, 0.0);
		EXPECT_EQ(row.metrics.msBetweenDisplayChange, 0.0);
		EXPECT_EQ(row.metrics.msDisplayedTime, 0.0);
		EXPECT_EQ(row.metrics.msDisplayLatency, 0.0);
		EXPECT_EQ(row.metrics.screenTimeQpc, 0u);
		EXPECT_TRUE(IsMissingFrameMetricValue(row.metrics.msReadyTimeToDisplayLatency));
		// CPU columns stay valid.
		EXPECT_NEAR(row.metrics.msCPUWait, 0.5, 1e-9);
		EXPECT_NEAR(row.metrics.msCPUBusy, 3.5, 1e-9);
		EXPECT_NEAR(row.metrics.msBetweenPresents, 4.0, 1e-9);
		EXPECT_NEAR(row.metrics.msUntilRenderComplete, 1.0, 1e-9);
	}
}

TEST(Adapter, FailedPresentsAreSkipped)
{
	SwapchainEngine e(ctx(), MetricsMode::V2, kT0);
	auto r1 = noInfo(makeRecord(1, kT0 + kMs, 1000));
	auto r2 = noInfo(makeRecord(2, kT0 + 2 * kMs, 1000));
	auto bad = noInfo(makeRecord(3, kT0 + 3 * kMs, 1000));
	bad.presentResult = -4;   // VK_ERROR_OUT_OF_DATE_KHR
	auto bad2 = noInfo(makeRecord(4, kT0 + 3 * kMs, 1000));
	bad2.flags |= pml::kFramePresentFailed;
	EXPECT_TRUE(isFailedPresent(bad));
	EXPECT_TRUE(isFailedPresent(bad2));
	EXPECT_TRUE(e.process(r1).empty());   // seed
	EXPECT_EQ(e.process(r2).size(), 1u);
	EXPECT_TRUE(e.process(bad).empty());
	EXPECT_TRUE(e.process(bad2).empty());
	EXPECT_EQ(e.recordsIgnored(), 2u);
	EXPECT_EQ(e.recordsProcessed(), 2u);
}

TEST(Adapter, FieldMapping)
{
	auto r = displayed(makeRecord(9, kT0 + kMs, 200000), kT0 + 2 * kMs, kT0 + 9 * kMs);
	r.gpuStartNs = kT0 + 1500000;
	r.gpuBusyNs = 700000;
	r.gpuVideoBusyNs = 11;
	r.latencyFrameId = 77;
	r.simStartNs = kT0 + 10;
	r.simEndNs = kT0 + 20;
	r.renderSubmitStartNs = kT0 + 30;
	r.renderSubmitEndNs = kT0 + 40;
	r.markerPresentStartNs = kT0 + 50;
	r.markerPresentEndNs = kT0 + 60;
	r.inputSampleNs = kT0 + 5;
	r.sleepStartNs = kT0 + 1;
	r.sleepEndNs = kT0 + 2;
	r.inputNs = kT0 + 3;
	r.mouseClickNs = kT0 + 4;
	r.pipelineCompileCount = 2;
	r.pipelineCompileBusyNs = 300;
	r.pipelineCompileSummedNs = 500;
	r.presentId = 5;
	FrameData f = toFrameData(r, ctx());
	EXPECT_EQ(f.gpuStartTime, kT0 + 1500000);
	EXPECT_EQ(f.gpuDuration, 700000u);
	EXPECT_EQ(f.gpuVideoDuration, 11u);
	EXPECT_EQ(f.appSimStartTime, kT0 + 10);
	EXPECT_EQ(f.pclSimStartTime, kT0 + 10);
	EXPECT_EQ(f.appSimEndTime, kT0 + 20);
	EXPECT_EQ(f.appRenderSubmitStartTime, kT0 + 30);
	EXPECT_EQ(f.appRenderSubmitEndTime, kT0 + 40);
	EXPECT_EQ(f.appPresentStartTime, kT0 + 50);
	EXPECT_EQ(f.appPresentEndTime, kT0 + 60);
	EXPECT_EQ(f.appInputSample.first, kT0 + 5);
	EXPECT_EQ(f.appInputSample.second, InputDeviceType::Unknown);
	EXPECT_EQ(f.appSleepStartTime, kT0 + 1);
	EXPECT_EQ(f.appSleepEndTime, kT0 + 2);
	EXPECT_EQ(f.inputTime, kT0 + 3);
	EXPECT_EQ(f.mouseClickTime, kT0 + 4);
	EXPECT_EQ(f.appFrameId, 77u);
	EXPECT_EQ(f.presentId, 5u);
	EXPECT_EQ(f.interFrameEventStats[0].activityCount, 2u);
	EXPECT_EQ(f.interFrameEventStats[0].busyQpc, 300u);
	EXPECT_EQ(f.interFrameEventStats[0].summedBusyQpc, 500u);

	auto zero = makeRecord(1, kT0, 0);
	FrameData z = toFrameData(zero, ctx());
	EXPECT_EQ(z.appInputSample.second, InputDeviceType::None);
	EXPECT_EQ(z.timeInPresent, 0u);

	// End before start must not wrap around.
	auto weird = makeRecord(1, kT0 + 10, 0);
	weird.presentEndNs = kT0;
	EXPECT_EQ(toFrameData(weird, ctx()).timeInPresent, 0u);
}

TEST(Adapter, QpcConverterUsesNanoseconds)
{
	auto q = makeQpcConverter(kT0);
	EXPECT_EQ(q.GetFrequency(), 1000000000ull);
	EXPECT_DOUBLE_EQ(q.DeltaUnsignedMilliSeconds(kT0, kT0 + 2500000), 2.5);
	EXPECT_DOUBLE_EQ(q.DeltaSignedMilliSeconds(kT0 + 2500000, kT0), -2.5);
	EXPECT_EQ(q.DeltaUnsignedMilliSeconds(0, kT0), 0.0);   // 0 means missing
}

TEST(Metadata, RuntimeName)
{
	EXPECT_STREQ(runtimeName("DXVK"), "DXVK");
	EXPECT_STREQ(runtimeName("dxvk"), "DXVK");
	EXPECT_STREQ(runtimeName("vkd3d"), "VKD3D");
	EXPECT_STREQ(runtimeName("vkd3d-proton"), "VKD3D");
	EXPECT_STREQ(runtimeName("Granite"), "Vulkan");
	EXPECT_STREQ(runtimeName(""), "Vulkan");
	EXPECT_STREQ(runtimeName("dxv"), "Vulkan");
}

TEST(Metadata, PresentModeNameSyncAndTearing)
{
	struct Case { uint32_t mode; const char *name; int sync; bool tearing; };
	const Case cases[] = {
		{ VK_PRESENT_MODE_FIFO_KHR, "FIFO", 1, false },
		{ VK_PRESENT_MODE_FIFO_RELAXED_KHR, "FIFO_RELAXED", 1, true },
		{ VK_PRESENT_MODE_MAILBOX_KHR, "MAILBOX", 0, false },
		{ VK_PRESENT_MODE_IMMEDIATE_KHR, "IMMEDIATE", 0, true },
		{ VK_PRESENT_MODE_FIFO_LATEST_READY_KHR, "FIFO_LATEST_READY", 1, false },
		{ VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR, "SHARED_DEMAND_REFRESH", 0, false },
		{ VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR, "SHARED_CONTINUOUS_REFRESH", 0, false },
		{ 123456, "Unknown", 0, false },
	};
	for (auto &c : cases) {
		EXPECT_STREQ(presentModeName(c.mode), c.name) << c.mode;
		EXPECT_EQ(syncIntervalFor(c.mode), c.sync) << c.name;
		EXPECT_EQ(allowsTearingFor(c.mode), c.tearing) << c.name;
	}
}

TEST(Metadata, ContextFromSwapchainInfo)
{
	pml::SwapchainInfo info = {};
	std::strcpy(info.engineName, "vkd3d");
	info.refreshDurationNs = 4166667;
	auto c = SwapchainContext::fromInfo(99, info);
	EXPECT_EQ(c.processId, 99u);
	EXPECT_EQ(c.engineName, "vkd3d");
	EXPECT_EQ(c.refreshDurationNs, 4166667u);

	// A full, unterminated engine name field must not overrun.
	std::memset(info.engineName, 'x', sizeof(info.engineName));
	EXPECT_EQ(SwapchainContext::fromInfo(1, info).engineName.size(), sizeof(info.engineName));
}

TEST(Metadata, RowCarriesLinuxMetadata)
{
	SwapchainEngine e(SwapchainContext{ 5, "vkd3d-proton", 0 }, MetricsMode::V2, kT0);
	std::vector<pml::FrameRecord> recs;
	for (uint64_t i = 1; i <= 4; ++i)
		recs.push_back(noInfo(makeRecord(i, kT0 + i * kMs, 1000, VK_PRESENT_MODE_IMMEDIATE_KHR)));
	auto rows = feedAll(e, recs);
	ASSERT_EQ(rows.size(), 3u);
	for (auto &row : rows) {
		EXPECT_STREQ(row.runtime, "VKD3D");
		EXPECT_STREQ(row.presentMode, "IMMEDIATE");
		EXPECT_EQ(row.vkPresentMode, uint32_t(VK_PRESENT_MODE_IMMEDIATE_KHR));
		EXPECT_EQ(row.syncInterval, 0);
		EXPECT_TRUE(row.allowsTearing);
		EXPECT_TRUE(row.metrics.allowsTearing);
		EXPECT_EQ(row.swapchainSerial, 1u);
		EXPECT_EQ(row.frame.processId, 5u);
	}
	EXPECT_EQ(rows[0].frameIndex, 2u);
}

// PresentMon computes MsSinceInput as presentStart - input in unsigned arithmetic: an input sample
// newer than the present start must not reach it (it would print as ~1.8e13 ms).
TEST(Adapter, InputNewerThanPresentStartIsDropped)
{
	auto r = displayed(makeRecord(1, kT0 + 10 * kMs, 200000), kT0 + 11 * kMs, kT0 + 19 * kMs);
	r.inputNs = r.presentStartNs + 2 * kMs;
	r.mouseClickNs = r.presentStartNs + 1;
	FrameData f = toFrameData(r, ctx());
	EXPECT_EQ(f.inputTime, 0u);
	EXPECT_EQ(f.mouseClickTime, 0u);

	r.inputNs = r.presentStartNs - 2 * kMs;
	r.mouseClickNs = r.presentStartNs;
	f = toFrameData(r, ctx());
	EXPECT_EQ(f.inputTime, r.inputNs);
	EXPECT_EQ(f.mouseClickTime, r.mouseClickNs);

	// Through the V1 engine: no wrapped value in the metric.
	SwapchainEngine e(ctx(), MetricsMode::V1, kT0);
	e.process(displayed(makeRecord(1, kT0 + 10 * kMs, 200000), kT0 + 11 * kMs, kT0 + 19 * kMs));
	auto second = displayed(makeRecord(2, kT0 + 15 * kMs, 200000), kT0 + 16 * kMs, kT0 + 24 * kMs);
	second.inputNs = second.presentStartNs + 2 * kMs;
	auto rows = e.process(second);
	ASSERT_EQ(rows.size(), 1u);
	EXPECT_LT(rows[0].metrics.msSinceInput, 1e6);
}
