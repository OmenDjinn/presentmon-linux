// SPDX-License-Identifier: MIT
// Synthetic timelines through the whole engine: exact expected values.
#include "test_util.hpp"

#include <gtest/gtest.h>

#include <CommonUtilities/mc/UnifiedSwapChain.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace pml::metrics;
using namespace pmltest;

namespace {

constexpr uint64_t kPeriod = 4166667;        // 240 Hz, ns
constexpr uint64_t kWait = 1500000;          // time inside vkQueuePresentKHR
constexpr uint64_t kReadyAfterPresent = 1 * kMs;
constexpr uint64_t kScreenAfterPresent = 12 * kMs;

double ms(uint64_t ns) { return double(ns) / 1e6; }

SwapchainContext ctx() { return SwapchainContext{ 1, "Granite", 0 }; }

std::vector<pml::FrameRecord> fifoTimeline(int n)
{
	std::vector<pml::FrameRecord> v;
	for (int i = 1; i <= n; ++i) {
		uint64_t start = kT0 + uint64_t(i) * kPeriod;
		v.push_back(displayed(makeRecord(uint64_t(i), start, kWait, kVkPresentModeFifo),
		                      start + kReadyAfterPresent,
		                      start + kScreenAfterPresent));
	}
	return v;
}

} // namespace

// 100 frames at 240 Hz, constant CPU busy / wait and screen offset.
TEST(Timeline, Fifo240HzV2)
{
	SwapchainEngine e(ctx(), MetricsMode::V2, kT0);
	auto recs = fifoTimeline(100);

	// Rows are released one displayed frame late: after record k we have k-2 rows (first seeds).
	size_t before = 0;
	std::vector<Row> rows;
	for (size_t k = 0; k < recs.size(); ++k) {
		auto got = e.process(recs[k]);
		EXPECT_LE(got.size(), 1u);
		rows.insert(rows.end(), got.begin(), got.end());
		if (k >= 2) {
			EXPECT_EQ(rows.size(), k - 1) << k;
		}
		before = rows.size();
	}
	EXPECT_EQ(before, 98u);
	auto tail = e.finish();
	ASSERT_EQ(tail.size(), 1u);
	rows.insert(rows.end(), tail.begin(), tail.end());
	ASSERT_EQ(rows.size(), 99u);
	EXPECT_TRUE(e.finish().empty());

	const double busy = ms(kPeriod - kWait);
	for (size_t i = 0; i < rows.size(); ++i) {
		const Row &row = rows[i];
		const FrameMetrics &m = row.metrics;
		SCOPED_TRACE(i);
		EXPECT_EQ(row.frameIndex, i + 2);
		EXPECT_EQ(row.display, DisplayState::Displayed);
		EXPECT_TRUE(row.hasDisplayMetrics);
		EXPECT_FALSE(row.dropped);
		EXPECT_FALSE(m.isDroppedFrame);
		EXPECT_TRUE(row.isAppFrame);
		EXPECT_EQ(row.frameType, FrameType::Application);
		EXPECT_STREQ(row.runtime, "Vulkan");
		EXPECT_STREQ(row.presentMode, "FIFO");
		EXPECT_EQ(row.syncInterval, 1);

		EXPECT_NEAR(m.msBetweenPresents, ms(kPeriod), 1e-9);
		EXPECT_NEAR(m.msBetweenDisplayChange, ms(kPeriod), 1e-9);
		EXPECT_NEAR(m.msUntilDisplayed, ms(kScreenAfterPresent), 1e-9);
		EXPECT_NEAR(m.msInPresentApi, ms(kWait), 1e-9);
		EXPECT_NEAR(m.msCPUBusy, busy, 1e-9);
		EXPECT_NEAR(m.msCPUWait, ms(kWait), 1e-9);
		EXPECT_NEAR(m.msCPUTime, ms(kPeriod), 1e-9);
		EXPECT_NEAR(m.msUntilRenderComplete, ms(kReadyAfterPresent), 1e-9);
		// Display latency: from the end of the previous present (CPU start) to the screen.
		EXPECT_NEAR(m.msDisplayLatency, ms(kScreenAfterPresent + kPeriod - kWait), 1e-9);
		EXPECT_NEAR(m.msReadyTimeToDisplayLatency, ms(kScreenAfterPresent - kReadyAfterPresent), 1e-9);
		EXPECT_EQ(m.screenTimeQpc, kT0 + (i + 2) * kPeriod + kScreenAfterPresent);
		// GPU columns are NA-ish (no GPU data yet): zero, not invented.
		EXPECT_EQ(m.msGPUBusy, 0.0);
		// Displayed time: next frame's screen time; the last one has no successor.
		if (i + 1 < rows.size())
			EXPECT_NEAR(m.msDisplayedTime, ms(kPeriod), 1e-9);
		else
			EXPECT_EQ(m.msDisplayedTime, 0.0);
		EXPECT_EQ(m.frameType, FrameType::Application);
	}
	// Relative CPU start time from the session start.
	EXPECT_NEAR(rows[0].metrics.cpuStartMs, ms(kPeriod + kWait), 1e-9);   // end of present #1
}

