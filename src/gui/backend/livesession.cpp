// SPDX-License-Identifier: MIT
#include "livesession.hpp"

#include "format.hpp"

#include <pml/clock.hpp>
#include <pml/shm.hpp>

#include <stats.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <vector>

namespace {

using pml::layer::overlay::OverlayStats;
using pml::layer::overlay::Snapshot;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kTickMs = 16;                                // ~60 Hz
constexpr uint64_t kRetryNs = 500'000'000;                 // attach retry, table scan, liveness check
constexpr uint64_t kCountWindowNs = 1'000'000'000;         // "busiest" = most presents in this window
constexpr uint64_t kSwitchDwellNs = 1'000'000'000;         // another swapchain must lead this long to take over
constexpr size_t kReadBatch = 512;
constexpr size_t kMaxTimes = 1 << 16;
// A destroyed swapchain that has not presented for this long (longer than the statistics' windows)
// is forgotten, so repeated resizes do not pile up engines.
constexpr uint64_t kPruneAgeNs = 15'000'000'000;

bool same(double a, double b) { return a == b || (std::isnan(a) && std::isnan(b)); }

// The numbers QML sees (a Snapshot without its graph arrays).
struct Numbers
{
	bool valid = false, idle = false, hasDisplayInfo = false;
	double presentFps = kNaN, displayFps = kNaN, avgMs = kNaN, p99Ms = kNaN, low1Fps = kNaN, low01Fps = kNaN, gqi = kNaN;
	double cpuBusyMs = kNaN, cpuWaitMs = kNaN, displayLatencyMs = kNaN, untilDisplayedMs = kNaN;
	double gpuBusyMs = kNaN, gpuWaitMs = kNaN, gpuLatencyMs = kNaN;
	double readyToScreenMs = kNaN, droppedPct = kNaN;
	qulonglong lost = 0;
	double vrrHz = kNaN, vrrSwingHz = kNaN, vrrLowHz = kNaN, vrrHighHz = kNaN, vrrMinHz = kNaN, vrrMaxHz = kNaN;
	int vrrSeverity = -1;
	QString vrrState, vrrVerdict, vrrCause, vrrDetail, vrrSource;

	bool operator==(const Numbers &o) const
	{
		return valid == o.valid && idle == o.idle && hasDisplayInfo == o.hasDisplayInfo && lost == o.lost &&
		       same(presentFps, o.presentFps) && same(displayFps, o.displayFps) && same(avgMs, o.avgMs) &&
		       same(p99Ms, o.p99Ms) && same(low1Fps, o.low1Fps) && same(low01Fps, o.low01Fps) && same(gqi, o.gqi) &&
		       same(cpuBusyMs, o.cpuBusyMs) && same(cpuWaitMs, o.cpuWaitMs) && same(gpuBusyMs, o.gpuBusyMs) &&
		       same(gpuWaitMs, o.gpuWaitMs) && same(gpuLatencyMs, o.gpuLatencyMs) &&
		       same(displayLatencyMs, o.displayLatencyMs) && same(untilDisplayedMs, o.untilDisplayedMs) &&
		       same(readyToScreenMs, o.readyToScreenMs) && same(droppedPct, o.droppedPct) && same(vrrHz, o.vrrHz) &&
		       same(vrrSwingHz, o.vrrSwingHz) && same(vrrLowHz, o.vrrLowHz) && same(vrrHighHz, o.vrrHighHz) &&
		       same(vrrMinHz, o.vrrMinHz) && same(vrrMaxHz, o.vrrMaxHz) && vrrSeverity == o.vrrSeverity &&
		       vrrState == o.vrrState && vrrVerdict == o.vrrVerdict && vrrCause == o.vrrCause &&
		       vrrDetail == o.vrrDetail && vrrSource == o.vrrSource;
	}
};

Numbers fromSnapshot(const Snapshot &s, const pml::metrics::VrrContext &context)
{
	Numbers n;
	n.valid = s.valid;
	n.idle = s.idle;
	n.hasDisplayInfo = s.hasDisplayInfo;
	n.presentFps = s.presentFps;
	n.displayFps = s.displayFps;
	n.avgMs = s.avgMs;
	n.p99Ms = s.p99Ms;
	n.low1Fps = s.low1Fps;
	n.low01Fps = s.low01Fps;
	n.gqi = s.gqi;
	n.cpuBusyMs = s.cpuBusyMs;
	n.cpuWaitMs = s.cpuWaitMs;
	n.gpuBusyMs = s.gpuBusyMs;
	n.gpuWaitMs = s.gpuWaitMs;
	n.gpuLatencyMs = s.gpuLatencyMs;
	n.displayLatencyMs = s.displayLatencyMs;
	n.untilDisplayedMs = s.untilDisplayedMs;
	n.readyToScreenMs = s.readyToScreenMs;
	n.droppedPct = s.droppedPct;
	const pml::metrics::VrrReadout &v = s.vrr;
	n.vrrHz = v.hz;
	n.vrrSwingHz = v.swingHz;
	n.vrrLowHz = v.lowHz;
	n.vrrHighHz = v.highHz;
	n.vrrMinHz = context.vrrMinHz > 0 ? context.vrrMinHz : kNaN;
	n.vrrMaxHz = v.refreshHz;
	n.vrrSeverity = pml::metrics::vrrStabilitySeverity(v.stability);
	n.vrrState = QString::fromLatin1(pml::metrics::vrrStateLabel(v.state));
	n.vrrVerdict = QString::fromLatin1(pml::metrics::vrrStabilityLabel(v.stability));
	n.vrrCause = QString::fromLatin1(pml::metrics::vrrCauseLabel(v.cause));
	n.vrrDetail = pml::gui::vrrDetailText(v);
	return n;
}

// One swapchain of the process: its own statistics, like the overlay keeps per swapchain.
struct Chain
{
	pml::SwapchainInfo info = {};
	std::unique_ptr<OverlayStats> stats;
	std::deque<uint64_t> times;   // present start of the records seen, ascending (for the busiest-swapchain vote)
	uint64_t newestNs = 0;
	bool known = false;     // found in the segment's swapchain table (a full table leaves records unknown)
	bool pending = false;   // records pushed since its last update() in this step
	bool needsRecompute = false;
};

} // namespace

