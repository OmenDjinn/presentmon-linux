// SPDX-License-Identifier: MIT
#include "pml/metrics/summary.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pml::metrics {

namespace {

// Index of the nearest-rank percentile p (0..1] in an ascending array of n > 0 values.
size_t rankIndex(size_t n, double p)
{
	const double k = std::ceil(double(n) * p);
	return std::min(n - 1, k >= 1 ? size_t(k) - 1 : size_t(0));
}

// Mean of the largest max(1, ceil(n * fraction)) values of an ascending array.
double meanOfSlowest(const std::vector<float> &ascending, double fraction)
{
	const size_t n = ascending.size();
	const size_t k = std::clamp<size_t>(size_t(std::ceil(double(n) * fraction)), 1, n);
	double sum = 0;
	for (size_t i = n - k; i < n; i++)
		sum += ascending[i];
	return sum / double(k);
}

} // namespace

FrameTimeSummary summarizeFrameTimes(std::span<const float> frameTimesMs)
{
	FrameTimeSummary s;
	std::vector<float> v;
	v.reserve(frameTimesMs.size());
	double sum = 0;
	for (float x : frameTimesMs)
	{
		if (x > 0)   // also drops NaN
		{
			v.push_back(x);
			sum += x;
		}
	}
	if (v.empty())
		return s;

	std::sort(v.begin(), v.end());
	const size_t n = v.size();
	s.count = n;
	s.avgMs = sum / double(n);
	s.minMs = v.front();
	s.maxMs = v.back();
	s.p50Ms = v[rankIndex(n, 0.50)];
	s.p95Ms = v[rankIndex(n, 0.95)];
	s.p99Ms = v[rankIndex(n, 0.99)];
	const double w1 = meanOfSlowest(v, 0.01);
	const double w01 = meanOfSlowest(v, 0.001);
	s.low1Fps = w1 > 0 ? 1000.0 / w1 : FrameTimeSummary::kNA;
	s.low01Fps = w01 > 0 ? 1000.0 / w01 : FrameTimeSummary::kNA;
	return s;
}

} // namespace pml::metrics