TEST(Timeline, Fifo240HzV1)
{
	SwapchainEngine e(ctx(), MetricsMode::V1, kT0);
	auto recs = fifoTimeline(100);
	std::vector<Row> rows;
	for (size_t k = 0; k < recs.size(); ++k) {
		auto got = e.process(recs[k]);
		EXPECT_EQ(got.size(), k == 0 ? 0u : 1u) << k;   // immediate, first present is seed only
		rows.insert(rows.end(), got.begin(), got.end());
	}
	EXPECT_TRUE(e.finish().empty());
	ASSERT_EQ(rows.size(), 99u);
	for (auto &row : rows) {
		EXPECT_NEAR(row.metrics.msBetweenPresents, ms(kPeriod), 1e-9);
		EXPECT_NEAR(row.metrics.msBetweenDisplayChange, ms(kPeriod), 1e-9);
		EXPECT_NEAR(row.metrics.msUntilDisplayed, ms(kScreenAfterPresent), 1e-9);
		EXPECT_NEAR(row.metrics.msCPUBusy, ms(kPeriod - kWait), 1e-9);
		EXPECT_NEAR(row.metrics.msCPUWait, ms(kWait), 1e-9);
		EXPECT_FALSE(row.dropped);
	}
}

// 1000 fps MAILBOX presents on a 250 Hz display: only the last present of every group of four
// reaches the screen, the other three are replaced.
TEST(Timeline, MailboxReplacedFramesAreDropped)
{
	SwapchainEngine e(ctx(), MetricsMode::V2, kT0);
	std::vector<pml::FrameRecord> recs;
	for (uint64_t i = 1; i <= 100; ++i) {
		uint64_t start = kT0 + i * kMs;
		auto r = makeRecord(i, start, 100000, kVkPresentModeMailbox);
		if (i % 4 == 3)
			recs.push_back(displayed(r, start + 200000, start + 3 * kMs));
		else
			recs.push_back(notDisplayed(r, start + 200000));
	}
	auto rows = feedAll(e, recs);
	ASSERT_EQ(rows.size(), 99u);   // present #1 seeds

	size_t dropped = 0, shown = 0;
	for (auto &row : rows) {
		SCOPED_TRACE(row.frameIndex);
		const bool shouldShow = row.frameIndex % 4 == 3;
		if (shouldShow) {
			++shown;
			EXPECT_FALSE(row.dropped);
			EXPECT_FALSE(row.metrics.isDroppedFrame);
			EXPECT_EQ(row.frame.finalState, PresentResult::Presented);
			EXPECT_NEAR(row.metrics.msUntilDisplayed, 3.0, 1e-9);
			// The first displayed present has no predecessor on screen (the seed was dropped).
			EXPECT_NEAR(row.metrics.msBetweenDisplayChange, row.frameIndex == 3 ? 0.0 : 4.0, 1e-9);
		} else {
			++dropped;
			EXPECT_TRUE(row.dropped);
			EXPECT_TRUE(row.metrics.isDroppedFrame);
			EXPECT_EQ(row.frame.finalState, PresentResult::Discarded);
			EXPECT_EQ(row.metrics.msUntilDisplayed, 0.0);
			EXPECT_EQ(row.metrics.msBetweenDisplayChange, 0.0);
			EXPECT_EQ(row.metrics.screenTimeQpc, 0u);
		}
		// CPU side is valid for every present, dropped or not.
		EXPECT_NEAR(row.metrics.msBetweenPresents, 1.0, 1e-9);
		EXPECT_NEAR(row.metrics.msCPUWait, 0.1, 1e-9);
		EXPECT_NEAR(row.metrics.msCPUBusy, 0.9, 1e-9);
		EXPECT_STREQ(row.presentMode, "MAILBOX");
	}
	// presents 2..100: displayed at 3,7,...,99 (25), the rest (74) are dropped
	EXPECT_EQ(shown, 25u);
	EXPECT_EQ(dropped, 74u);
}