struct LiveSession::Impl
{
	explicit Impl(LiveSession *owner) : q(owner) {}

	LiveSession *q;
	int pid = 0;
	bool running = true;
	double history = 10;

	FrameSeries frameTimes;
	FrameSeries displayTimes;
	bool frameDirty = false, displayDirty = false;

	std::unique_ptr<pml::Segment> segment;
	std::unique_ptr<pml::RingReader> reader;
	uint64_t lostBase = 0;      // lost count at attach: the history from before is not a loss
	std::vector<pml::FrameRecord> batch;   // reused read buffer (sizeof(FrameRecord) * kReadBatch = 160 KB)
	QString processName, attachError;
	uint64_t attachNs = 0;
	uint64_t nextRetryNs = 0;   // next attach attempt while detached
	uint64_t nextCheckNs = 0;   // next table scan / liveness check while attached
	bool dead = false;          // the producer is gone: numbers stay as they were

	std::map<uint64_t, Chain> chains;   // by swapchain serial
	uint64_t followed = 0;
	uint64_t candidate = 0;
	uint64_t candidateSince = 0;

	Numbers numbers;
	std::function<std::vector<pml::gui::OutputVrr>()> outputSource;
	std::vector<pml::gui::OutputVrr> outputs;   // as last read (twice a second while attached)
	QString vrrSource;
	QString swapchain;
	int swapchainCount = 0;
	double refreshHz = kNaN;

	double toX(uint64_t ns) const { return double(int64_t(ns) - int64_t(attachNs)) * 1e-9; }

	void resetData()
	{
		segment.reset();
		reader.reset();
		lostBase = 0;
		chains.clear();
		followed = candidate = 0;
		candidateSince = 0;
		dead = false;
		processName.clear();
		numbers = Numbers{};
		vrrSource.clear();
		swapchain.clear();
		swapchainCount = 0;
		refreshHz = kNaN;
		frameTimes.clear();
		displayTimes.clear();
		frameDirty = displayDirty = true;
	}

	void follow(uint64_t serial);
	Chain &chainFor(uint64_t serial);
	QString vrrSourceText(const pml::SwapchainInfo &info) const;
	void scanTable();
	uint64_t countRecent(const Chain &c, uint64_t refNs) const;
	void chooseFollowed(uint64_t nowNs);
	void pruneChains(uint64_t refNs);
};

