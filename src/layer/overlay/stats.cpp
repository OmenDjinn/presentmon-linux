// SPDX-License-Identifier: MIT
#include "stats.hpp"

#include <pml/metrics/summary.hpp>

#include <algorithm>

namespace pml::layer::overlay {

using pml::metrics::DisplayState;
using pml::metrics::IsMissingFrameMetricValue;

namespace {

constexpr uint64_t kRecomputeIntervalNs = 250'000'000;
constexpr size_t kMaxQueued = 4096;

} // namespace

OverlayStats::OverlayStats(const pml::SwapchainInfo &info, uint32_t pid, uint64_t sessionStartNs)
    : info_(info), pid_(pid), sessionStartNs_(sessionStartNs)
{
	reset();
}

void OverlayStats::reset()
{
	const auto ctx = pml::metrics::SwapchainContext::fromInfo(pid_, info_);
	engineV1_ = std::make_unique<pml::metrics::SwapchainEngine>(ctx, pml::metrics::MetricsMode::V1, sessionStartNs_);
	engineV2_ = std::make_unique<pml::metrics::SwapchainEngine>(ctx, pml::metrics::MetricsMode::V2, sessionStartNs_);
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		queue_.clear();
	}
	samples_.clear();
	displayed_.clear();
	rows_ = 0;
	for (auto &b : buckets_)
		b = Bucket{};
	newestBucket_ = -1;
	newestNs_ = 0;
	lastRecomputeNs_ = 0;
	snapshot_ = Snapshot{};
}

void OverlayStats::setCollecting(bool on)
{
	collecting_.store(on, std::memory_order_release);
}

bool OverlayStats::collecting() const
{
	return collecting_.load(std::memory_order_acquire);
}

void OverlayStats::push(const pml::FrameRecord &record)
{
	lastPresentMode.store(record.presentMode, std::memory_order_relaxed);
	if (!collecting())
		return;
	std::lock_guard<std::mutex> lock(queueMutex_);
	if (queue_.size() >= kMaxQueued)
		queue_.erase(queue_.begin(), queue_.begin() + kMaxQueued / 2);  // the presenting thread is not draining
	queue_.push_back(record);
}

void OverlayStats::addPresentRow(const pml::metrics::Row &row)
{
	if (onPresentRow)
		onPresentRow(row);
	rows_++;
	if (!row.isAppFrame)
		return;
	const auto &m = row.metrics;
	Sample s;
	s.tNs = m.presentStartQpc;
	s.display = row.display;
	s.betweenPresentsMs = float(m.msBetweenPresents);
	s.cpuBusyMs = float(m.msCPUBusy);
	s.cpuWaitMs = float(m.msCPUWait);
	if (row.frame.gpuStartTime != 0 && !IsMissingFrameMetricValue(m.msGPUBusy))
	{
		s.gpuBusyMs = float(m.msGPUBusy);
		s.gpuWaitMs = float(m.msGPUWait);
		s.gpuLatencyMs = float(m.msGPULatency);
	}
	if (s.tNs == 0 || !(s.betweenPresentsMs >= 0))
		return;

	samples_.push_back(s);
	newestNs_ = std::max(newestNs_, s.tNs);
	while (samples_.size() > kMaxSamples || (!samples_.empty() && newestNs_ - samples_.front().tNs > kLongWindowNs))
		samples_.pop_front();

	// Graph bucket (max of the frame times that fall into it).
	const int64_t id = int64_t(s.tNs / kGraphBucketNs);
	Bucket &b = buckets_[size_t(id % kGraphPoints)];
	if (b.id != id)
		b = Bucket{id, 0, 0};
	b.present = std::max(b.present, s.betweenPresentsMs);
	newestBucket_ = std::max(newestBucket_, id);
}

