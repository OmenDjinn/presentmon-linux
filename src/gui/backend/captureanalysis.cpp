// SPDX-License-Identifier: MIT
#include "captureanalysis.hpp"

#include "csvreader.hpp"
#include "format.hpp"

#include <pml/metrics/vrr.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QPointF>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Everything the worker computes; applied to the object on the GUI thread in one go.
struct LoadResult
{
	bool ok = false;
	std::string error;
	pml::gui::CaptureData data;
	pml::gui::CaptureStats stats;
	pml::metrics::VrrReadout vrr;
	QList<double> histogram;
	double binMs = 1;
	QList<QPointF> framePoints;
	QList<QPointF> displayPoints;
};

// Round bin widths (0.25, 0.5, 1, 2, ...): the smallest one that keeps the histogram at 80 bins or
// fewer over `rangeMs`. The steps are at most a factor of two apart, so it ends up with 40-80 bins.
double chooseBinMs(double rangeMs)
{
	if (!std::isfinite(rangeMs) || !(rangeMs > 0))
		return 1;
	static const double kWidths[] = {0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000};
	for (double w : kWidths)
		if (rangeMs / w <= 80.0)
			return w;
	// Absurd ranges (a corrupt file, or one in the wrong unit): keep going in decades, so it stays at
	// 80 bins or fewer instead of millions.
	double w = 10000;
	while (rangeMs / w > 80.0)
		w *= 10;
	return w;
}

LoadResult loadOrThrow(const std::string &path, const std::atomic<bool> &cancel)
{
	LoadResult r;
	r.ok = pml::gui::readCaptureCsv(path, &r.data, &r.error, &cancel);
	if (!r.ok)
		return r;
	r.stats = pml::gui::computeCaptureStats(r.data);
	if (r.stats.times.count == 0)
	{
		r.ok = false;
		r.error = "The file has no usable frame times";
		return r;
	}
	if (r.data.hasDisplay)
	{
		// A CSV does not say whether VRR was on: the readout judges the pacing as if it were, over every frame.
		std::vector<pml::metrics::VrrFrame> frames(r.data.frameMs.size());
		for (size_t i = 0; i < frames.size(); i++)
			frames[i] = {r.data.frameMs[i], i < r.data.displayMs.size() ? r.data.displayMs[i] : 0.0f};
		r.vrr = pml::metrics::analyzeVrr(frames);
	}

	// Cover the usual frames: up to 1.5 x p99 (at least 2 x the median), but no further than 4 x the
	// median, so a capture with hitches does not squeeze its normal frames into a few bars at the left
	// (the last bin holds everything above; the page labels it "N+ ms").
	const double p50 = r.stats.times.p50Ms;
	double range = std::max(r.stats.times.p99Ms * 1.5, p50 * 2.0);
	if (p50 > 0)
		range = std::min(range, p50 * 4.0);
	if (cancel.load(std::memory_order_relaxed))
	{
		r.ok = false;
		r.error = "cancelled";
		return r;
	}
	r.binMs = chooseBinMs(range);
	const double bins = std::max(1.0, std::ceil(range / r.binMs - 1e-9));
	const auto h = pml::gui::frameTimeHistogram(r.data.frameMs, r.binMs, bins * r.binMs);
	r.histogram = QList<double>(h.begin(), h.end());

	const size_t n = r.data.frameMs.size();
	r.framePoints.reserve(qsizetype(n));
	for (size_t i = 0; i < n; ++i)
		if (std::isfinite(r.data.frameMs[i]))
			r.framePoints.append(QPointF(r.data.timeSec[i], r.data.frameMs[i]));
	if (r.data.hasDisplay)
		for (size_t i = 0; i < n; ++i)
			if (std::isfinite(r.data.displayMs[i]))
				r.displayPoints.append(QPointF(r.data.timeSec[i], r.data.displayMs[i]));
	return r;
}