void LiveSession::Impl::follow(uint64_t serial)
{
	if (followed != 0)
	{
		auto it = chains.find(followed);
		if (it != chains.end())
		{
			it->second.stats->onPresentRow = nullptr;
			it->second.stats->onDisplayRow = nullptr;
		}
	}
	followed = serial;
	candidate = 0;
	// A new swapchain has a different rhythm and resolution: its history starts empty.
	frameTimes.clear();
	displayTimes.clear();
	frameDirty = displayDirty = true;
	if (serial == 0)
		return;
	OverlayStats &stats = *chains.at(serial).stats;
	stats.onPresentRow = [this](const pml::metrics::Row &row) {
		const double ms = row.metrics.msBetweenPresents;
		if (!row.isAppFrame || !(ms > 0) || pml::metrics::IsMissingFrameMetricValue(ms) || row.metrics.presentStartQpc == 0)
			return;
		frameTimes.append(toX(row.metrics.presentStartQpc), ms);
		frameDirty = true;
	};
	stats.onDisplayRow = [this](const pml::metrics::Row &row) {
		const double ms = row.metrics.msBetweenDisplayChange;
		if (!row.isAppFrame || !row.hasDisplayMetrics || !(ms > 0) || pml::metrics::IsMissingFrameMetricValue(ms) ||
		    row.metrics.presentStartQpc == 0)
			return;
		displayTimes.append(toX(row.metrics.presentStartQpc), ms);
		displayDirty = true;
	};
}

// What the driver says about the output this swapchain is on, as text for the VRR panel.
QString LiveSession::Impl::vrrSourceText(const pml::SwapchainInfo &info) const
{
	const pml::gui::OutputVrr *used = nullptr;
	const pml::metrics::VrrContext ctx =
	    pml::gui::vrrContextFromOutputs(outputs, info.width, info.height, info.refreshDurationNs, &used);
	const QString name = used ? QString::fromStdString(used->name) : QString();
	if (ctx.driver == pml::metrics::DriverVrr::On)
	{
		const QString range =
		    used && used->edidMinHz > 0
		        ? QStringLiteral(", %1–%2 Hz")
		              .arg(used->edidMinHz, 0, 'f', 0)
		              .arg(std::min(used->refreshHz > 0 ? used->refreshHz : used->edidMaxHz, used->edidMaxHz), 0, 'f', 0)
		        : QString();
		return QStringLiteral("Driver: VRR on, %1%2").arg(name, range);
	}
	if (ctx.driver == pml::metrics::DriverVrr::Off)
		return used ? QStringLiteral("Driver: VRR off on %1, fixed %2 Hz (KWin turns it on only for the active fullscreen window)")
		                  .arg(name)
		                  .arg(used->refreshHz, 0, 'f', 0)
		            : QStringLiteral("Driver: VRR off on every screen (KWin turns it on only for the active fullscreen window)");
	return QStringLiteral("Driver: VRR state not readable");
}

Chain &LiveSession::Impl::chainFor(uint64_t serial)
{
	auto it = chains.find(serial);
	if (it != chains.end())
		return it->second;
	Chain c;
	c.known = pml::findSwapchain(*segment, serial, &c.info);
	if (!c.known)
		c.info.serial = serial;   // the record outran the table (or it is full): unknown rather than lost
	c.stats = std::make_unique<OverlayStats>(c.info, uint32_t(pid), attachNs);
	c.stats->setVrrContext(
	    pml::gui::vrrContextFromOutputs(outputs, c.info.width, c.info.height, c.info.refreshDurationNs));
	return chains.emplace(serial, std::move(c)).first->second;
}

