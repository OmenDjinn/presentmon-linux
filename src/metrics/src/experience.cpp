// SPDX-License-Identifier: MIT
#include "pml/metrics/experience.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace pml::metrics {

namespace {

// ITU-T G.1072 input-delay impairment, high-sensitivity games (A=90, B=1.191, C=0.009775, D=-18.73), clipped.
double g1072InputImpairment(double delayMs)
{
	const double v = 90.0 / (1.0 + std::exp(1.191 - 0.009775 * delayMs)) - 18.73;
	return std::clamp(v, 0.0, 68.98);
}

double median(std::vector<double> v)
{
	if (v.empty())
		return std::numeric_limits<double>::quiet_NaN();
	const size_t k = v.size() / 2;
	std::nth_element(v.begin(), v.begin() + ptrdiff_t(k), v.end());
	return v[k];
}

} // namespace

const char *experienceVerdictLabel(int score)
{
	if (score < 0)
		return "";
	if (score >= 85)
		return "smooth";
	if (score >= 65)
		return "minor issues";
	if (score >= 40)
		return "rough";
	return "very rough";
}

const char *experienceCauseLabel(ExperienceCause cause)
{
	switch (cause)
	{
	case ExperienceCause::Rate: return "low frame rate";
	case ExperienceCause::Motion: return "judder (uneven motion)";
	case ExperienceCause::Stutter: return "stutters";
	case ExperienceCause::Latency: return "high latency";
	case ExperienceCause::None: break;
	}
	return "";
}

std::string experienceDetail(const ExperienceVerdict &v)
{
	if (v.score < 0)
		return {};
	char buf[384];
	int n = std::snprintf(buf, sizeof(buf), "%.0f fps shown: rate -%.1f", v.displayFps, v.rate);
	if (std::isfinite(v.animationRmsMs))
		n += std::snprintf(buf + n, sizeof(buf) - size_t(n), " \u00b7 judder -%.1f (%.1f ms RMS)", v.motion, v.animationRmsMs);
	else
		n += std::snprintf(buf + n, sizeof(buf) - size_t(n), " \u00b7 judder n/a");
	n += std::snprintf(buf + n, sizeof(buf) - size_t(n), " \u00b7 stutters -%.1f (%u hitch%s", v.stutter, v.hitches,
	                   v.hitches == 1 ? "" : "es");
	if (v.hitches)
		n += std::snprintf(buf + n, sizeof(buf) - size_t(n), ", worst %.0f ms", v.worstHitchMs);
	n += std::snprintf(buf + n, sizeof(buf) - size_t(n), ")");
	if (std::isfinite(v.latencyMs))
		n += std::snprintf(buf + n, sizeof(buf) - size_t(n), " \u00b7 latency -%.1f (%.0f ms)", v.latency, v.latencyMs);
	else
		n += std::snprintf(buf + n, sizeof(buf) - size_t(n), " \u00b7 latency n/a");
	std::snprintf(buf + n, sizeof(buf) - size_t(n), " \u00b7 %zu frames", v.frames);
	return buf;
}

ExperienceVerdict analyzeExperience(std::span<const ExperienceFrame> frames)
{
	ExperienceVerdict s;
	std::vector<double> intervals;
	intervals.reserve(frames.size());
	for (const ExperienceFrame &f : frames)
		if (std::isfinite(f.displayIntervalMs) && f.displayIntervalMs > 0)
			intervals.push_back(f.displayIntervalMs);
	s.frames = intervals.size();
	if (intervals.size() < ExperienceVerdict::kMinFrames)
		return s;

	double total = 0;
	for (double v : intervals)
		total += v;
	const double meanMs = total / double(intervals.size());
	const double med = median(intervals);
	s.displayFps = 1000.0 / meanMs;

	// Motion rate.
	const double d = std::max(0.0, meanMs - 4.0);
	s.rate = std::min(60.0, d * d / (d + 3.0));

	// Hitches and judder.
	const double hitchCut = std::max(2.0 * med, med + 8.0) + 1.0;
	double hitchSum = 0, aeSquares = 0;
	size_t aeCount = 0;
	std::vector<double> latencies;
	for (const ExperienceFrame &f : frames)
	{
		if (!(std::isfinite(f.displayIntervalMs) && f.displayIntervalMs > 0))
			continue;
		const bool haveAe = std::isfinite(f.animationErrorMs);
		const double ae = haveAe ? std::fabs(double(f.animationErrorMs)) : 0.0;
		const double jump = std::max(double(f.displayIntervalMs) - med, ae);
		if (jump + med > hitchCut)
		{
			s.hitches++;
			hitchSum += jump;
			s.worstHitchMs = std::max(s.worstHitchMs, jump + med);
		}
		else if (haveAe)
		{
			aeSquares += ae * ae;
			aeCount++;
		}
		if (std::isfinite(f.latencyMs) && f.latencyMs > 0)
			latencies.push_back(f.latencyMs);
	}
	if (aeCount > 0)
	{
		s.animationRmsMs = std::sqrt(aeSquares / double(aeCount));
		s.motion = std::min(30.0, 3.0 * std::max(0.0, s.animationRmsMs - 1.0));
	}
	s.interruptMsPerS = hitchSum / (total / 1000.0);
	s.stutter = std::min(50.0, 0.1 * s.interruptMsPerS +
	                               25.0 * std::clamp((s.worstHitchMs - 50.0) / 100.0, 0.0, 1.75));

	// Latency.
	if (!latencies.empty())
	{
		s.latencyMs = median(std::move(latencies));
		s.latency = 0.848 * (g1072InputImpairment(s.latencyMs) - g1072InputImpairment(0.0));
	}

	s.score = int(std::lround(std::clamp(100.0 - s.rate - s.motion - s.stutter - s.latency, 0.0, 100.0)));

	if (s.score < 85)
	{
		const double terms[] = {s.rate, s.motion, s.stutter, s.latency};
		const ExperienceCause causes[] = {ExperienceCause::Rate, ExperienceCause::Motion, ExperienceCause::Stutter,
		                                  ExperienceCause::Latency};
		double worst = 0;
		for (size_t i = 0; i < 4; i++)
			if (terms[i] > worst)
			{
				worst = terms[i];
				s.cause = causes[i];
			}
	}
	return s;
}

} // namespace pml::metrics