void OverlayStats::addDisplayRow(const pml::metrics::Row &row)
{
	if (onDisplayRow)
		onDisplayRow(row);
	if (!row.isAppFrame || !row.hasDisplayMetrics || row.metrics.screenTimeQpc == 0)
		return;
	const auto &m = row.metrics;
	DisplaySample d;
	d.tNs = m.presentStartQpc;
	d.screenNs = m.screenTimeQpc;
	d.betweenDisplayMs = float(m.msBetweenDisplayChange);
	d.untilDisplayedMs = float(m.msUntilDisplayed);
	d.displayLatencyMs = float(m.msDisplayLatency);
	d.betweenPresentsMs = float(m.msBetweenPresents);
	if (!IsMissingFrameMetricValue(m.msAnimationError))
		d.animationErrorMs = float(m.msAnimationError);
	if (!IsMissingFrameMetricValue(m.msReadyTimeToDisplayLatency) && m.msReadyTimeToDisplayLatency >= 0)
		d.readyToScreenMs = float(m.msReadyTimeToDisplayLatency);
	displayed_.push_back(d);
	while (displayed_.size() > kMaxSamples ||
	       (!displayed_.empty() && d.screenNs > displayed_.front().screenNs && d.screenNs - displayed_.front().screenNs > kLongWindowNs))
		displayed_.pop_front();

	const int64_t id = int64_t(d.tNs / kGraphBucketNs);
	Bucket &b = buckets_[size_t(id % kGraphPoints)];
	if (b.id != id)
		b = Bucket{id, 0, 0};
	b.display = std::max(b.display, d.betweenDisplayMs);
	newestBucket_ = std::max(newestBucket_, id);
}

void OverlayStats::update(uint64_t nowNs)
{
	drain_.clear();   // left over only if processing threw last time
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		drain_.swap(queue_);
	}
	for (const pml::FrameRecord &rec : drain_)
	{
		for (const pml::metrics::Row &row : engineV1_->process(rec))
			addPresentRow(row);
		for (const pml::metrics::Row &row : engineV2_->process(rec))
			addDisplayRow(row);
	}
	drain_.clear();

	if (nowNs - lastRecomputeNs_ >= kRecomputeIntervalNs || lastRecomputeNs_ == 0)
		recompute(nowNs);
	rebuildGraph();
}