// Refreshes the swapchain descriptions (destroyed flag, refresh duration appears later) and the
// live count; recomputes what the swapchain properties show.
void LiveSession::Impl::scanTable()
{
	const pml::SegmentHeader &h = segment->header();
	const uint32_t used = std::min(pml::atomicLoad(h.swapchainHighWater), pml::kMaxSwapchains);
	int live = 0;
	const pml::SwapchainInfo *newest = nullptr;
	std::vector<pml::SwapchainInfo> infos;
	infos.reserve(used);
	for (uint32_t i = 0; i < used; ++i)
	{
		pml::SwapchainInfo info;
		if (pml::readSwapchain(*segment, i, &info))
			infos.push_back(info);
	}
	for (const pml::SwapchainInfo &info : infos)
	{
		auto it = chains.find(info.serial);
		if (it != chains.end())
		{
			it->second.info = info;
			it->second.known = true;
		}
		if (info.destroyedNs != 0)
			continue;
		++live;
		if (!newest || info.createdNs > newest->createdNs)
			newest = &info;
	}
	swapchainCount = live;
	// Before the first record there is no followed swapchain: describe the newest live one.
	if (followed == 0)
	{
		swapchain = newest ? pml::gui::swapchainSummary(*newest) : QString();
		refreshHz = newest && newest->refreshDurationNs ? 1e9 / double(newest->refreshDurationNs) : kNaN;
		vrrSource = newest ? vrrSourceText(*newest) : QString();
	}
}

uint64_t LiveSession::Impl::countRecent(const Chain &c, uint64_t refNs) const
{
	uint64_t n = 0;
	for (auto it = c.times.rbegin(); it != c.times.rend() && refNs - *it <= kCountWindowNs; ++it)
		++n;
	return n;
}

// Follows the swapchain with the most presents in the last second. A swapchain takes over only when
// it has clearly more (1.5x + 2) for a full second: games with a second swapchain (a video
// player, an offscreen preview) would otherwise make the graph jump back and forth.
void LiveSession::Impl::chooseFollowed(uint64_t nowNs)
{
	if (chains.empty())
		return;
	uint64_t ref = 0;
	for (const auto &[serial, c] : chains)
		ref = std::max(ref, c.newestNs);

	// Destroyed swapchains do not vote (their last second of presents would keep them "busiest"
	// after a resize); if every swapchain is destroyed they all vote.
	// A swapchain the table does not know (it was full) never gets a destroyed flag: count it as live
	// only while it presented within the last second.
	auto isLive = [&](const Chain &c) {
		return c.info.destroyedNs == 0 && (c.known || (ref >= c.newestNs && ref - c.newestNs <= kCountWindowNs));
	};
	bool anyLive = false;
	for (const auto &[serial, c] : chains)
		anyLive = anyLive || isLive(c);
	uint64_t best = 0, bestCount = 0;
	for (const auto &[serial, c] : chains)
	{
		if (anyLive && !isLive(c))
			continue;
		const uint64_t n = countRecent(c, ref);
		if (best == 0 || n > bestCount)
		{
			best = serial;
			bestCount = n;
		}
	}
	if (followed == 0)
	{
		follow(best);
		return;
	}
	if (best == followed)
	{
		candidate = 0;
		return;
	}
	const Chain &cur = chains.at(followed);
	const uint64_t curCount = countRecent(cur, ref);
	const bool curGone = !isLive(cur) && anyLive;
	if (!curGone && double(bestCount) <= 1.5 * double(curCount) + 2)
	{
		candidate = 0;
		return;
	}
	if (candidate != best)
	{
		candidate = best;
		candidateSince = nowNs;
	}
	if (curGone || nowNs - candidateSince >= kSwitchDwellNs)
		follow(best);
}

void LiveSession::Impl::pruneChains(uint64_t refNs)
{
	for (auto it = chains.begin(); it != chains.end();)
	{
		const Chain &c = it->second;
		// Any swapchain but the followed one that has been silent for a while: destroyed ones, and also
		// ones the table never knew (never flagged destroyed). A live one that presents again is
		// recreated by chainFor() on its next record.
		if (it->first != followed && refNs > c.newestNs && refNs - c.newestNs > kPruneAgeNs)
		{
			if (candidate == it->first)
				candidate = 0;
			it = chains.erase(it);
		}
		else
			++it;
	}
}

LiveSession::LiveSession(QObject *parent) : QObject(parent), d_(std::make_unique<Impl>(this))
{
	timer_.setInterval(kTickMs);
	connect(&timer_, &QTimer::timeout, this, &LiveSession::tick);
}

LiveSession::~LiveSession() = default;

int LiveSession::pid() const { return d_->pid; }

void LiveSession::setPid(int pid)
{
	if (pid == d_->pid)
		return;
	detach();
	d_->pid = pid;
	Q_EMIT pidChanged();
	attach();
	if (d_->running && pid != 0)
		timer_.start();
	else
		timer_.stop();
}

