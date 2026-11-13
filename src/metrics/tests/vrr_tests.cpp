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

// An NVIDIA output with VRR on: 48 Hz floor, the display engine re-scans there by itself.
VrrContext nvidia(double refreshHz)
{
	VrrContext ctx;
	ctx.refreshHz = refreshHz;
	ctx.vrrMinHz = 48;
	ctx.driver = DriverVrr::On;
	ctx.floor = VrrFloor::Rescan;
	return ctx;
}

// The same frames presented with IMMEDIATE: they tear in instead of waiting for a running scan-out.
std::vector<VrrFrame> tearing(std::vector<VrrFrame> v)
{
	for (VrrFrame &f : v)
		f.tears = true;
	return v;
}

// Display intervals a, b, a, b, ... with the game presenting every `present` ms.
std::vector<VrrFrame> alternating(size_t n, float a, float b, float present)
{
	std::vector<VrrFrame> v;
	for (size_t i = 0; i < n; i++)
		v.push_back({present, i % 2 ? b : a});
	return v;
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
	// The panel runs at 1000/9.3 = 108 Hz for one frame; 3.8 ms later it cannot have refreshed again faster than
	// its 240 Hz mode allows.
	EXPECT_NEAR(r.highHz, 240.0, 0.01);
	EXPECT_NEAR(r.lowHz, 107.5, 1.0);
	EXPECT_NEAR(r.swingHz, 66.2, 1.5);
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
	// 48 Hz floor (20.8 ms), a driver whose behaviour there is not known: a game hovering around 45-50 fps goes
	// in and out of the VRR range.
	std::vector<VrrFrame> v;
	for (size_t i = 0; i < 300; i++)
	{
		const float x = (i / 4) % 2 ? 22.0f : 20.0f;
		v.push_back({x, x});
	}
	VrrContext ctx = vrrOn240();
	ctx.vrrMinHz = 48;
	const VrrReadout r = analyzeVrr(v, ctx, kVrrLive);
	EXPECT_EQ(r.stability, VrrStability::BelowRange);
	EXPECT_STREQ(vrrStabilityLabel(r.stability), "below range");
	EXPECT_EQ(vrrStabilitySeverity(r.stability), 1);
	EXPECT_EQ(r.cause, VrrCause::Floor);
	EXPECT_STREQ(vrrCauseLabel(r.cause), "frame rate under the VRR floor");
	EXPECT_GT(r.lfcCrossings, 15.0);
	EXPECT_NEAR(r.belowRangePct, 50.0, 4.0);
	EXPECT_TRUE(std::isnan(r.rescans)) << "not modelled for this driver";
	EXPECT_NEAR(r.hz, 1000.0 / 21.0, 2.5) << "frames only";
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

// ---------------------------------------------------------------------------------------
// Panel refreshes, as a monitor's own counter sees them (VrrFloor::Rescan, the top of vrr.hpp)
// ---------------------------------------------------------------------------------------

TEST(Vrr, BelowTheFloorThePanelRefreshesWithoutTheGame)
{
	// 30 fps on a 48 Hz floor: 20.8 ms after each frame the display engine scans it out again, the next frame
	// comes 12.5 ms after that. 60 refreshes a second, alternating 48 and 80 Hz: what monitor counters show for a
	// 30 fps game on NVIDIA under Linux ("jumps between 48 and 80").
	const VrrReadout r = analyzeVrr(steady(300, 1000.0f / 30.0f, 0.0f), nvidia(240), kVrrLive);
	EXPECT_EQ(r.state, VrrState::Active);
	EXPECT_NEAR(r.hz, 60.0, 0.01);
	EXPECT_NEAR(r.meanHz, 60.0, 0.01);
	EXPECT_NEAR(r.lowHz, 48.0, 0.01);
	EXPECT_NEAR(r.highHz, 80.0, 0.1);
	EXPECT_NEAR(r.swingHz, 16.0, 0.1);
	EXPECT_EQ(r.stability, VrrStability::BelowRange);
	EXPECT_EQ(r.cause, VrrCause::Floor);
	EXPECT_EQ(r.rescans, double(r.judgedFrames));
	EXPECT_NEAR(r.belowRangePct, 100.0, 0.01);
	EXPECT_EQ(r.unevenPct, 0.0) << "the frames themselves come evenly";

	// The same frames from a driver whose floor behaviour is not known: the frame rate.
	VrrContext other = nvidia(240);
	other.floor = VrrFloor::Unknown;
	EXPECT_NEAR(analyzeVrr(steady(300, 1000.0f / 30.0f, 0.0f), other, kVrrLive).hz, 30.0, 0.01);
	// And only while the driver says VRR is on for the game's output.
	VrrContext unknown = nvidia(240);
	unknown.driver = DriverVrr::Unknown;
	EXPECT_NEAR(analyzeVrr(steady(300, 1000.0f / 30.0f, 0.0f), unknown, kVrrLive).hz, 30.0, 0.01);
}

TEST(Vrr, AFrameThatArrivesDuringTheRescanWaitsForIt)
{
	// Measured 2026-11-13 with a 45 fps cap (22.2 ms), VRR on. 240 Hz mode: the frames reach the screen
	// alternating 19.4 and 25.0 ms (25.0 = the 20.83 ms floor + one 4.17 ms scan-out: the frame came while the
	// old one was being re-scanned). Per pair the panel refreshed three times: 19.4 ms, 20.8 ms, 4.2 ms.
	const VrrReadout fast = analyzeVrr(alternating(400, 19.44f, 25.0f, 22.22f), nvidia(239.991), kVrrLive);
	EXPECT_NEAR(fast.hz, 67.5, 1.5) << "3 refreshes per 44.4 ms, not 45";
	EXPECT_NEAR(fast.meanHz, 67.5, 0.5);
	EXPECT_NEAR(fast.lowHz, 48.0, 0.01);
	EXPECT_NEAR(fast.highHz, 239.991, 0.01) << "the refresh right after the re-scan";
	EXPECT_NEAR(fast.belowRangePct, 50.0, 1.0);
	EXPECT_EQ(fast.stability, VrrStability::BelowRange) << "names the cause, not just 'unstable'";
	EXPECT_EQ(fast.cause, VrrCause::Floor);
	// 165 Hz mode: 17.7 and 26.9 ms (20.83 + 6.06).
	const VrrReadout slow = analyzeVrr(alternating(400, 17.7f, 26.9f, 22.22f), nvidia(165.0), kVrrLive);
	EXPECT_NEAR(slow.meanHz, 67.3, 0.5);
	EXPECT_NEAR(slow.highHz, 165.0, 0.5);
	EXPECT_EQ(slow.stability, VrrStability::BelowRange);
}

TEST(Vrr, FramesThatTearInDoNotStartARefresh)
{
	// IMMEDIATE presents under VRR tear in (KWin flips them asynchronously), so a frame does not wait for a
	// re-scan to end: measured 2026-11-13, a 45 fps cap with IMMEDIATE reaches the screen every 22.2 ms, evenly
	// (MAILBOX: 19.4 / 25.0 ms). The panel re-scans 20.8 ms after its last refresh started; the frame comes 1.4 ms
	// into that scan and tears in, the next one 2.8 ms into the next re-scan, the third 4.2 ms after the third
	// re-scan started, when that scan is over, and starts a refresh itself: four refreshes per three frames.
	const VrrReadout low = analyzeVrr(tearing(steady(300, 22.222f, 0.0f)), nvidia(240), kVrrLive);
	EXPECT_NEAR(low.hz, 60.0, 1.0);
	EXPECT_NEAR(low.meanHz, 60.0, 0.5);
	EXPECT_NEAR(low.lowHz, 48.0, 0.01);
	EXPECT_NEAR(low.highHz, 240.0, 0.5);
	EXPECT_NEAR(low.belowRangePct, 100.0, 0.01);
	EXPECT_EQ(low.stability, VrrStability::BelowRange);
	// 400 fps tearing in on a 240 Hz mode (screen times 2.5 ms apart): a scan-out takes 4.2 ms, so every second
	// frame starts one: 200 refreshes a second. 300 fps (3.33 ms): 150.
	const VrrReadout fast = analyzeVrr(tearing(steady(2000, 2.5f, 0.0f)), nvidia(240), kVrrLive);
	EXPECT_NEAR(fast.hz, 200.0, 0.5);
	EXPECT_EQ(fast.rescans, 0.0);
	EXPECT_EQ(fast.stability, VrrStability::AtMax);
	EXPECT_NEAR(analyzeVrr(tearing(steady(2000, 3.333f, 0.0f)), nvidia(240), kVrrLive).hz, 150.0, 0.5);
	// 0.94 of a scan-out after the last refresh the scan is over (the active lines of the mode): a refresh.
	EXPECT_NEAR(analyzeVrr(tearing(steady(600, 3.93f, 0.0f)), vrrOn240(), kVrrLive).hz, 240.0, 0.01);
	// Timestamp noise around the floor does not change the count: 20.7 or 21.0 ms is one refresh per frame,
	// tearing or not.
	EXPECT_NEAR(analyzeVrr(steady(100, 20.7f, 0.0f), nvidia(240)).hz, 48.3, 0.1);
	EXPECT_NEAR(analyzeVrr(alternating(100, 20.7f, 21.0f, 20.85f), nvidia(240)).meanHz, 47.96, 0.3);
	EXPECT_NEAR(analyzeVrr(tearing(alternating(100, 20.7f, 21.0f, 20.85f)), nvidia(240)).meanHz, 47.96, 0.3);

	// Frames that cannot tear (FIFO, MAILBOX) always start a refresh: screen times a little closer than a
	// scan-out are noise (a composited window: up to 0.45 ms off the refresh grid), not a torn frame.
	const VrrReadout noisy = analyzeVrr(alternating(1200, 3.6f, 4.733f, 4.1667f), vrrOn240(), kVrrLive);
	EXPECT_NEAR(noisy.hz, 240.0, 1.0);
	EXPECT_EQ(noisy.stability, VrrStability::AtMax);
	// And one that seems to come less than half a scan-out after the floor came just before the re-scan was due.
	EXPECT_EQ(analyzeVrr(steady(100, 22.5f, 0.0f), nvidia(240)).rescans, 0.0);
	EXPECT_EQ(analyzeVrr(steady(100, 23.2f, 0.0f), nvidia(240)).rescans, 100.0);
}

TEST(Vrr, LongFramesAndSingleHitches)
{
	// 10 fps: four re-scans (20.8, 41.7, 62.5, 83.3 ms) and the frame: 50 refreshes a second.
	const VrrReadout crawl = analyzeVrr(steady(100, 100.0f, 0.0f), nvidia(240));
	EXPECT_EQ(crawl.rescans, 400.0);
	EXPECT_NEAR(crawl.hz, 50.0, 0.01);
	EXPECT_NEAR(crawl.lowHz, 48.0, 0.01);
	EXPECT_NEAR(crawl.highHz, 60.0, 0.1) << "16.7 ms from the last re-scan to the frame";
	// A single hitch in a fast game: one re-scan, "stutter" (it is one), not "below range".
	std::vector<VrrFrame> v = steady(1200, 6.0f);
	v[1150] = {30.0f, 30.0f};
	const VrrReadout hitch = analyzeVrr(v, nvidia(240), kVrrLive);
	EXPECT_EQ(hitch.rescans, 1.0);
	EXPECT_EQ(hitch.stability, VrrStability::Stutter);
	// Also when the hitch is under 2.5x the median: a 100 fps game with one 24 ms frame is not "under the VRR
	// floor". The frame after it comes 4 ms early on the game's cadence; no matter.
	std::vector<VrrFrame> w = steady(400, 10.0f);
	w[350] = {24.0f, 24.0f};
	const VrrReadout small = analyzeVrr(w, nvidia(240), kVrrLive);
	EXPECT_EQ(small.rescans, 1.0);
	EXPECT_EQ(small.stutters, 1u);
	EXPECT_EQ(small.stability, VrrStability::Stutter);
	EXPECT_EQ(small.cause, VrrCause::None);
	// A game hovering just above the floor (55 fps) that drops under it now and then is "below range".
	std::vector<VrrFrame> near = steady(300, 18.2f);
	for (size_t i = 20; i < near.size(); i += 40)
		near[i] = {25.0f, 25.0f};
	const VrrReadout hover = analyzeVrr(near, nvidia(240), kVrrLive);
	EXPECT_EQ(hover.stability, VrrStability::BelowRange);
	EXPECT_EQ(hover.cause, VrrCause::Floor);
	// No floor known, or a floor at the mode's rate (no VRR range): nothing to model.
	VrrContext noFloor = nvidia(240);
	noFloor.vrrMinHz = 0;
	EXPECT_TRUE(std::isnan(analyzeVrr(steady(100, 100.0f, 0.0f), noFloor).rescans));
	VrrContext noRange = nvidia(50);
	EXPECT_TRUE(std::isnan(analyzeVrr(steady(100, 100.0f, 0.0f), noRange).rescans));
}

TEST(Vrr, ThePanelCannotRefreshFasterThanItsMode)
{
	// 500 presents a second tearing in (screen times 2 ms apart) on a 240 Hz mode: the panel still refreshes 240
	// times a second.
	const VrrReadout r = analyzeVrr(steady(1000, 2.0f, 0.0f), vrrOn240(), kVrrLive);
	EXPECT_NEAR(r.hz, 240.0, 0.01);
	EXPECT_NEAR(r.highHz, 240.0, 0.01);
	EXPECT_NEAR(r.lowHz, 240.0, 0.01);
	EXPECT_EQ(r.swingHz, 0.0);
	EXPECT_EQ(r.stability, VrrStability::AtMax);
	// Without the output's rate there is nothing to cap at.
	EXPECT_NEAR(analyzeVrr(steady(1000, 2.0f, 0.0f), {}, kVrrLive).hz, 500.0, 0.01);
}

TEST(Vrr, NoNewFramesShowsTheFloorInsteadOfTheLastRate)
{
	// A game at 141 fps stops presenting (a loading screen). The data still says 141 Hz; the panel re-scans the
	// last frame at its 48 Hz floor.
	const std::vector<VrrFrame> v = steady(600, 1000.0f / 141.0f);
	VrrOptions live = kVrrLive;
	live.sinceLastMs = 40;   // the usual lag of the newest frame
	EXPECT_NEAR(analyzeVrr(v, nvidia(240), live).hz, 141.0, 0.5);
	EXPECT_EQ(analyzeVrr(v, nvidia(240), live).stability, VrrStability::Stable);
	live.sinceLastMs = 2000;
	const VrrReadout r = analyzeVrr(v, nvidia(240), live);
	EXPECT_EQ(r.stability, VrrStability::NoFrames);
	EXPECT_STREQ(vrrStabilityLabel(r.stability), "no new frames");
	EXPECT_EQ(vrrStabilitySeverity(r.stability), 0);
	EXPECT_EQ(r.hz, 48.0);
	EXPECT_EQ(r.swingHz, 0.0);
	EXPECT_EQ(r.state, VrrState::Active);
	// A driver whose floor behaviour is unknown: no number rather than a stale one.
	const VrrReadout other = analyzeVrr(v, vrrOn240(), live);
	EXPECT_EQ(other.stability, VrrStability::NoFrames);
	EXPECT_TRUE(std::isnan(other.hz));
	// VRR off: the fixed rate, frames or not.
	VrrContext off = nvidia(240);
	off.driver = DriverVrr::Off;
	EXPECT_EQ(analyzeVrr(v, off, live).stability, VrrStability::Fixed);
	EXPECT_EQ(analyzeVrr(v, off, live).hz, 240.0);
	// A slow game is not a stopped one: at 1 fps the newest frame is seconds old all the time.
	live.sinceLastMs = 2500;
	const VrrReadout slow = analyzeVrr(steady(40, 1000.0f, 0.0f), nvidia(240), live);
	EXPECT_NE(slow.stability, VrrStability::NoFrames);
	EXPECT_NEAR(slow.hz, 48.0, 0.1) << "47 re-scans and the frame, every second";
	live.sinceLastMs = 4500;
	EXPECT_EQ(analyzeVrr(steady(40, 1000.0f, 0.0f), nvidia(240), live).stability, VrrStability::NoFrames);
	// A capture has no clock.
	EXPECT_EQ(analyzeVrr(v, nvidia(240)).stability, VrrStability::Stable);
}
