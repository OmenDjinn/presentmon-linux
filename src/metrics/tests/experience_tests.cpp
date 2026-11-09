// SPDX-License-Identifier: MIT
// analyzeExperience (pml/metrics/experience.hpp): each impairment on frame sequences with a known cause.
#include <pml/metrics/experience.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace pml::metrics;

namespace {

std::vector<ExperienceFrame> steady(size_t n, float ms, float latency = 10.0f)
{
	return std::vector<ExperienceFrame>(n, ExperienceFrame{ms, 0.0f, latency});
}

} // namespace

TEST(Experience, NeedsEnoughFrames)
{
	EXPECT_EQ(analyzeExperience(steady(ExperienceVerdict::kMinFrames - 1, 8.33f)).score, -1);
	EXPECT_GE(analyzeExperience(steady(ExperienceVerdict::kMinFrames, 8.33f)).score, 0);
	EXPECT_EQ(analyzeExperience({}).score, -1);
	EXPECT_STREQ(experienceVerdictLabel(-1), "");
	EXPECT_STREQ(experienceVerdictLabel(85), "smooth");
	EXPECT_STREQ(experienceVerdictLabel(65), "minor issues");
	EXPECT_STREQ(experienceVerdictLabel(40), "rough");
	EXPECT_STREQ(experienceVerdictLabel(39), "very rough");
}

TEST(Experience, RateFlattensAtHighFrameRates)
{
	const auto at = [](double fps) { return analyzeExperience(steady(600, float(1000.0 / fps))).rate; };
	EXPECT_NEAR(at(250), 0.0, 1e-3);
	EXPECT_NEAR(at(120), 2.6, 0.1);
	EXPECT_NEAR(at(60), 10.2, 0.1);
	EXPECT_NEAR(at(30), 26.6, 0.2);
	EXPECT_LE(at(5), 60.0);
	// Diminishing returns: 60 -> 120 gains less than 30 -> 60.
	EXPECT_LT(at(60) - at(120), at(30) - at(60));
}

TEST(Experience, SteadyHighRateScoresHigh)
{
	const ExperienceVerdict s = analyzeExperience(steady(1200, 1000.0f / 240.0f, 5.0f));
	EXPECT_GE(s.score, 98);
	EXPECT_EQ(s.hitches, 0u);
	EXPECT_EQ(s.cause, ExperienceCause::None) << "85 and above: no cause, as with the VRR verdict";
}

TEST(Experience, JudderIsMotionNotStutter)
{
	// 90 fps on a 120 Hz screen: every third frame stays two refreshes; the motion steps disagree by ~4 ms.
	std::vector<ExperienceFrame> v;
	for (int i = 0; i < 900; i++)
		v.push_back({i % 3 == 2 ? 16.67f : 8.33f, i % 3 == 2 ? 5.56f : -2.78f, 15.0f});
	const ExperienceVerdict s = analyzeExperience(v);
	EXPECT_EQ(s.hitches, 0u) << "one missed refresh is judder, not a hitch";
	EXPECT_GT(s.motion, 5.0);
	EXPECT_EQ(s.stutter, 0.0);
	EXPECT_LT(s.score, 85);
	EXPECT_EQ(s.cause, ExperienceCause::Motion);
	EXPECT_STREQ(experienceCauseLabel(s.cause), "judder (uneven motion)");
}

TEST(Experience, AFreezeIsAStutter)
{
	auto v = steady(1200, 8.33f);   // 10 s at 120 fps
	v[600].displayIntervalMs = 108.0f;
	const ExperienceVerdict s = analyzeExperience(v);
	EXPECT_EQ(s.hitches, 1u);
	EXPECT_NEAR(s.worstHitchMs, 108.0, 0.1);
	EXPECT_NEAR(s.stutter, 0.1 * (108.0 - 8.33) / 10.08 + 25.0 * 0.58, 0.3);
	EXPECT_EQ(s.cause, ExperienceCause::Stutter);
	// Spikes under 50 ms count only by their time per second.
	auto w = steady(1200, 8.33f);
	w[600].displayIntervalMs = 40.0f;
	EXPECT_LT(analyzeExperience(w).stutter, 1.0);
}

TEST(Experience, AHitchTheQueueHidesStillCountsThroughAnimationError)
{
	// V-Sync with a full queue: the screen keeps its cadence, but one frame's motion jumps 30 ms.
	auto v = steady(1200, 8.33f);
	v[600].animationErrorMs = 30.0f;
	const ExperienceVerdict s = analyzeExperience(v);
	EXPECT_EQ(s.hitches, 1u);
	EXPECT_GT(s.stutter, 0.2);
}

TEST(Experience, LatencyUsesTheG1072Curve)
{
	EXPECT_NEAR(analyzeExperience(steady(600, 4.0f, 0.5f)).latency, 0.0, 0.1);
	const double l40 = analyzeExperience(steady(600, 4.0f, 40.0f)).latency;
	const double l100 = analyzeExperience(steady(600, 4.0f, 100.0f)).latency;
	EXPECT_NEAR(l40, 6.3, 0.5);
	EXPECT_NEAR(l100, 16.6, 0.5);
	// Unknown latency: no impairment, NA.
	auto v = steady(600, 8.33f, std::nanf(""));
	const ExperienceVerdict s = analyzeExperience(v);
	EXPECT_EQ(s.latency, 0.0);
	EXPECT_TRUE(std::isnan(s.latencyMs));
}

TEST(Experience, ACapThatIsSmootherBeatsFastButSpiky)
{
	// The case GQI gets wrong: a steady 141 fps cap against 200 fps average with hitches and uneven frames.
	const ExperienceVerdict capped = analyzeExperience(steady(1410, 1000.0f / 141.0f, 8.0f));
	std::vector<ExperienceFrame> spiky;
	for (int i = 0; i < 2000; i++)
		spiky.push_back({i % 200 == 199 ? 60.0f : (i % 2 ? 3.5f : 6.5f), i % 2 ? -3.0f : 3.0f, 8.0f});
	EXPECT_GT(capped.score, analyzeExperience(spiky).score);
}
