// SPDX-License-Identifier: MIT
// FrameLimiter and the cap policy (frame_limiter.hpp), on a fake clock.
#include "frame_limiter.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace pml::layer;

namespace {

// Fake time: sleeping lands `wakeLateNs` after the requested time, a spin step costs `spinNs`.
class FakeClock final : public WaitClock
{
public:
	uint64_t t = 1'000'000'000;
	uint64_t wakeLateNs = 50'000;
	uint64_t spinNs = 200;
	uint64_t sleeps = 0;
	bool failSleeps = false;   // a sandbox that refuses every sleep
	uint64_t now() override { return t; }
	bool sleepUntil(uint64_t deadline) override
	{
		sleeps++;
		if (failSleeps)
			return false;
		if (deadline > t)
			t = deadline;
		t += wakeLateNs;
		return true;
	}
	void relax() override { t += spinNs; }
	void work(uint64_t ns) { t += ns; }
};

constexpr uint64_t ms(double v) { return uint64_t(v * 1'000'000.0); }

// Runs `frames` frames of `workNs` app work each and returns the release times.
std::vector<uint64_t> run(FrameLimiter &lim, FakeClock &clock, size_t frames, uint64_t workNs)
{
	std::vector<uint64_t> releases;
	for (size_t i = 0; i < frames; i++)
	{
		clock.work(workNs);
		lim.wait();
		releases.push_back(clock.t);
	}
	return releases;
}

} // namespace

TEST(FrameLimiter, HoldsAnExactRateOnAGrid)
{
	FakeClock clock;
	FrameLimiter lim(clock);
	lim.setInterval(ms(10));
	const auto r = run(lim, clock, 1001, ms(2));
	// After the first frame every release is within the spin step of its deadline, and the long-run rate is
	// exact (no drift from wake-up lateness).
	for (size_t i = 2; i < r.size(); i++)
	{
		const int64_t d = int64_t(r[i] - r[i - 1]) - int64_t(ms(10));
		EXPECT_LE(std::llabs(d), 400) << i;
	}
	const double avg = double(r.back() - r[1]) / double(r.size() - 2);
	EXPECT_NEAR(avg, double(ms(10)), 1.0);
	const LimiterStats s = lim.stats();
	EXPECT_EQ(s.reanchors, 0u);
	EXPECT_GE(s.waits, 999u);
	EXPECT_NEAR(s.meanWaitNs, double(ms(8)), double(ms(0.1)));
	EXPECT_LE(s.releaseP99Ns, 400u);
}

