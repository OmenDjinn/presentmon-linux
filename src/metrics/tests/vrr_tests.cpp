// SPDX-License-Identifier: MIT
// analyzeVrr (pml/metrics/vrr.hpp): the VRR readout (refresh rate now, swing, verdict).
#include <pml/metrics/vrr.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

using namespace pml::metrics;

namespace {

// Deterministic small jitter in [-a, a].
float wobble(size_t i, float a) { return a * float(std::sin(double(i) * 1.7)); }

std::vector<VrrFrame> steady(size_t n, float ms, float jitter = 0.1f)
{
	std::vector<VrrFrame> v;
	for (size_t i = 0; i < n; i++)
	{
		const float x = ms + wobble(i, jitter);
		v.push_back({x, x});
	}
	return v;
}

// Lies of P under vkd3d-proton, VRR on (2026-11-09): presents every ~6 ms, but about every tenth frame the screen
// waits ~9.3 ms and then catches up with ~3.8 ms (another screen's 15 fps wallpaper animation).
void liesOfP(std::vector<VrrFrame> &v, size_t from)
{
	for (size_t i = from + 10; i + 1 < v.size(); i += 10)
	{
		v[i].displayMs = 9.3f;
		v[i + 1].displayMs = 3.8f;
	}
}

VrrContext vrrOn240()
{
	VrrContext ctx;
	ctx.refreshHz = 240;
	ctx.driver = DriverVrr::On;
	return ctx;
}

} // namespace

TEST(Vrr, SteadyFramesUnderVrrAreStable)
{
	VrrContext ctx;
	ctx.refreshHz = 240;
	const VrrReadout v = analyzeVrr(steady(600, 6.0f), ctx, kVrrLive);
	EXPECT_EQ(v.state, VrrState::Active);
	EXPECT_EQ(v.stability, VrrStability::Stable);
	EXPECT_STREQ(vrrStabilityLabel(v.stability), "stable");
	EXPECT_EQ(vrrStabilitySeverity(v.stability), 0);
	EXPECT_EQ(v.cause, VrrCause::None);
	EXPECT_NEAR(v.hz, 166.7, 0.5);
	EXPECT_NEAR(v.medianHz, 166.7, 2.0);
	EXPECT_GT(v.swingHz, 0.5);
	EXPECT_LT(v.swingHz, 3.5) << "0.1 ms of wobble at 6 ms";
	EXPECT_LT(v.onGridPct, 20.0);
	EXPECT_NEAR(v.judgedSeconds, 2.0, 0.05);
	EXPECT_EQ(v.frames, 600u);
	EXPECT_LT(v.judgedFrames, 600u);
}

TEST(Vrr, ScreenUnevenWhileTheGamePacesEvenly)
{
	std::vector<VrrFrame> v = steady(600, 6.0f);
	liesOfP(v, 0);
	const VrrReadout r = analyzeVrr(v, vrrOn240(), kVrrLive);
	EXPECT_EQ(r.state, VrrState::Active);
	EXPECT_NEAR(r.unevenPct, 30.0, 3.0);   // into the long frame, long -> short, short -> normal (2.2 ms)
	EXPECT_NEAR(r.unevenDisplayPct, r.unevenPct, 0.01);
	EXPECT_EQ(r.stability, VrrStability::Unstable);
	EXPECT_EQ(vrrStabilitySeverity(r.stability), 2);
	EXPECT_EQ(r.cause, VrrCause::Display);
	EXPECT_STREQ(vrrCauseLabel(r.cause), "uneven on screen, game steady");
	// The panel runs at 1000/3.8 = 263 Hz for one frame and 1000/9.3 = 108 Hz for another.
	EXPECT_NEAR(r.highHz, 263.2, 1.0);
	EXPECT_NEAR(r.lowHz, 107.5, 1.0);
	EXPECT_NEAR(r.swingHz, 77.8, 1.5);
}