// FIFO never drops: every present is displayed.
TEST(Timeline, FifoHasNoDroppedFrames)
{
	SwapchainEngine e(ctx(), MetricsMode::V2, kT0);
	auto rows = feedAll(e, fifoTimeline(50));
	for (auto &row : rows)
		EXPECT_FALSE(row.dropped);
}

// Xwayland / timing off: only CPU data exists.
TEST(Timeline, NoTimingInfo)
{
	SwapchainEngine e(ctx(), MetricsMode::V2, kT0);
	std::vector<pml::FrameRecord> recs;
	for (uint64_t i = 1; i <= 100; ++i) {
		uint64_t start = kT0 + i * kPeriod;
		recs.push_back(noInfo(makeRecord(i, start, kWait), start + kReadyAfterPresent));
	}
	std::vector<Row> rows;
	for (size_t k = 0; k < recs.size(); ++k) {
		auto got = e.process(recs[k]);
		EXPECT_EQ(got.size(), k == 0 ? 0u : 1u) << k;
		rows.insert(rows.end(), got.begin(), got.end());
	}
	EXPECT_TRUE(e.finish().empty());   // nothing was held
	ASSERT_EQ(rows.size(), 99u);
	for (auto &row : rows) {
		const FrameMetrics &m = row.metrics;
		EXPECT_EQ(row.display, DisplayState::Unknown);
		EXPECT_FALSE(row.dropped);
		EXPECT_FALSE(m.isDroppedFrame);
		EXPECT_EQ(row.frame.finalState, PresentResult::Presented);
		EXPECT_EQ(m.msUntilDisplayed, 0.0);
		EXPECT_EQ(m.msBetweenDisplayChange, 0.0);
		EXPECT_EQ(m.msDisplayedTime, 0.0);
		EXPECT_EQ(m.msDisplayLatency, 0.0);
		EXPECT_EQ(m.screenTimeQpc, 0u);
		EXPECT_NEAR(m.msBetweenPresents, ms(kPeriod), 1e-9);
		EXPECT_NEAR(m.msCPUBusy, ms(kPeriod - kWait), 1e-9);
		EXPECT_NEAR(m.msCPUWait, ms(kWait), 1e-9);
		EXPECT_NEAR(m.msCPUTime, ms(kPeriod), 1e-9);
		EXPECT_NEAR(m.msUntilRenderComplete, ms(kReadyAfterPresent), 1e-9);
	}
}

// A swapchain whose timing reports start failing half way: later rows have no display
// info but are not counted as dropped, and the displayed ones before it are intact.
TEST(Timeline, TimingLostMidStream)
{
	SwapchainEngine e(ctx(), MetricsMode::V2, kT0);
	auto recs = fifoTimeline(40);
	for (size_t i = 20; i < recs.size(); ++i) {
		recs[i].screenNs = 0;
		recs[i].flags = pml::kFrameTimingRequested | pml::kFrameTimingLost;
	}
	auto rows = feedAll(e, recs);
	ASSERT_EQ(rows.size(), 39u);
	size_t unknown = 0, shown = 0;
	for (auto &row : rows) {
		EXPECT_FALSE(row.dropped) << row.frameIndex;
		EXPECT_FALSE(row.metrics.isDroppedFrame) << row.frameIndex;
		EXPECT_NEAR(row.metrics.msCPUBusy, ms(kPeriod - kWait), 1e-9) << row.frameIndex;
		if (row.display == DisplayState::Unknown)
			++unknown;
		else
			++shown;
	}
	EXPECT_EQ(shown, 19u);    // presents 2..20
	EXPECT_EQ(unknown, 20u);  // presents 21..40
}

