// SPDX-License-Identifier: MIT
#include <pml/metrics/vrr.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pml::metrics {

namespace {

bool usable(float v) { return v > 0.0f && std::isfinite(v); }

double median(std::vector<double> v)
{
	if (v.empty())
		return VrrVerdict::kNA;
	const size_t mid = v.size() / 2;
	std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(mid), v.end());
	return v[mid];
}

// Share (0-1) of the values that sit on a multiple of periodMs.
double onGridShare(const std::vector<double> &values, double periodMs)
{
	if (values.empty() || !(periodMs > 0))
		return 0;
	size_t on = 0;
	for (double x : values)
	{
		const double k = std::round(x / periodMs);
		if (k >= 1 && std::fabs(x - k * periodMs) < 0.12)
			on++;
	}
	return double(on) / double(values.size());
}

// Fixed refresh shows as screen updates snapping to the refresh grid while the game's own intervals do not
// (under VRR, or with tearing, the screen follows the game). When the game itself runs on the grid (FIFO at
// the refresh rate, a 120 fps cap on 240 Hz) nothing can be told: no snap.
bool snaps(const std::vector<double> &display, const std::vector<double> &present, double periodMs)
{
	return onGridShare(display, periodMs) >= 0.8 && onGridShare(present, periodMs) < 0.5;
}

// Common refresh rates, tried when the output's is unknown (the in-game overlay). Lowest first: a 120 Hz grid
// is also a 240 and a 480 Hz grid, and the lowest rate that explains the snap is the real one.
constexpr double kCommonHz[] = {48, 50, 59.94, 60, 72, 75, 85, 90, 100, 119.88, 120, 144, 155, 160, 165, 170,
                                 175, 180, 200, 240, 280, 300, 360, 390, 480, 500};

} // namespace

VrrVerdict analyzeVrr(std::span<const VrrFrame> frames, const VrrContext &context)
{
	VrrVerdict out;
	// Displayed frames only; the game's interval may be unknown on a few.
	std::vector<VrrFrame> shown;
	shown.reserve(frames.size());
	for (const VrrFrame &f : frames)
		if (usable(f.displayMs))
			shown.push_back(f);
	out.frames = shown.size();
	if (shown.size() < VrrVerdict::kMinFrames)
		return out;

	std::vector<double> d;
	d.reserve(shown.size());
	for (const VrrFrame &f : shown)
		d.push_back(f.displayMs);
	const double med = median(d);
	out.medianDisplayMs = med;
	std::vector<double> p;   // the game's intervals of the displayed frames that have one
	p.reserve(shown.size());
	for (const VrrFrame &f : shown)
		if (usable(f.presentMs))
			p.push_back(f.presentMs);

	// Refresh grid: the output's period when known, else a common rate the screen updates snap to.
	double periodMs = context.refreshHz > 0 ? 1000.0 / context.refreshHz : 0.0;
	bool snapped = periodMs > 0 && snaps(d, p, periodMs);
	if (periodMs <= 0)
	{
		for (double hz : kCommonHz)
			if (snaps(d, p, 1000.0 / hz))
			{
				periodMs = 1000.0 / hz;
				snapped = true;
				break;
			}
	}
	if (periodMs > 0)
	{
		out.onGridPct = 100.0 * onGridShare(d, periodMs);
		out.refreshHz = 1000.0 / periodMs;
	}

	// State. At the cap: the game itself runs at or above the refresh rate (its median interval; the
	// screen's median is no good: at 90 fps on 120 Hz two of three updates are one refresh apart).
	const double paceMs = p.size() >= d.size() / 2 ? median(p) : med;
	const bool atCap = periodMs > 0 && paceMs <= periodMs * 1.03;
	if (context.driver == DriverVrr::Off)
		out.state = VrrState::Off;
	else if (atCap)
		out.state = VrrState::Idle;
	else if (context.driver == DriverVrr::On)
		out.state = VrrState::Active;
	else if (snapped)
		out.state = VrrState::Off;
	else if (context.refreshHz > 0 && onGridShare(p, periodMs) < 0.5)
		out.state = VrrState::Active;   // the screen follows a game that is off the known grid
	else
		out.state = VrrState::Unknown;

	// Events over consecutive displayed frames.
	const double threshold = std::max(2.0, 0.25 * med);
	const double lfcMs = context.vrrMinHz > 0 ? 1000.0 / context.vrrMinHz : 0.0;
	size_t pairs = 0, uneven = 0, unevenDisplay = 0, stutters = 0, crossings = 0, below = 0;
	for (size_t i = 0; i < shown.size(); i++)
	{
		const double di = shown[i].displayMs;
		if (lfcMs > 0 && di > lfcMs)
			below++;
		if (di > 2.5 * med)
			stutters++;
		if (i == 0)
			continue;
		pairs++;
		const double dp = shown[i - 1].displayMs;
		if (std::fabs(di - dp) > threshold)
		{
			uneven++;
			const float pi = shown[i].presentMs, pp = shown[i - 1].presentMs;
			if (usable(pi) && usable(pp) && std::fabs(double(pi) - double(pp)) <= threshold / 2)
				unevenDisplay++;
		}
		if (lfcMs > 0 && (di > lfcMs) != (dp > lfcMs))
			crossings++;
	}
	out.unevenPct = 100.0 * double(uneven) / double(pairs);
	out.unevenDisplayPct = 100.0 * double(unevenDisplay) / double(pairs);
	out.stutterPct = 100.0 * double(stutters) / double(shown.size());
	if (lfcMs > 0)
	{
		out.lfcCrossings = double(crossings);
		out.belowRangePct = 100.0 * double(below) / double(shown.size());
	}

	if (out.state == VrrState::Off)
		return out;   // fixed refresh: no VRR score

	// A stutter also shows up as two uneven pairs; it adds two more to weigh 3 in total. LFC crossings
	// weigh 3 on top of the uneven pairs they usually are.
	const double events = double(uneven) + 2.0 * double(stutters) + 3.0 * double(crossings);
	const double rate = events / double(pairs);
	out.score = int(std::lround(100.0 * std::exp(-rate / 0.25)));

	if (out.score < 85)
	{
		const double game = double(uneven - unevenDisplay);
		const double display = double(unevenDisplay);
		const double stutter = 3.0 * double(stutters);
		const double lfc = 3.0 * double(crossings);
		const double top = std::max({game, display, stutter, lfc});
		out.cause = top == lfc && lfc > 0 ? VrrCause::Lfc
		          : top == stutter && stutter > 0 ? VrrCause::Stutter
		          : top == display ? VrrCause::Display
		                           : VrrCause::Game;
	}
	return out;
}

const char *vrrVerdictLabel(int score)
{
	if (score < 0)
		return "";
	if (score >= 85)
		return "stable";
	if (score >= 65)
		return "minor jitter";
	if (score >= 40)
		return "unstable";
	return "very unstable";
}

const char *vrrStateLabel(VrrState state)
{
	switch (state)
	{
	case VrrState::NoData: return "no data";
	case VrrState::Unknown: return "unknown";
	case VrrState::Off: return "off";
	case VrrState::Idle: return "idle";
	case VrrState::Active: return "active";
	}
	return "";
}

const char *vrrCauseLabel(VrrCause cause)
{
	switch (cause)
	{
	case VrrCause::None: return "";
	case VrrCause::Display: return "uneven on screen, game steady";
	case VrrCause::Game: return "game frame times swing";
	case VrrCause::Stutter: return "stutters";
	case VrrCause::Lfc: return "crossing the VRR floor (LFC)";
	}
	return "";
}

} // namespace pml::metrics