bool LiveSession::running() const { return d_->running; }

void LiveSession::setRunning(bool running)
{
	if (running == d_->running)
		return;
	d_->running = running;
	if (running && d_->pid != 0)
		timer_.start();
	else
		timer_.stop();
	Q_EMIT runningChanged();
}

double LiveSession::historySeconds() const { return d_->history; }

void LiveSession::setHistorySeconds(double seconds)
{
	if (!(seconds > 0) || !std::isfinite(seconds) || seconds == d_->history)
		return;
	d_->history = seconds;
	Q_EMIT historySecondsChanged();
}

bool LiveSession::attached() const { return d_->segment != nullptr; }
QString LiveSession::processName() const { return d_->processName; }
QString LiveSession::attachError() const { return d_->attachError; }
QString LiveSession::swapchain() const { return d_->swapchain; }
int LiveSession::swapchainCount() const { return d_->swapchainCount; }
double LiveSession::refreshHz() const { return d_->refreshHz; }

bool LiveSession::valid() const { return d_->numbers.valid; }
bool LiveSession::idle() const { return d_->numbers.idle; }
bool LiveSession::hasDisplayInfo() const { return d_->numbers.hasDisplayInfo; }
double LiveSession::presentFps() const { return d_->numbers.presentFps; }
double LiveSession::displayFps() const { return d_->numbers.displayFps; }
double LiveSession::avgMs() const { return d_->numbers.avgMs; }
double LiveSession::p99Ms() const { return d_->numbers.p99Ms; }
double LiveSession::low1Fps() const { return d_->numbers.low1Fps; }
double LiveSession::low01Fps() const { return d_->numbers.low01Fps; }
double LiveSession::gqi() const { return d_->numbers.gqi; }
double LiveSession::cpuBusyMs() const { return d_->numbers.cpuBusyMs; }
double LiveSession::gpuBusyMs() const { return d_->numbers.gpuBusyMs; }
double LiveSession::gpuWaitMs() const { return d_->numbers.gpuWaitMs; }
double LiveSession::gpuLatencyMs() const { return d_->numbers.gpuLatencyMs; }
double LiveSession::cpuWaitMs() const { return d_->numbers.cpuWaitMs; }
double LiveSession::displayLatencyMs() const { return d_->numbers.displayLatencyMs; }
double LiveSession::untilDisplayedMs() const { return d_->numbers.untilDisplayedMs; }
double LiveSession::readyToScreenMs() const { return d_->numbers.readyToScreenMs; }
double LiveSession::droppedPct() const { return d_->numbers.droppedPct; }
qulonglong LiveSession::lostRecords() const { return d_->numbers.lost; }
FrameSeries *LiveSession::frameTimes() const { return &d_->frameTimes; }
FrameSeries *LiveSession::displayTimes() const { return &d_->displayTimes; }

uint64_t LiveSession::attachNs() const { return d_->attachNs; }

void LiveSession::clearHistory()
{
	d_->frameTimes.clear();
	d_->displayTimes.clear();
	d_->frameTimes.notify();
	d_->displayTimes.notify();
	d_->frameDirty = d_->displayDirty = false;
}

void LiveSession::tickForTest(uint64_t nowNs) { step(nowNs); }
void LiveSession::setOutputSourceForTest(std::function<std::vector<pml::gui::OutputVrr>()> source)
{
	d_->outputSource = std::move(source);
}
double LiveSession::vrrHz() const { return d_->numbers.vrrHz; }
double LiveSession::vrrSwingHz() const { return d_->numbers.vrrSwingHz; }
double LiveSession::vrrLowHz() const { return d_->numbers.vrrLowHz; }
double LiveSession::vrrHighHz() const { return d_->numbers.vrrHighHz; }
double LiveSession::vrrMinHz() const { return d_->numbers.vrrMinHz; }
double LiveSession::vrrMaxHz() const { return d_->numbers.vrrMaxHz; }
int LiveSession::vrrSeverity() const { return d_->numbers.vrrSeverity; }
QString LiveSession::vrrState() const { return d_->numbers.vrrState; }
QString LiveSession::vrrVerdict() const { return d_->numbers.vrrVerdict; }
QString LiveSession::vrrCause() const { return d_->numbers.vrrCause; }
QString LiveSession::vrrDetail() const { return d_->numbers.vrrDetail; }
QString LiveSession::vrrSource() const { return d_->numbers.vrrSource; }