// Time zero (session start 0) must work too: the overlay does not need a session origin.
TEST(Timeline, SessionStartZero)
{
	SwapchainEngine e(ctx(), MetricsMode::V2, 0);
	auto rows = feedAll(e, fifoTimeline(10));
	ASSERT_EQ(rows.size(), 9u);
	EXPECT_EQ(rows[0].metrics.cpuStartMs, 0.0);
}

// V2 holds not-displayed presents until the next displayed one. When the layer stops getting display
// information (its timing queue is full at ~19000 fps in mailbox mode), rows must still flow.
TEST(Timeline, RowsKeepFlowingWhenDisplayInfoStopsArriving)
{
	SwapchainEngine e(ctx(), MetricsMode::V2, kT0);
	std::vector<Row> rows;
	auto feed = [&](const pml::FrameRecord &r) {
		auto got = e.process(r);
		rows.insert(rows.end(), got.begin(), got.end());
	};
	uint64_t i = 1;
	for (; i <= 20; ++i) {
		uint64_t start = kT0 + i * kPeriod;
		feed(displayed(makeRecord(i, start, kWait), start + kReadyAfterPresent, start + kScreenAfterPresent));
	}
	const size_t before = rows.size();

	// 3 seconds without any display information.
	const uint64_t n = 3000 * kMs / kPeriod;
	for (uint64_t k = 0; k < n; ++k, ++i) {
		uint64_t start = kT0 + i * kPeriod;
		feed(noInfo(makeRecord(i, start, kWait), start + kReadyAfterPresent));
		if (k == 100) {
			EXPECT_EQ(rows.size(), before) << "within the stall window rows are still held, as upstream";
		}
	}
	// Everything but the presents of the last stall window (and the one that re-seeded) has come out.
	const uint64_t perWindow = 500 * kMs / kPeriod;
	EXPECT_GE(rows.size() - before, n - 2 * perWindow - 8);
	for (size_t k = before; k < rows.size(); ++k) {
		if (rows[k].frameIndex <= 20)
			continue;   // displayed presents of the first run, released by the recovery
		EXPECT_EQ(rows[k].display, DisplayState::Unknown);
		EXPECT_EQ(rows[k].metrics.msDisplayLatency, 0.0);
		EXPECT_FALSE(rows[k].dropped);
	}

	// Display information coming back works normally again.
	size_t beforeDisplayed = rows.size();
	for (uint64_t k = 0; k < 20; ++k, ++i) {
		uint64_t start = kT0 + i * kPeriod;
		feed(displayed(makeRecord(i, start, kWait), start + kReadyAfterPresent, start + kScreenAfterPresent));
	}
	EXPECT_GT(rows.size(), beforeDisplayed);
	EXPECT_TRUE(rows.back().hasDisplayMetrics || rows.back().display == DisplayState::Displayed);
	// Row times never go backwards.
	for (size_t k = 1; k < rows.size(); ++k)
		EXPECT_GE(rows[k].frame.presentStartTime, rows[k - 1].frame.presentStartTime) << k;
}

// ---------------------------------------------------------------------------------------------------------
// The stall recovery above must never change what upstream computes when display information is complete:
// the same records through the port's V2 engine and through upstream's UnifiedSwapChain give the same rows.