// Runs in a worker thread. Never throws: an exception (a file too large for memory, ...) would be
// rethrown by QFutureWatcher::result() inside a slot and terminate the app. `cancel` is set when the
// object was re-pathed or destroyed; the result is then dropped anyway, so stop early.
LoadResult load(const std::string &path, const std::shared_ptr<std::atomic<bool>> &cancel)
{
	try
	{
		return loadOrThrow(path, *cancel);
	}
	catch (const std::exception &e)
	{
		LoadResult r;
		r.error = std::string("Cannot load the capture: ") + e.what();
		return r;
	}
	catch (...)
	{
		LoadResult r;
		r.error = "Cannot load the capture";
		return r;
	}
}

} // namespace

struct CaptureAnalysis::Impl
{
	QString path;
	FrameSeries frameTimes;
	FrameSeries displayTimes;
	quint64 generation = 0;   // a result is applied only if it belongs to the newest setPath
	std::shared_ptr<std::atomic<bool>> cancel;   // the running load's; set when it is superseded
	bool loading = false;
	bool loaded = false;
	QString error;
	pml::gui::CaptureData data;
	pml::gui::CaptureStats stats;
	pml::metrics::VrrReadout vrr;
	QList<double> histogram;
	double binMs = 1;
};

CaptureAnalysis::CaptureAnalysis(QObject *parent) : QObject(parent), d_(std::make_unique<Impl>())
{
	d_->frameTimes.setName(QStringLiteral("Frame time"));
	d_->displayTimes.setName(QStringLiteral("Display time"));
}

CaptureAnalysis::~CaptureAnalysis()
{
	// The watcher (a child) dies with us, so the result is never applied; let the worker stop early.
	if (d_->cancel)
		d_->cancel->store(true);
}

QString CaptureAnalysis::path() const { return d_->path; }

void CaptureAnalysis::setPath(const QString &path)
{
	if (path == d_->path)
		return;
	d_->path = path;
	const quint64 gen = ++d_->generation;
	if (d_->cancel)
		d_->cancel->store(true);
	d_->cancel = std::make_shared<std::atomic<bool>>(false);

	// Drop the previous capture's numbers right away: they must not be shown next to the new path.
	const bool hadResult = d_->loaded || !d_->error.isEmpty();
	d_->loaded = false;
	d_->error.clear();
	d_->data = {};
	d_->stats = {};
	d_->vrr = {};
	d_->histogram.clear();
	d_->binMs = 1;
	d_->frameTimes.clear();
	d_->frameTimes.notify();
	d_->displayTimes.clear();
	d_->displayTimes.notify();
	Q_EMIT pathChanged();

	const bool wasLoading = d_->loading;
	d_->loading = !path.isEmpty();
	if (wasLoading != d_->loading)
		Q_EMIT loadingChanged();
	if (hadResult)
		Q_EMIT loadedChanged();
	if (path.isEmpty())
		return;

	auto *watcher = new QFutureWatcher<LoadResult>(this);
	connect(watcher, &QFutureWatcher<LoadResult>::finished, this, [this, watcher, gen]() {
		watcher->deleteLater();
		if (gen != d_->generation)
			return;   // a newer setPath superseded this load
		LoadResult r;
		try
		{
			r = watcher->result();
		}
		catch (...)   // load() catches everything; this is the last guard against terminate()
		{
			r = LoadResult{};
			r.error = "Cannot load the capture";
		}
		d_->loading = false;
		if (r.ok)
		{
			d_->data = std::move(r.data);
			d_->stats = r.stats;
			d_->vrr = r.vrr;
			d_->histogram = std::move(r.histogram);
			d_->binMs = r.binMs;
			d_->frameTimes.setPoints(std::move(r.framePoints));
			d_->displayTimes.setPoints(std::move(r.displayPoints));
			d_->frameTimes.notify();
			d_->displayTimes.notify();
			d_->loaded = true;
		}
		else
			d_->error = QString::fromStdString(r.error);
		Q_EMIT loadingChanged();
		Q_EMIT loadedChanged();
	});
	watcher->setFuture(QtConcurrent::run(load, path.toStdString(), d_->cancel));
}

bool CaptureAnalysis::loading() const { return d_->loading; }
bool CaptureAnalysis::loaded() const { return d_->loaded; }
QString CaptureAnalysis::error() const { return d_->error; }
QString CaptureAnalysis::application() const { return QString::fromStdString(d_->data.application); }
QString CaptureAnalysis::runtime() const { return QString::fromStdString(d_->data.runtime); }
QString CaptureAnalysis::presentMode() const { return QString::fromStdString(d_->data.presentMode); }
QString CaptureAnalysis::frameColumn() const { return QString::fromStdString(d_->data.frameColumn); }