TEST(Vrr, TheLiveReadoutJudgesTheLastTwoSeconds)
{
	// 8 s steady, then 2 s of the Lies of P pattern: unstable now, only jittery over the whole run.
	std::vector<VrrFrame> v = steady(1667, 6.0f);
	liesOfP(v, 1333);
	const VrrReadout live = analyzeVrr(v, vrrOn240(), kVrrLive);
	EXPECT_EQ(live.stability, VrrStability::Unstable);
	const VrrReadout all = analyzeVrr(v, vrrOn240());
	EXPECT_EQ(all.stability, VrrStability::Jittery);
	EXPECT_EQ(all.judgedFrames, v.size());
	// And once it is over: 2 s of steady frames later the live readout is stable again.
	std::vector<VrrFrame> after = v;
	const std::vector<VrrFrame> tail = steady(340, 6.0f);
	after.insert(after.end(), tail.begin(), tail.end());
	EXPECT_EQ(analyzeVrr(after, vrrOn240(), kVrrLive).stability, VrrStability::Stable);
}

TEST(Vrr, GameFrameTimesSwing)
{
	std::vector<VrrFrame> v;
	for (size_t i = 0; i < 400; i++)
	{
		const float x = (i / 3) % 2 ? 10.0f : 6.0f;   // three at 6 ms, three at 10 ms
		v.push_back({x, x});
	}
	const VrrReadout r = analyzeVrr(v, {}, kVrrLive);
	EXPECT_EQ(r.state, VrrState::Unknown) << "no refresh, no driver state";
	EXPECT_EQ(r.stability, VrrStability::Unstable);
	EXPECT_EQ(r.cause, VrrCause::Game);
	EXPECT_EQ(r.unevenDisplayPct, 0.0);
	EXPECT_NEAR(r.lowHz, 100.0, 0.1);
	EXPECT_NEAR(r.highHz, 166.7, 0.1);
}

TEST(Vrr, MildJitterIsJittery)
{
	// Every thirtieth frame reaches the screen 2.5 ms late and the next one catches up: 3 uneven pairs in 30
	// (into the late frame, late -> early, early -> normal) = 10 %.
	std::vector<VrrFrame> v = steady(600, 6.0f);
	for (size_t i = 15; i + 1 < v.size(); i += 30)
	{
		v[i].displayMs = 8.5f;
		v[i + 1].displayMs = 3.5f;
	}
	const VrrReadout r = analyzeVrr(v, vrrOn240(), kVrrLive);
	EXPECT_NEAR(r.unevenPct, 10.0, 1.5);
	EXPECT_EQ(r.stability, VrrStability::Jittery);
	EXPECT_EQ(vrrStabilitySeverity(r.stability), 1);
	EXPECT_EQ(r.cause, VrrCause::Display);
}

TEST(Vrr, FixedRefreshShowsTheFixedRate)
{
	// 240 Hz without VRR at ~100 fps: every frame on the 4.167 ms grid (2 or 3 refreshes).
	std::vector<VrrFrame> v;
	const float t = 1000.0f / 240.0f;
	for (size_t i = 0; i < 400; i++)
		v.push_back({10.0f, i % 2 ? 2 * t : 3 * t});
	VrrContext ctx;
	ctx.refreshHz = 240;
	const VrrReadout r = analyzeVrr(v, ctx, kVrrLive);
	EXPECT_EQ(r.state, VrrState::Off);
	EXPECT_EQ(r.stability, VrrStability::Fixed);
	EXPECT_STREQ(vrrStabilityLabel(r.stability), "fixed");
	EXPECT_DOUBLE_EQ(r.hz, 240.0) << "what the monitor's own counter shows";
	EXPECT_EQ(r.swingHz, 0.0);
	EXPECT_GT(r.onGridPct, 99.0);

	ctx.driver = DriverVrr::Off;
	EXPECT_EQ(analyzeVrr(steady(100, 6.0f), ctx).state, VrrState::Off) << "the driver's word wins";
}