namespace {

struct Both
{
	std::vector<Row> port;
	std::vector<pmon::util::metrics::FrameMetrics> upstream;
};

Both throughBoth(const std::vector<pml::FrameRecord> &records)
{
	Both b;
	SwapchainEngine e(ctx(), MetricsMode::V2, kT0);
	for (auto &r : records)
		for (auto &row : e.process(r))
			b.port.push_back(row);
	pmon::util::metrics::UnifiedSwapChain u;
	auto qpc = makeQpcConverter(kT0);
	for (auto &r : records)
		for (auto &row : u.ProcessPresent(qpc, toFrameData(r, ctx())))
			b.upstream.push_back(row.computed.metrics);
	return b;
}

void expectSameAsUpstream(const Both &b, const std::string &what)
{
	ASSERT_EQ(b.port.size(), b.upstream.size()) << what;
	for (size_t k = 0; k < b.port.size(); ++k) {
		const auto &p = b.port[k].metrics;
		const auto &u = b.upstream[k];
		EXPECT_EQ(p.presentStartQpc, u.presentStartQpc) << what << " row " << k;
		EXPECT_DOUBLE_EQ(p.msBetweenPresents, u.msBetweenPresents) << what << " row " << k;
		EXPECT_DOUBLE_EQ(p.msCPUBusy, u.msCPUBusy) << what << " row " << k;
		EXPECT_DOUBLE_EQ(p.msCPUWait, u.msCPUWait) << what << " row " << k;
		EXPECT_DOUBLE_EQ(p.msDisplayedTime, u.msDisplayedTime) << what << " row " << k;
		EXPECT_DOUBLE_EQ(p.msUntilDisplayed, u.msUntilDisplayed) << what << " row " << k;
		EXPECT_DOUBLE_EQ(p.msDisplayLatency, u.msDisplayLatency) << what << " row " << k;
		EXPECT_DOUBLE_EQ(p.msBetweenDisplayChange, u.msBetweenDisplayChange) << what << " row " << k;
		// The one deliberate difference: a present without display information is not called dropped (upstream's
		// math sees "no display entry" and does; see SwapchainEngine::makeRow).
		if (b.port[k].display != DisplayState::Unknown)
		{
			EXPECT_EQ(p.isDroppedFrame, u.isDroppedFrame) << what << " row " << k;
		}
	}
}

double maxOf(const std::vector<Row> &rows, double FrameMetrics::*field)
{
	double m = 0;
	for (auto &r : rows)
		m = std::max(m, r.metrics.*field);
	return m;
}

} // namespace

// A 60 Hz game with one long hitch: the post-hitch row carries the hitch as its frame time, the pre-hitch
// row as its displayed time, as in upstream. (The stall recovery used to swallow both for gaps >= 500 ms.)
TEST(Timeline, LongHitchBetweenDisplayedPresentsMatchesUpstream)
{
	const uint64_t p60 = 16666667;
	for (uint64_t hitchMs : { 400ull, 499ull, 600ull, 1100ull, 3000ull, 5000ull }) {
		std::vector<pml::FrameRecord> v;
		uint64_t t = kT0 + 10 * kMs;
		for (uint64_t i = 1; i <= 120; ++i) {
			v.push_back(displayed(makeRecord(i, t, kWait), t + kReadyAfterPresent, t + kScreenAfterPresent));
			t += (i == 60) ? hitchMs * kMs : p60;
		}
		const Both b = throughBoth(v);
		const std::string what = "hitch " + std::to_string(hitchMs) + " ms";
		expectSameAsUpstream(b, what);
		EXPECT_NEAR(maxOf(b.port, &FrameMetrics::msBetweenPresents), double(hitchMs), 0.001) << what;
		EXPECT_NEAR(maxOf(b.port, &FrameMetrics::msDisplayedTime), double(hitchMs), 0.001) << what;
	}
}

// Slow but displayed streams (a menu, a background frame limiter): every row but the first (seed) and the
// last (waits for its lookahead), as in upstream. (Below 2 fps the stall recovery used to drop all of them.)
TEST(Timeline, SlowDisplayedStreamsMatchUpstream)
{
	for (double fps : { 0.5, 1.0, 1.9, 2.1, 3.0, 3.9, 4.0, 5.0 }) {
		const uint64_t period = uint64_t(1e9 / fps);
		const uint64_t n = fps < 2 ? 20 : 100;
		std::vector<pml::FrameRecord> v;
		uint64_t t = kT0 + 10 * kMs;
		for (uint64_t i = 1; i <= n; ++i, t += period)
			v.push_back(displayed(makeRecord(i, t, kWait), t + kReadyAfterPresent, t + kScreenAfterPresent));
		const Both b = throughBoth(v);
		const std::string what = std::to_string(fps) + " fps";
		expectSameAsUpstream(b, what);
		EXPECT_EQ(b.port.size(), n - 2) << what;
	}
}