QStringList CaptureAnalysis::warnings() const
{
	QStringList l;
	for (const auto &w : d_->data.warnings)
		l << QString::fromStdString(w);
	return l;
}

int CaptureAnalysis::frames() const { return d_->loaded ? int(d_->stats.frames) : 0; }
double CaptureAnalysis::durationSeconds() const { return d_->loaded ? d_->stats.durationSec : kNaN; }
double CaptureAnalysis::avgFps() const { return d_->loaded ? d_->stats.avgFps : kNaN; }
double CaptureAnalysis::avgMs() const { return d_->loaded ? d_->stats.times.avgMs : kNaN; }
double CaptureAnalysis::minMs() const { return d_->loaded ? d_->stats.times.minMs : kNaN; }
double CaptureAnalysis::maxMs() const { return d_->loaded ? d_->stats.times.maxMs : kNaN; }
double CaptureAnalysis::p50Ms() const { return d_->loaded ? d_->stats.times.p50Ms : kNaN; }
double CaptureAnalysis::p95Ms() const { return d_->loaded ? d_->stats.times.p95Ms : kNaN; }
double CaptureAnalysis::p99Ms() const { return d_->loaded ? d_->stats.times.p99Ms : kNaN; }
double CaptureAnalysis::low1Fps() const { return d_->loaded ? d_->stats.times.low1Fps : kNaN; }
double CaptureAnalysis::low01Fps() const { return d_->loaded ? d_->stats.times.low01Fps : kNaN; }
double CaptureAnalysis::gqi() const { return d_->loaded ? d_->stats.times.gqi : kNaN; }
bool CaptureAnalysis::hasDisplay() const { return d_->loaded && d_->data.hasDisplay; }
double CaptureAnalysis::displayFps() const { return hasDisplay() ? d_->stats.displayFps : kNaN; }
double CaptureAnalysis::droppedPct() const { return hasDisplay() ? d_->stats.droppedPct : kNaN; }
double CaptureAnalysis::vrrHz() const
{
	return d_->vrr.state == pml::metrics::VrrState::Off ? d_->vrr.hz : d_->vrr.medianHz;
}
double CaptureAnalysis::vrrSwingHz() const { return d_->vrr.swingHz; }
double CaptureAnalysis::vrrLowHz() const { return d_->vrr.lowHz; }
double CaptureAnalysis::vrrHighHz() const { return d_->vrr.highHz; }
double CaptureAnalysis::vrrMaxHz() const { return d_->vrr.refreshHz; }
double CaptureAnalysis::vrrUnevenPct() const
{
	return d_->vrr.state == pml::metrics::VrrState::NoData ? kNaN : d_->vrr.unevenPct;
}
int CaptureAnalysis::vrrSeverity() const { return pml::metrics::vrrStabilitySeverity(d_->vrr.stability); }
QString CaptureAnalysis::vrrState() const { return QString::fromLatin1(pml::metrics::vrrStateLabel(d_->vrr.state)); }
QString CaptureAnalysis::vrrVerdict() const { return QString::fromLatin1(pml::metrics::vrrStabilityLabel(d_->vrr.stability)); }
QString CaptureAnalysis::vrrCause() const { return QString::fromLatin1(pml::metrics::vrrCauseLabel(d_->vrr.cause)); }
QString CaptureAnalysis::vrrDetail() const { return pml::gui::vrrDetailText(d_->vrr); }
FrameSeries *CaptureAnalysis::frameTimes() const { return &d_->frameTimes; }
FrameSeries *CaptureAnalysis::displayTimes() const { return &d_->displayTimes; }
QList<double> CaptureAnalysis::histogram() const { return d_->histogram; }
double CaptureAnalysis::histogramBinMs() const { return d_->binMs; }

bool CaptureAnalysis::waitForLoaded(int timeoutMs)
{
	QElapsedTimer t;
	t.start();
	while (d_->loading && t.elapsed() < timeoutMs)
	{
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		QThread::msleep(2);
	}
	return d_->loaded;
}