void OverlayStats::recompute(uint64_t nowNs)
{
	lastRecomputeNs_ = nowNs;
	Snapshot s;
	s.rowsTotal = rows_;
	// The graph is rebuilt separately; keep what is there.
	std::copy(std::begin(snapshot_.graphPresent), std::end(snapshot_.graphPresent), s.graphPresent);
	std::copy(std::begin(snapshot_.graphDisplay), std::end(snapshot_.graphDisplay), s.graphDisplay);
	s.graphCount = snapshot_.graphCount;
	s.graphMaxMs = snapshot_.graphMaxMs;

	if (samples_.size() < 2)
	{
		snapshot_ = s;
		return;
	}
	s.valid = true;
	s.idle = nowNs > newestNs_ && nowNs - newestNs_ > kStaleNs;

	// Short window: the last second of presents, measured against the newest sample (the data lags
	// the application by a few frames, so "now" would shave the span).
	size_t first = samples_.size();
	while (first > 0 && newestNs_ - samples_[first - 1].tNs <= kShortWindowNs)
		first--;
	const size_t n = samples_.size() - first;

	double busy = 0, wait = 0, gBusy = 0, gWait = 0, gLat = 0;
	size_t gpuN = 0;
	for (size_t i = first; i < samples_.size(); i++)
	{
		busy += samples_[i].cpuBusyMs;
		wait += samples_[i].cpuWaitMs;
		if (samples_[i].gpuBusyMs >= 0)
		{
			gBusy += samples_[i].gpuBusyMs;
			gWait += samples_[i].gpuWaitMs;
			gLat += samples_[i].gpuLatencyMs;
			gpuN++;
		}
	}
	// GPU numbers once at least half of the window's frames have them (a few frames without GPU data,
	// e.g. lost reports, do not blank the row; a swapchain without GPU timing shows NA).
	if (gpuN > 0 && gpuN * 2 >= n)
	{
		s.gpuBusyMs = gBusy / double(gpuN);
		s.gpuWaitMs = gWait / double(gpuN);
		s.gpuLatencyMs = gLat / double(gpuN);
	}
	if (n >= 2)
	{
		const double span = double(samples_.back().tNs - samples_[first].tNs) * 1e-9;
		if (span > 0)
			s.presentFps = double(n - 1) / span;
		s.cpuBusyMs = busy / double(n);
		s.cpuWaitMs = wait / double(n);
	}

	// Long window: percentiles and drops.
	std::vector<float> frameTimes;
	frameTimes.reserve(samples_.size());
	for (const Sample &x : samples_)
	{
		if (x.betweenPresentsMs > 0)
			frameTimes.push_back(x.betweenPresentsMs);
		if (x.display != DisplayState::Unknown)
		{
			s.displayKnownCount++;
			if (x.display == DisplayState::NotDisplayed)
				s.droppedCount++;
		}
	}
	s.hasDisplayInfo = s.displayKnownCount > 0;
	if (s.hasDisplayInfo)
		s.droppedPct = 100.0 * double(s.droppedCount) / double(s.displayKnownCount);

	// Average, p99 and lows (fps of the mean of the slowest 1 % / 0.1 % of the frames): the same
	// definitions as the GUI's capture analysis (pml/metrics/summary.hpp).
	const pml::metrics::FrameTimeSummary times = pml::metrics::summarizeFrameTimes(frameTimes);
	if (times.count > 0)
	{
		s.avgMs = times.avgMs;
		s.p99Ms = times.p99Ms;
		s.low1Fps = times.low1Fps;
		s.low01Fps = times.low01Fps;
		s.gqi = times.gqi;
	}

	if (s.hasDisplayInfo)
	{
		// V2 samples of the last second, measured against the newest screen time.
		uint64_t newestScreen = 0;
		for (const DisplaySample &d : displayed_)
			newestScreen = std::max(newestScreen, d.screenNs);
		uint64_t minScreen = ~0ull, maxScreen = 0;
		size_t screenN = 0, readyN = 0;
		double latency = 0, until = 0, ready = 0;
		for (size_t i = displayed_.size(); i > 0; i--)
		{
			const DisplaySample &d = displayed_[i - 1];
			if (newestScreen - d.screenNs > kShortWindowNs)
				break;
			minScreen = std::min(minScreen, d.screenNs);
			maxScreen = std::max(maxScreen, d.screenNs);
			screenN++;
			latency += d.displayLatencyMs;
			until += d.untilDisplayedMs;
			if (d.readyToScreenMs >= 0)
			{
				ready += d.readyToScreenMs;
				readyN++;
			}
		}
		if (screenN >= 2 && maxScreen > minScreen)
			s.displayFps = double(screenN - 1) / (double(maxScreen - minScreen) * 1e-9);
		if (screenN)
		{
			s.displayLatencyMs = latency / double(screenN);
			s.untilDisplayedMs = until / double(screenN);
		}
		if (readyN)
			s.readyToScreenMs = ready / double(readyN);

		// VRR verdict over the displayed frames of the long window: a steady reading rather than a
		// per-second one (single hitches should move it, not dominate it).
		vrrScratch_.clear();
		for (const DisplaySample &d : displayed_)
			vrrScratch_.push_back({d.betweenPresentsMs, d.betweenDisplayMs});
		pml::metrics::VrrContext ctx = vrrContext_;
		if (ctx.refreshHz <= 0 && info_.refreshDurationNs)
			ctx.refreshHz = 1e9 / double(info_.refreshDurationNs);
		s.vrr = pml::metrics::analyzeVrr(vrrScratch_, ctx);

		// Experience score over the same frames (pml/metrics/experience.hpp).
		experienceScratch_.clear();
		for (const DisplaySample &d : displayed_)
			experienceScratch_.push_back({d.betweenDisplayMs, d.animationErrorMs, d.displayLatencyMs});
		s.experience = pml::metrics::analyzeExperience(experienceScratch_);
	}

	if (s.idle)
	{
		s.presentFps = 0;
		if (s.hasDisplayInfo)
			s.displayFps = 0;
	}
	snapshot_ = s;
}

void OverlayStats::rebuildGraph()
{
	Snapshot &s = snapshot_;
	if (newestBucket_ < 0)
	{
		s.graphCount = 0;
		return;
	}
	float lastP = 0, lastD = 0, maxMs = 0;
	int have = 0;
	for (int k = 0; k < kGraphPoints; k++)
	{
		const int64_t id = newestBucket_ - (kGraphPoints - 1) + k;
		float p = 0, d = 0;
		bool present = false;
		if (id >= 0)
		{
			const Bucket &b = buckets_[size_t(id % kGraphPoints)];
			if (b.id == id)
			{
				p = b.present;
				d = b.display;
				present = true;
			}
		}
		// A bucket without a frame (low fps) repeats the previous value, so the line stays continuous.
		if (!present)
		{
			p = lastP;
			d = lastD;
		}
		else
		{
			if (d == 0)
				d = lastD;
			have++;
		}
		lastP = p;
		lastD = d;
		s.graphPresent[k] = p;
		s.graphDisplay[k] = d;
		maxMs = std::max({maxMs, p, d});
	}
	s.graphCount = have > 1 ? kGraphPoints : 0;
	s.graphMaxMs = maxMs;
}

} // namespace pml::layer::overlay