// Mailbox after a hitch: the first frames after it are replaced (not displayed) before one is shown. That
// burst is not a stall, so the pre-hitch frame keeps its displayed time, as in upstream.
TEST(Timeline, ReplacedFramesAfterAHitchMatchUpstream)
{
	std::vector<pml::FrameRecord> v;
	uint64_t t = kT0 + 10 * kMs;
	uint64_t i = 1;
	auto shown = [&](uint64_t start) {
		v.push_back(displayed(makeRecord(i, start, 200000, kVkPresentModeMailbox), start + kMs, start + 5 * kMs));
		++i;
	};
	auto replaced = [&](uint64_t start) {
		v.push_back(notDisplayed(makeRecord(i, start, 200000, kVkPresentModeMailbox), start + kMs));
		++i;
	};
	for (int k = 0; k < 30; ++k, t += kPeriod)
		shown(t);
	t += 600 * kMs;                       // the hitch
	for (int k = 0; k < 6; ++k, t += 300000)
		replaced(t);                      // 6 frames replaced within 2 ms
	for (int k = 0; k < 30; ++k, t += kPeriod)
		shown(t);
	const Both b = throughBoth(v);
	expectSameAsUpstream(b, "mailbox hitch");
	EXPECT_GT(maxOf(b.port, &FrameMetrics::msDisplayedTime), 600.0);
}

// Replaced (not displayed) frames held BEFORE a hitch too: the gap starts a new run, so the hitch never counts as
// a stall, whatever was held before or after it, and the rows around it keep upstream's values.
TEST(Timeline, ReplacedFramesAroundAHitchMatchUpstream)
{
	for (int pre : { 0, 1, 2, 3 })
		for (int post : { 0, 1, 2, 3 }) {
			std::vector<pml::FrameRecord> v;
			uint64_t t = kT0 + 10 * kMs;
			uint64_t i = 1;
			auto add = [&](bool shown) {
				auto r = makeRecord(i++, t, 200000, kVkPresentModeMailbox);
				v.push_back(shown ? displayed(r, t + kMs, t + 5 * kMs) : notDisplayed(r, t + kMs));
				t += 3 * kMs;
			};
			for (int k = 0; k < 30; ++k) {   // mailbox at 3x the refresh: shown, replaced, replaced
				add(true);
				add(false);
				add(false);
			}
			add(true);
			for (int k = 0; k < pre; ++k)
				add(false);
			t += 600 * kMs;                    // the hitch
			for (int k = 0; k < post; ++k)
				add(false);
			for (int k = 0; k < 30; ++k) {
				add(true);
				add(false);
				add(false);
			}
			add(true);
			const Both b = throughBoth(v);
			const std::string what = "pre " + std::to_string(pre) + " post " + std::to_string(post);
			expectSameAsUpstream(b, what);
			EXPECT_NEAR(maxOf(b.port, &FrameMetrics::msBetweenPresents), 603.0, 0.001) << what;
		}
}

// Display information missing from a slow stream (under 2 fps): every gap starts a new run, so nothing is
// flushed early; the rows wait for the next displayed present, as in upstream.
TEST(Timeline, SlowStreamWithoutDisplayInfoWaitsLikeUpstream)
{
	std::vector<pml::FrameRecord> v;
	uint64_t t = kT0 + 10 * kMs;
	uint64_t i = 1;
	for (int k = 0; k < 10; ++k, t += 700 * kMs, ++i)
		v.push_back(displayed(makeRecord(i, t, kWait), t + kReadyAfterPresent, t + kScreenAfterPresent));
	for (int k = 0; k < 10; ++k, t += 700 * kMs, ++i)
		v.push_back(noInfo(makeRecord(i, t, kWait), t + kReadyAfterPresent));
	for (int k = 0; k < 5; ++k, t += 700 * kMs, ++i)
		v.push_back(displayed(makeRecord(i, t, kWait), t + kReadyAfterPresent, t + kScreenAfterPresent));
	expectSameAsUpstream(throughBoth(v), "slow stream with a run of missing display information");
}