TEST(FrameLimiter, MarginAdaptsToLateWakeUps)
{
	// Wake-ups 300 us late (a busy machine, default timer slack): the first sleeps overshoot the initial
	// 200 us margin; the margin grows to 1.5 x 300 + 20 = 470 us and releases are on time again.
	FakeClock clock;
	clock.wakeLateNs = 300'000;
	FrameLimiter lim(clock);
	lim.setInterval(ms(10));
	run(lim, clock, 40, ms(2));
	EXPECT_EQ(lim.stats().marginNs, 470'000u);
	const auto r = run(lim, clock, 200, ms(2));
	for (size_t i = 1; i < r.size(); i++)
		EXPECT_LE(std::llabs(int64_t(r[i] - r[i - 1]) - int64_t(ms(10))), 400) << i;
}

TEST(FrameLimiter, MarginStaysWithinBounds)
{
	FakeClock clock;
	clock.wakeLateNs = 0;   // perfect wake-ups: the margin shrinks to the 30 us floor
	FrameLimiter lim(clock);
	lim.setInterval(ms(10));
	run(lim, clock, 100, ms(1));
	EXPECT_EQ(lim.stats().marginNs, uint32_t(FrameLimiter::kMinMarginNs));
	clock.wakeLateNs = ms(5);   // terrible wake-ups: capped at min(1 ms, T/4)
	run(lim, clock, 100, ms(1));
	EXPECT_EQ(lim.stats().marginNs, uint32_t(FrameLimiter::kMaxMarginNs));
	FrameLimiter fast(clock);
	fast.setInterval(ms(2));      // 500 fps: T/4 = 500 us
	run(fast, clock, 100, ms(0.2));
	EXPECT_EQ(fast.stats().marginNs, 500'000u);
}

TEST(FrameLimiter, ALateFrameMovesTheGridInsteadOfCatchingUp)
{
	// One 15 ms frame at a 10 ms cap: the next frame gets a full interval, not a 5 ms catch-up frame (a short
	// frame is a refresh-rate jump under VRR).
	FakeClock clock;
	FrameLimiter lim(clock);
	lim.setInterval(ms(10));
	auto r = run(lim, clock, 20, ms(2));
	clock.work(ms(15));
	lim.wait();
	const uint64_t lateRelease = clock.t;
	r = run(lim, clock, 5, ms(2));
	EXPECT_GE(r[0] - lateRelease, ms(10) - 400);
	EXPECT_LE(r[0] - lateRelease, ms(10) + 400);
	EXPECT_EQ(lim.stats().reanchors, 1u);
}

TEST(FrameLimiter, WakeUpNoiseStaysOnTheGrid)
{
	// Late by less than the tolerance (min(500 us, T/8)): no re-anchor, the rate stays exact.
	FakeClock clock;
	clock.wakeLateNs = 0;
	clock.spinNs = 100'000;   // coarse spin: releases up to 100 us late
	FrameLimiter lim(clock);
	lim.setInterval(ms(10));
	const auto r = run(lim, clock, 501, ms(3));
	EXPECT_EQ(lim.stats().reanchors, 0u);
	EXPECT_NEAR(double(r.back() - r[1]) / double(r.size() - 2), double(ms(10)), 200.0);
}

TEST(FrameLimiter, SlowerThanTheCapNeverWaits)
{
	FakeClock clock;
	FrameLimiter lim(clock);
	lim.setInterval(ms(10));
	run(lim, clock, 50, ms(12));
	const LimiterStats s = lim.stats();
	EXPECT_EQ(s.waits, 0u);
	EXPECT_EQ(clock.sleeps, 0u);
	EXPECT_LT(s.meanWaitNs, 1.0);
}

TEST(FrameLimiter, IntervalChangesReAnchorAndZeroIsOff)
{
	FakeClock clock;
	FrameLimiter lim(clock);
	EXPECT_EQ(lim.wait(), 0u) << "no cap";
	lim.setInterval(ms(10));
	run(lim, clock, 10, ms(1));
	lim.setInterval(ms(5));
	clock.work(ms(1));
	EXPECT_EQ(lim.wait(), 0u) << "the first frame after a change passes";
	const auto r = run(lim, clock, 10, ms(1));
	for (size_t i = 1; i < r.size(); i++)
		EXPECT_LE(std::llabs(int64_t(r[i] - r[i - 1]) - int64_t(ms(5))), 400);
	lim.setInterval(0);
	clock.work(ms(1));
	EXPECT_EQ(lim.wait(), 0u);
}

TEST(CapPolicy, VrrAutoCap)
{
	EXPECT_EQ(vrrAutoCap(240.0), 224.0);
	EXPECT_EQ(vrrAutoCap(239.991), 223.0);
	EXPECT_EQ(vrrAutoCap(144.0), 138.0);
	EXPECT_EQ(vrrAutoCap(120.0), 116.0);
	EXPECT_EQ(vrrAutoCap(165.0), 157.0);
	EXPECT_EQ(vrrAutoCap(20.0), 0.0);
	EXPECT_EQ(vrrAutoCap(std::nan("")), 0.0);
}

TEST(FrameLimiter, ARefusedSleepSpinsAndKeepsTheMargin)
{
	// clock_nanosleep and nanosleep both refused: the limiter still holds the rate (spinning), counts the failures,
	// and does not read the failed sleeps as perfect wake-ups (the margin stays where it was).
	FakeClock clock;
	clock.failSleeps = true;
	FrameLimiter lim(clock);
	lim.setInterval(ms(10));
	const auto r = run(lim, clock, 101, ms(2));
	for (size_t i = 2; i < r.size(); i++)
		EXPECT_NEAR(double(r[i] - r[i - 1]), double(ms(10)), 1000.0) << i;
	EXPECT_EQ(lim.stats().sleepFailures, 100u);
	EXPECT_EQ(lim.stats().marginNs, uint32_t(FrameLimiter::kInitialMarginNs));
}

TEST(FrameLimiter, IntervalIsBounded)
{
	FakeClock clock;
	FrameLimiter lim(clock);
	lim.setInterval(100'000'000'000ull);   // 0.01 fps from a corrupt control block
	EXPECT_EQ(lim.interval(), FrameLimiter::kMaxIntervalNs);
	clock.work(ms(1));
	lim.wait();
	const uint64_t t0 = clock.t;
	lim.wait();
	EXPECT_LE(clock.t - t0, FrameLimiter::kMaxIntervalNs + ms(1)) << "never holds a present longer than 1 s";
}

TEST(CapPolicy, EffectiveCap)
{
	AutoCapInfo on{1, 240.0, 48.0};
	AutoCapInfo off{2, 240.0, 48.0};
	EXPECT_EQ(effectiveCapFps(0, 144, on), 0.0) << "off";
	EXPECT_EQ(effectiveCapFps(1, 144, off), 144.0) << "fixed ignores VRR";
	EXPECT_EQ(effectiveCapFps(2, 0, on), 224.0);
	EXPECT_EQ(effectiveCapFps(2, 165, on), 165.0) << "auto with a lower user bound";
	EXPECT_EQ(effectiveCapFps(2, 300, on), 224.0);
	EXPECT_EQ(effectiveCapFps(2, 0, off), 0.0) << "auto: nothing without VRR";
	EXPECT_EQ(effectiveCapFps(2, 0, AutoCapInfo{}), 0.0);
	EXPECT_EQ(effectiveCapFps(1, -5, on), 0.0);
	// Outside 1..10000 fps (only a foreign writer of the control block can store that): no cap / no bound.
	EXPECT_EQ(effectiveCapFps(1, 0.01, on), 0.0);
	EXPECT_EQ(effectiveCapFps(1, 20000, on), 0.0);
	EXPECT_EQ(effectiveCapFps(1, std::nan(""), on), 0.0);
	EXPECT_EQ(effectiveCapFps(2, 0.5, on), 224.0) << "an invalid Auto bound is no bound";
	EXPECT_EQ(effectiveCapFps(1, 1.0, on), 1.0);
	EXPECT_EQ(effectiveCapFps(1, 10000.0, on), 10000.0);
}

TEST(CapPolicy, ReflexInterval)
{
	EXPECT_EQ(capIntervalUs(224.0), 4465u) << "ceil: never faster than the cap";
	EXPECT_EQ(capIntervalUs(100.0), 10000u);
	EXPECT_EQ(capIntervalUs(0.0), 0u);
	EXPECT_EQ(mergedIntervalUs(0, 4465), 4465u);
	EXPECT_EQ(mergedIntervalUs(16667, 4465), 16667u) << "the game's own lower cap stays";
}