void LiveSession::tick() { step(pml::monotonicNs()); }

// Drops the segment and everything derived from it; the properties go back to "nothing".
void LiveSession::detach()
{
	// attachError and processName are NOTIFY attachedChanged too: a stale error text must go away.
	const bool changed = d_->segment != nullptr || !d_->attachError.isEmpty() || !d_->processName.isEmpty();
	d_->resetData();
	d_->frameTimes.notify();
	d_->displayTimes.notify();
	d_->frameDirty = d_->displayDirty = false;
	d_->attachError.clear();
	if (changed)
		Q_EMIT attachedChanged();
	Q_EMIT swapchainChanged();
	Q_EMIT statsChanged();
}

void LiveSession::attach()
{
	Impl &d = *d_;
	if (d.pid == 0 || d.segment)
		return;
	const uint64_t now = pml::monotonicNs();
	d.nextRetryNs = now + kRetryNs;
	std::string error;
	std::unique_ptr<pml::Segment> seg = pml::Segment::open(pml::segmentNameForPid(d.pid), &error);
	if (!seg)
	{
		const QString text = error.empty() ? QStringLiteral("cannot open the segment")
		                                   : QString::fromStdString(error);
		const QString message = QStringLiteral("No presentmon layer data for process %1: %2").arg(d.pid).arg(text);
		if (message != d.attachError)
		{
			d.attachError = message;
			Q_EMIT attachedChanged();
		}
		return;
	}
	d.segment = std::move(seg);
	d.attachError.clear();
	d.attachNs = now;
	d.dead = false;
	d.processName = QString::fromStdString(pml::headerString(d.segment->header().processName));
	// Oldest: the ring still holds the last few thousand presents (seconds of history). Reading them
	// fills the graph and the 10 s statistics windows at once, instead of an empty chart that takes
	// ten seconds to become meaningful. Newest would only show what happens after the attach.
	d.reader = std::make_unique<pml::RingReader>(*d.segment, pml::RingReader::Start::Oldest);
	d.lostBase = d.reader->lostRecords();   // Oldest on a wrapped ring starts with written - capacity "lost"
	if (d.batch.size() != kReadBatch)
		d.batch.resize(kReadBatch);
	d.nextCheckNs = 0;
	Q_EMIT attachedChanged();
}

