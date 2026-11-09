// SPDX-License-Identifier: MIT
// analyzeVrr (pml/metrics/vrr.hpp): the VRR stability verdict.
#include <pml/metrics/vrr.hpp>

#include <gtest/gtest.h>

#include <cmath>
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

} // namespace

TEST(Vrr, SteadyFramesUnderVrrAreStable)
{
	VrrContext ctx;
	ctx.refreshHz = 240;
	const VrrVerdict v = analyzeVrr(steady(600, 6.0f), ctx);
	EXPECT_EQ(v.state, VrrState::Active);
	EXPECT_GE(v.score, 95);
	EXPECT_STREQ(vrrVerdictLabel(v.score), "stable");
	EXPECT_EQ(v.cause, VrrCause::None);
	EXPECT_NEAR(v.medianDisplayMs, 6.0, 0.15);
	EXPECT_LT(v.onGridPct, 20.0);
}

TEST(Vrr, ScreenUnevenWhileTheGamePacesEvenly)
{
	// Lies of P under vkd3d-proton, VRR on (2026-11-09): presents every ~6 ms, but about every tenth frame
	// the screen waits ~9.3 ms and then catches up with ~3.8 ms.
	std::vector<VrrFrame> v = steady(600, 6.0f);
	for (size_t i = 10; i + 1 < v.size(); i += 10)
	{
		v[i].displayMs = 9.3f;
		v[i + 1].displayMs = 3.8f;
	}
	VrrContext ctx;
	ctx.refreshHz = 240;
	ctx.driver = DriverVrr::On;
	const VrrVerdict r = analyzeVrr(v, ctx);
	EXPECT_EQ(r.state, VrrState::Active);
	EXPECT_NEAR(r.unevenPct, 30.0, 3.0);   // into the long frame, long -> short, short -> normal (2.2 ms)
	EXPECT_NEAR(r.unevenDisplayPct, r.unevenPct, 0.01);
	EXPECT_EQ(r.cause, VrrCause::Display);
	EXPECT_GE(r.score, 25);
	EXPECT_LT(r.score, 40);
	EXPECT_STREQ(vrrVerdictLabel(r.score), "very unstable");
	EXPECT_STREQ(vrrCauseLabel(r.cause), "uneven on screen, game steady");
}

TEST(Vrr, GameFrameTimesSwing)
{
	std::vector<VrrFrame> v;
	for (size_t i = 0; i < 400; i++)
	{
		const float x = (i / 3) % 2 ? 10.0f : 6.0f;   // three at 6 ms, three at 10 ms
		v.push_back({x, x});
	}
	const VrrVerdict r = analyzeVrr(v);
	EXPECT_EQ(r.state, VrrState::Unknown) << "no refresh, no driver state";
	EXPECT_EQ(r.cause, VrrCause::Game);
	EXPECT_EQ(r.unevenDisplayPct, 0.0);
	EXPECT_LT(r.score, 65);
}

TEST(Vrr, FixedRefreshIsOffWithoutAScore)
{
	// 240 Hz without VRR at ~100 fps: every frame on the 4.167 ms grid (2 or 3 refreshes).
	std::vector<VrrFrame> v;
	const float t = 1000.0f / 240.0f;
	for (size_t i = 0; i < 400; i++)
		v.push_back({10.0f, i % 2 ? 2 * t : 3 * t});
	VrrContext ctx;
	ctx.refreshHz = 240;
	const VrrVerdict r = analyzeVrr(v, ctx);
	EXPECT_EQ(r.state, VrrState::Off);
	EXPECT_EQ(r.score, -1);
	EXPECT_GT(r.onGridPct, 99.0);
	EXPECT_STREQ(vrrVerdictLabel(r.score), "");

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
	const VrrVerdict r = analyzeVrr(v);
	EXPECT_EQ(r.state, VrrState::Off);
	EXPECT_NEAR(r.refreshHz, 120.0, 0.2) << "the lowest rate that explains the grid, not 240 or 480";
	EXPECT_EQ(r.score, -1);
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
	EXPECT_GE(analyzeVrr(v, ctx).score, 95);
	ctx.driver = DriverVrr::On;
	EXPECT_EQ(analyzeVrr(v, ctx).state, VrrState::Active);
}

TEST(Vrr, FasterThanTheRefreshIsIdleEvenWithoutTheRate)
{
	// 300 fps on a 240 Hz screen: every update on the 4.17 ms grid, the game off it.
	std::vector<VrrFrame> v;
	for (size_t i = 0; i < 300; i++)
		v.push_back({3.333f, 1000.0f / 240.0f});
	const VrrVerdict r = analyzeVrr(v);
	EXPECT_EQ(r.state, VrrState::Idle);
	EXPECT_NEAR(r.refreshHz, 240.0, 0.5);
}

TEST(Vrr, AtTheRefreshCapIsIdle)
{
	VrrContext ctx;
	ctx.refreshHz = 240;
	ctx.driver = DriverVrr::On;
	const VrrVerdict r = analyzeVrr(steady(300, 1000.0f / 240.0f, 0.02f), ctx);
	EXPECT_EQ(r.state, VrrState::Idle);
	EXPECT_GE(r.score, 95);
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
	VrrContext ctx;
	ctx.refreshHz = 240;
	ctx.vrrMinHz = 48;
	const VrrVerdict r = analyzeVrr(v, ctx);
	EXPECT_EQ(r.cause, VrrCause::Lfc);
	EXPECT_GT(r.lfcCrossings, 50.0);
	EXPECT_NEAR(r.belowRangePct, 50.0, 2.0);
	EXPECT_LT(r.score, 40);
	EXPECT_TRUE(std::isnan(analyzeVrr(v).lfcCrossings)) << "no floor known";
}

TEST(Vrr, OccasionalStutter)
{
	std::vector<VrrFrame> v = steady(600, 6.0f);
	for (size_t i = 50; i < v.size(); i += 60)
		v[i] = {30.0f, 30.0f};
	const VrrVerdict r = analyzeVrr(v);
	EXPECT_EQ(r.cause, VrrCause::Stutter);
	EXPECT_NEAR(r.stutterPct, 10.0 / 6.0, 0.2);
	EXPECT_LT(r.score, 85);
	EXPECT_GE(r.score, 65) << "ten stutters in 600 frames is minor";
}

TEST(Vrr, TooFewFramesAndDroppedOnes)
{
	EXPECT_EQ(analyzeVrr(steady(10, 6.0f)).state, VrrState::NoData);
	EXPECT_EQ(analyzeVrr(steady(10, 6.0f)).score, -1);
	// Dropped frames (no display time) are skipped, not counted as events.
	std::vector<VrrFrame> v = steady(300, 6.0f);
	for (size_t i = 0; i < v.size(); i += 7)
		v[i].displayMs = std::nanf("");
	const VrrVerdict r = analyzeVrr(v);
	EXPECT_LT(r.frames, 300u);
	EXPECT_GE(r.score, 95);
}
