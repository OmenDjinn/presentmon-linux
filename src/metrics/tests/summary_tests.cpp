// SPDX-License-Identifier: MIT
// summarizeFrameTimes: the frame-time summary shared by the overlay and the GUI's capture analysis.
#include <pml/metrics/summary.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <vector>

using pml::metrics::FrameTimeSummary;
using pml::metrics::summarizeFrameTimes;

TEST(FrameTimeSummary, NothingUsableGivesNA)
{
	const std::vector<float> none = {0.0f, -1.0f, std::nanf("")};
	const FrameTimeSummary s = summarizeFrameTimes(none);
	EXPECT_EQ(s.count, 0u);
	EXPECT_TRUE(std::isnan(s.avgMs));
	EXPECT_TRUE(std::isnan(s.p99Ms));
	EXPECT_TRUE(std::isnan(s.low1Fps));
	EXPECT_TRUE(summarizeFrameTimes({}).count == 0);
}

TEST(FrameTimeSummary, HandComputedThousandFrames)
{
	// 990 frames of 10 ms and 10 of 50 ms (exactly the slowest 1 %).
	std::vector<float> v(990, 10.0f);
	v.insert(v.end(), 10, 50.0f);
	std::shuffle(v.begin(), v.end(), std::mt19937(7));
	const FrameTimeSummary s = summarizeFrameTimes(v);
	EXPECT_EQ(s.count, 1000u);
	EXPECT_DOUBLE_EQ(s.avgMs, 10.4);
	EXPECT_DOUBLE_EQ(s.minMs, 10.0);
	EXPECT_DOUBLE_EQ(s.maxMs, 50.0);
	EXPECT_DOUBLE_EQ(s.p50Ms, 10.0);   // index 499
	EXPECT_DOUBLE_EQ(s.p95Ms, 10.0);   // index 949
	EXPECT_DOUBLE_EQ(s.p99Ms, 10.0);   // index 989: the last 10 ms frame
	EXPECT_DOUBLE_EQ(s.low1Fps, 20.0); // mean of the 10 slowest = 50 ms
	EXPECT_DOUBLE_EQ(s.low01Fps, 20.0);// the single slowest = 50 ms
}

TEST(FrameTimeSummary, SmallInputsUseAtLeastOneFrame)
{
	const std::vector<float> v = {20.0f, 5.0f, 0.0f, 10.0f};   // the 0 is ignored
	const FrameTimeSummary s = summarizeFrameTimes(v);
	EXPECT_EQ(s.count, 3u);
	EXPECT_DOUBLE_EQ(s.p50Ms, 10.0);   // ceil(1.5) - 1 = 1
	EXPECT_DOUBLE_EQ(s.p99Ms, 20.0);   // ceil(2.97) - 1 = 2
	EXPECT_DOUBLE_EQ(s.low1Fps, 50.0); // one frame: 20 ms
	EXPECT_DOUBLE_EQ(s.low01Fps, 50.0);
}

// The overlay computed avg / p99 / lows with nth_element before this function existed. The numbers
// must not change for it.
TEST(FrameTimeSummary, MatchesTheOverlaysFormerFormulas)
{
	auto meanOfLargest = [](std::vector<float> v, size_t k) {
		k = std::clamp<size_t>(k, 1, v.size());
		std::nth_element(v.begin(), v.begin() + (k - 1), v.end(), std::greater<float>());
		double sum = 0;
		for (size_t i = 0; i < k; i++)
			sum += v[i];
		return sum / double(k);
	};
	std::mt19937 rng(11);
	std::lognormal_distribution<float> frame(2.0f, 0.4f);
	for (size_t n : {1u, 2u, 7u, 99u, 100u, 101u, 999u, 1000u, 1001u, 8192u})
	{
		std::vector<float> v(n);
		for (float &x : v)
			x = frame(rng);
		double sum = 0;
		for (float x : v)
			sum += x;
		std::vector<float> tmp = v;
		const size_t k99 = std::min(n - 1, size_t(std::ceil(double(n) * 0.99)) - 1);
		std::nth_element(tmp.begin(), tmp.begin() + k99, tmp.end());
		const double p99 = tmp[k99];
		const double low1 = 1000.0 / meanOfLargest(v, size_t(std::ceil(double(n) * 0.01)));
		const double low01 = 1000.0 / meanOfLargest(v, size_t(std::ceil(double(n) * 0.001)));

		const FrameTimeSummary s = summarizeFrameTimes(v);
		SCOPED_TRACE(n);
		EXPECT_NEAR(s.avgMs, sum / double(n), 1e-9);
		EXPECT_DOUBLE_EQ(s.p99Ms, p99);
		EXPECT_NEAR(s.low1Fps, low1, 1e-9 * low1);
		EXPECT_NEAR(s.low01Fps, low01, 1e-9 * low01);
	}
}