void LiveSession::step(uint64_t nowNs)
{
	Impl &d = *d_;
	if (d.pid == 0)
		return;
	if (!d.segment)
	{
		if (nowNs >= d.nextRetryNs || d.nextRetryNs == 0)
			attach();
		if (!d.segment)
			return;
	}
	if (d.dead)
		return;

	bool swapchainDirty = false;
	// Table scan and liveness, twice a second.
	bool checkStale = false;
	if (nowNs >= d.nextCheckNs)
	{
		d.nextCheckNs = nowNs + kRetryNs;
		if (d.segment->replacedOnDisk())
		{
			// The process re-created its segment (execve keeps the pid): start over on the new one.
			detach();
			attach();
			if (!d.segment)
				return;
		}
		const QString oldSwapchain = d.swapchain;
		const int oldCount = d.swapchainCount;
		const double oldHz = d.refreshHz;
		d.scanTable();
		swapchainDirty = oldSwapchain != d.swapchain || oldCount != d.swapchainCount || !same(oldHz, d.refreshHz);
		checkStale = true;

		// The outputs' VRR state, for the VRR readout (twice a second: a handful of DRM ioctls). Each swapchain
		// gets the state of the output it is on (its refresh cycle, just read from the table, says which).
		d.outputs = d.outputSource ? d.outputSource() : pml::gui::readDrmOutputs();
		for (auto &[serial, c] : d.chains)
			c.stats->setVrrContext(
			    pml::gui::vrrContextFromOutputs(d.outputs, c.info.width, c.info.height, c.info.refreshDurationNs));
	}

	const uint64_t before = d.followed;
	// Read everything new.
	std::vector<pml::FrameRecord> &batch = d.batch;
	for (;;)
	{
		const size_t n = d.reader->read(batch.data(), batch.size());
		for (size_t i = 0; i < n; ++i)
		{
			const pml::FrameRecord &rec = batch[i];
			if (rec.swapchainSerial == 0)
				continue;
			Chain &c = d.chainFor(rec.swapchainSerial);
			c.stats->push(rec);
			c.times.push_back(rec.presentStartNs);
			c.newestNs = std::max(c.newestNs, rec.presentStartNs);
			c.pending = true;
			while (c.times.size() > kMaxTimes || (!c.times.empty() && c.newestNs - c.times.front() > 2 * kCountWindowNs))
				c.times.pop_front();
		}
		if (n < batch.size())
			break;
		// A backlog (the attach reads a whole ring): OverlayStats drops the oldest half of its queue
		// at 4096 records, so drain it between batches. update() recomputes the numbers only every
		// 250 ms of its clock; the final recompute below makes them include the whole backlog.
		// The observers sit on the followed swapchain only: pick it before its rows are released.
		d.chooseFollowed(nowNs);
		for (auto &[serial, c] : d.chains)
			if (c.pending)
			{
				c.stats->update(nowNs);
				c.pending = false;
				c.needsRecompute = true;
			}
	}

	d.chooseFollowed(nowNs);
	if (checkStale)
	{
		uint64_t ref = 0;
		for (const auto &[serial, c] : d.chains)
			ref = std::max(ref, c.newestNs);
		d.pruneChains(ref);
	}
	if (d.followed != before)
	{
		const Chain &c = d.chains.at(d.followed);
		d.swapchain = pml::gui::swapchainSummary(c.info);
		d.refreshHz = c.info.refreshDurationNs ? 1e9 / double(c.info.refreshDurationNs) : kNaN;
		swapchainDirty = true;
	}
	else if (d.followed != 0 && checkStale)
	{
		// The table scan may have brought a refresh duration for the followed swapchain.
		const Chain &c = d.chains.at(d.followed);
		const QString text = pml::gui::swapchainSummary(c.info);
		const double hz = c.info.refreshDurationNs ? 1e9 / double(c.info.refreshDurationNs) : kNaN;
		if (text != d.swapchain || !same(hz, d.refreshHz))
		{
			d.swapchain = text;
			d.refreshHz = hz;
			swapchainDirty = true;
		}
	}

	// The producer may be gone; its last records were read above and are processed below.
	if (d.segment->header().state == uint32_t(pml::SegmentState::Exited) ||
	    (checkStale && pml::segmentIsStale(*d.segment, nowNs)))
		d.dead = true;

	for (auto &[serial, c] : d.chains)
	{
		c.stats->update(nowNs);
		if (c.needsRecompute)
			c.stats->recompute(nowNs);
		c.pending = c.needsRecompute = false;
	}

	Numbers n;
	if (d.followed != 0)
	{
		const Chain &c = d.chains.at(d.followed);
		n = fromSnapshot(c.stats->snapshot(), c.stats->vrrContext());
		d.vrrSource = d.vrrSourceText(c.info);
	}
	n.vrrSource = d.vrrSource;
	n.lost = d.reader->lostRecords() - d.lostBase;
	if (d.dead)
		n.idle = true;
	const bool statsChangedNow = !(n == d.numbers);
	d.numbers = n;

	// History: keep historySeconds before the newest point; one notify per series and tick.
	const size_t frameCount = size_t(d.frameTimes.count()), displayCount = size_t(d.displayTimes.count());
	double newestX = -std::numeric_limits<double>::infinity();
	if (d.frameTimes.count() > 0)
		newestX = std::max(newestX, d.frameTimes.maxX());
	if (d.displayTimes.count() > 0)
		newestX = std::max(newestX, d.displayTimes.maxX());
	if (std::isfinite(newestX))
	{
		d.frameTimes.trimBefore(newestX - d.history);
		d.displayTimes.trimBefore(newestX - d.history);
	}
	if (d.frameDirty || size_t(d.frameTimes.count()) != frameCount)
		d.frameTimes.notify();
	if (d.displayDirty || size_t(d.displayTimes.count()) != displayCount)
		d.displayTimes.notify();
	d.frameDirty = d.displayDirty = false;

	if (swapchainDirty)
		Q_EMIT swapchainChanged();
	if (statsChangedNow)
		Q_EMIT statsChanged();
}
