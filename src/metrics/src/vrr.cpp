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
		return VrrReadout::kNA;
	const size_t mid = v.size() / 2;
	std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(mid), v.end());
	return v[mid];
}

// q-quantile (0..1) of a sorted vector, nearest rank.
double quantile(const std::vector<double> &sorted, double q)
{
	if (sorted.empty())
		return VrrReadout::kNA;
	const size_t i = size_t(std::lround(q * double(sorted.size() - 1)));
	return sorted[std::min(i, sorted.size() - 1)];
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

constexpr double kUnevenFloorMs = 2.0;      // a jump smaller than this is never "uneven"
constexpr double kUnevenShare = 0.25;       // ... nor one under 25 % of the median interval
constexpr double kStutterFactor = 2.5;      // on screen this many medians: a stutter
constexpr double kUnstablePct = 15.0;
constexpr double kJitteryPct = 5.0;
constexpr double kEventsPerSecond = 0.5;    // stutters or LFC crossings that make the verdict
constexpr double kVaryingPct = 15.0;

} // namespace

VrrReadout analyzeVrr(std::span<const VrrFrame> frames, const VrrContext &context, const VrrOptions &options)
{
	VrrReadout out;
	// Displayed frames only; the game's interval may be unknown on a few.
	std::vector<VrrFrame> shown;
	shown.reserve(frames.size());
	for (const VrrFrame &f : frames)
		if (usable(f.displayMs))
			shown.push_back(f);
	out.frames = shown.size();
	if (shown.size() < VrrReadout::kMinFrames)
		return out;

	// --- State, from every frame given ---
	std::vector<double> d;
	d.reserve(shown.size());
	for (const VrrFrame &f : shown)
		d.push_back(f.displayMs);
	std::vector<double> p;   // the game's intervals of the displayed frames that have one
	p.reserve(shown.size());
	for (const VrrFrame &f : shown)
		if (usable(f.presentMs))
			p.push_back(f.presentMs);

	// Refresh grid: the output's period when known, else a common rate the screen updates snap to.
	double refreshHz = context.refreshHz > 0 ? context.refreshHz : 0.0;
	double periodMs = refreshHz > 0 ? 1000.0 / refreshHz : 0.0;
	bool snapped = periodMs > 0 && snaps(d, p, periodMs);
	if (periodMs <= 0)
	{
		for (double hz : kCommonHz)
			if (snaps(d, p, 1000.0 / hz))
			{
				refreshHz = hz;
				periodMs = 1000.0 / hz;
				snapped = true;
				break;
			}
	}
	if (periodMs > 0)
	{
		out.onGridPct = 100.0 * onGridShare(d, periodMs);
		out.refreshHz = refreshHz;
	}

	// At the cap: the game itself runs at or above the refresh rate (its median interval; the screen's median is
	// no good: at 90 fps on 120 Hz two of three updates are one refresh apart).
	const double paceMs = p.size() >= d.size() / 2 ? median(p) : median(d);
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

	// --- The judged frames: the last recentMs of screen time, at least kMinFrames ---
	size_t first = 0;
	if (options.recentMs > 0)
	{
		double span = 0;
		first = shown.size();
		while (first > 0 && (span < options.recentMs || shown.size() - first < VrrReadout::kMinFrames))
			span += shown[--first].displayMs;
	}
	const std::span<const VrrFrame> judged(shown.data() + first, shown.size() - first);
	out.judgedFrames = judged.size();

	std::vector<double> rates;
	rates.reserve(judged.size());
	double judgedMs = 0;
	for (const VrrFrame &f : judged)
	{
		judgedMs += f.displayMs;
		rates.push_back(1000.0 / f.displayMs);
	}
	out.judgedSeconds = judgedMs / 1000.0;
	std::sort(rates.begin(), rates.end());
	out.lowHz = quantile(rates, 0.05);
	out.highHz = quantile(rates, 0.95);
	out.medianHz = quantile(rates, 0.5);
	out.swingHz = (out.highHz - out.lowHz) / 2.0;

	// Now: the frames of the last nowMs of screen time (at least one).
	{
		double span = 0;
		size_t n = 0;
		for (size_t i = shown.size(); i > 0 && (n == 0 || span < options.nowMs); i--)
		{
			span += shown[i - 1].displayMs;
			n++;
		}
		out.hz = 1000.0 * double(n) / span;
	}

	// Drift: the rate of consecutive nowMs slices of the judged frames (an incomplete last slice is left out).
	{
		const double sliceMs = options.nowMs > 0 ? options.nowMs : 250.0;
		double lo = 0, hi = 0, span = 0;
		size_t n = 0, slices = 0;
		for (const VrrFrame &f : judged)
		{
			span += f.displayMs;
			n++;
			if (span >= sliceMs)
			{
				const double r = 1000.0 * double(n) / span;
				lo = slices ? std::min(lo, r) : r;
				hi = slices ? std::max(hi, r) : r;
				slices++;
				span = 0;
				n = 0;
			}
		}
		out.driftPct = slices >= 2 ? 100.0 * (hi - lo) / out.medianHz : 0.0;
	}

	// Events over consecutive judged frames.
	std::vector<double> jd;
	jd.reserve(judged.size());
	for (const VrrFrame &f : judged)
		jd.push_back(f.displayMs);
	const double med = median(jd);
	const double threshold = std::max(kUnevenFloorMs, kUnevenShare * med);
	const double lfcMs = context.vrrMinHz > 0 ? 1000.0 / context.vrrMinHz : 0.0;
	size_t pairs = 0, uneven = 0, unevenDisplay = 0, stutters = 0, crossings = 0, below = 0;
	for (size_t i = 0; i < judged.size(); i++)
	{
		const double di = judged[i].displayMs;
		if (lfcMs > 0 && di > lfcMs)
			below++;
		if (di > kStutterFactor * med)
			stutters++;
		if (i == 0)
			continue;
		pairs++;
		const double dp = judged[i - 1].displayMs;
		if (std::fabs(di - dp) > threshold)
		{
			uneven++;
			const float pi = judged[i].presentMs, pp = judged[i - 1].presentMs;
			if (usable(pi) && usable(pp) && std::fabs(double(pi) - double(pp)) <= threshold / 2)
				unevenDisplay++;
		}
		if (lfcMs > 0 && (di > lfcMs) != (dp > lfcMs))
			crossings++;
	}
	out.unevenPct = pairs ? 100.0 * double(uneven) / double(pairs) : 0.0;
	out.unevenDisplayPct = pairs ? 100.0 * double(unevenDisplay) / double(pairs) : 0.0;
	out.stutters = stutters;
	if (lfcMs > 0)
	{
		out.lfcCrossings = double(crossings);
		out.belowRangePct = 100.0 * double(below) / double(judged.size());
	}

	if (out.state == VrrState::Off)
	{
		// What a monitor shows with VRR off: its fixed rate, still.
		if (!std::isnan(out.refreshHz))
		{
			out.hz = out.refreshHz;
			out.lowHz = out.highHz = out.refreshHz;
		}
		out.swingHz = 0;
		out.stability = VrrStability::Fixed;
		return out;
	}

	// Stutters and LFC crossings count from kEventsPerSecond, at least one: one in the live 2 s (which ends a
	// frame past 2 s), three in a 7 s capture.
	const double events = std::max(1.0, std::floor(kEventsPerSecond * out.judgedSeconds));
	if (out.unevenPct >= kUnstablePct)
		out.stability = VrrStability::Unstable;
	else if (double(stutters) >= events)
		out.stability = VrrStability::Stutter;
	else if (double(crossings) >= events)
		out.stability = VrrStability::Lfc;
	else if (out.unevenPct >= kJitteryPct)
		out.stability = VrrStability::Jittery;
	else if (out.state == VrrState::Idle)
		out.stability = VrrStability::AtMax;
	else if (out.driftPct >= kVaryingPct)
		out.stability = VrrStability::Varying;
	else
		out.stability = VrrStability::Stable;

	if (out.stability == VrrStability::Unstable || out.stability == VrrStability::Jittery)
		out.cause = 2 * unevenDisplay > uneven ? VrrCause::Display : VrrCause::Game;
	return out;
}

const char *vrrStabilityLabel(VrrStability stability)
{
	switch (stability)
	{
	case VrrStability::None: return "";
	case VrrStability::Fixed: return "fixed";
	case VrrStability::Stable: return "stable";
	case VrrStability::Varying: return "varying";
	case VrrStability::AtMax: return "at max";
	case VrrStability::Jittery: return "jittery";
	case VrrStability::Stutter: return "stutter";
	case VrrStability::Lfc: return "LFC";
	case VrrStability::Unstable: return "unstable";
	}
	return "";
}

int vrrStabilitySeverity(VrrStability stability)
{
	switch (stability)
	{
	case VrrStability::None: return -1;
	case VrrStability::Fixed:
	case VrrStability::Stable:
	case VrrStability::Varying:
	case VrrStability::AtMax: return 0;
	case VrrStability::Jittery:
	case VrrStability::Stutter:
	case VrrStability::Lfc: return 1;
	case VrrStability::Unstable: return 2;
	}
	return -1;
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
	}
	return "";
}

} // namespace pml::metrics