TEST(Vrr, FixedRefreshFoundWithoutKnowingTheRate)
{
	// The in-game overlay does not know the output's rate. 90 fps on a fixed 120 Hz screen: the screen updates
	// snap to 8.33 / 16.67 ms while the game presents every 11.1 ms.
	std::vector<VrrFrame> v;
	for (size_t i = 0; i < 300; i++)
		v.push_back({11.11f, i % 3 == 2 ? 16.667f : 8.333f});
	const VrrReadout r = analyzeVrr(v, {}, kVrrLive);
	EXPECT_EQ(r.state, VrrState::Off);
	EXPECT_NEAR(r.refreshHz, 120.0, 0.2) << "the lowest rate that explains the grid, not 240 or 480";
	EXPECT_NEAR(r.hz, 120.0, 0.2);
	EXPECT_EQ(r.stability, VrrStability::Fixed);
}

TEST(Vrr, AGameOnTheGridCannotBeToldApart)
{
	// A VRR game capped at 120 fps on a 240 Hz screen, or FIFO at the refresh rate: screen and game both on
	// the grid. Not "off" (that was a bug of the first version): unknown without the driver, active with it.
	const std::vector<VrrFrame> v = steady(300, 1000.0f / 120.0f, 0.01f);
	VrrContext ctx;
	ctx.refreshHz = 240;
	EXPECT_EQ(analyzeVrr(v, ctx).state, VrrState::Unknown);
	EXPECT_EQ(analyzeVrr(v).state, VrrState::Unknown);
	EXPECT_EQ(analyzeVrr(v, ctx).stability, VrrStability::Stable);
	ctx.driver = DriverVrr::On;
	EXPECT_EQ(analyzeVrr(v, ctx).state, VrrState::Active);
}

TEST(Vrr, FasterThanTheRefreshIsAtMaxEvenWithoutTheRate)
{
	// 300 fps on a 240 Hz screen: every update on the 4.17 ms grid, the game off it.
	std::vector<VrrFrame> v;
	for (size_t i = 0; i < 600; i++)
		v.push_back({3.333f, 1000.0f / 240.0f});
	const VrrReadout r = analyzeVrr(v, {}, kVrrLive);
	EXPECT_EQ(r.state, VrrState::Idle);
	EXPECT_EQ(r.stability, VrrStability::AtMax);
	EXPECT_NEAR(r.refreshHz, 240.0, 0.5);
	EXPECT_NEAR(r.hz, 240.0, 0.5);
}

TEST(Vrr, AtTheRefreshCapIsAtMax)
{
	const VrrReadout r = analyzeVrr(steady(600, 1000.0f / 240.0f, 0.02f), vrrOn240(), kVrrLive);
	EXPECT_EQ(r.state, VrrState::Idle);
	EXPECT_EQ(r.stability, VrrStability::AtMax);
	EXPECT_STREQ(vrrStabilityLabel(r.stability), "at max");
}

TEST(Vrr, ASmoothlyChangingFrameRateIsVarying)
{
	// The frame interval glides between 6 and 9 ms and back over 2 s (166 <-> 111 Hz): no frame-to-frame jump,
	// but the 250 ms rate moves by a third. VRR doing its job.
	std::vector<VrrFrame> v;
	for (size_t i = 0; i < 1500; i++)
	{
		const float x = 7.5f + 1.5f * float(std::sin(double(i) * 2.0 * std::numbers::pi / 270.0));
		v.push_back({x, x});
	}
	const VrrReadout r = analyzeVrr(v, vrrOn240(), kVrrLive);
	EXPECT_LT(r.unevenPct, 1.0);
	EXPECT_GT(r.driftPct, 25.0);
	EXPECT_EQ(r.stability, VrrStability::Varying);
	EXPECT_GT(r.swingHz, 20.0);
}

TEST(Vrr, CrossingTheVrrFloor)
{
	// 48 Hz floor (20.8 ms): a game hovering around 45-50 fps goes in and out of LFC.
	std::vector<VrrFrame> v;
	for (size_t i = 0; i < 300; i++)
	{
		const float x = (i / 4) % 2 ? 22.0f : 20.0f;
		v.push_back({x, x});
	}
	VrrContext ctx = vrrOn240();
	ctx.vrrMinHz = 48;
	const VrrReadout r = analyzeVrr(v, ctx, kVrrLive);
	EXPECT_EQ(r.stability, VrrStability::Lfc);
	EXPECT_GT(r.lfcCrossings, 15.0);
	EXPECT_NEAR(r.belowRangePct, 50.0, 4.0);
	const VrrReadout noFloor = analyzeVrr(v, vrrOn240(), kVrrLive);
	EXPECT_TRUE(std::isnan(noFloor.lfcCrossings)) << "no floor known";
	EXPECT_EQ(noFloor.stability, VrrStability::Stable) << "2 ms steps at 20 ms are not uneven";
}

TEST(Vrr, AStutterShowsWhileItIsRecent)
{
	// One 30 ms hitch near the end of 7 s of steady frames: "stutter" in the live readout, a rounding error over
	// the whole run.
	std::vector<VrrFrame> v = steady(1200, 6.0f);
	v[1150] = {30.0f, 30.0f};
	const VrrReadout live = analyzeVrr(v, vrrOn240(), kVrrLive);
	EXPECT_EQ(live.stability, VrrStability::Stutter);
	EXPECT_EQ(live.stutters, 1u);
	const VrrReadout all = analyzeVrr(v, vrrOn240());
	EXPECT_EQ(all.stability, VrrStability::Stable);

	// Regular hitches (2.6 per second) over a whole run.
	std::vector<VrrFrame> w = steady(1200, 6.0f);
	for (size_t i = 50; i < w.size(); i += 60)
		w[i] = {30.0f, 30.0f};
	EXPECT_EQ(analyzeVrr(w, vrrOn240()).stability, VrrStability::Stutter);
}

TEST(Vrr, NowIsTheLastQuarterSecond)
{
	// 5 s at 6 ms, then 0.5 s at 10 ms: the panel runs at 100 Hz now.
	std::vector<VrrFrame> v = steady(833, 6.0f, 0.0f);
	const std::vector<VrrFrame> slow = steady(50, 10.0f, 0.0f);
	v.insert(v.end(), slow.begin(), slow.end());
	const VrrReadout r = analyzeVrr(v, vrrOn240(), kVrrLive);
	EXPECT_NEAR(r.hz, 100.0, 0.01);
	// At 3 fps a single frame is longer than the window: the rate of that one frame.
	std::vector<VrrFrame> crawl = steady(40, 333.0f, 0.0f);
	EXPECT_NEAR(analyzeVrr(crawl, vrrOn240(), kVrrLive).hz, 3.003, 0.001);
}

TEST(Vrr, TooFewFramesAndDroppedOnes)
{
	const VrrReadout none = analyzeVrr(steady(10, 6.0f));
	EXPECT_EQ(none.state, VrrState::NoData);
	EXPECT_EQ(none.stability, VrrStability::None);
	EXPECT_TRUE(std::isnan(none.hz));
	EXPECT_EQ(vrrStabilitySeverity(none.stability), -1);
	EXPECT_STREQ(vrrStabilityLabel(none.stability), "");
	// Dropped frames (no display time) are skipped, not counted as events.
	std::vector<VrrFrame> v = steady(300, 6.0f);
	for (size_t i = 0; i < v.size(); i += 7)
		v[i].displayMs = std::nanf("");
	const VrrReadout r = analyzeVrr(v);
	EXPECT_LT(r.frames, 300u);
	EXPECT_EQ(r.stability, VrrStability::Stable);
}
