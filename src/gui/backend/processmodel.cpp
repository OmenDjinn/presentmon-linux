// SPDX-License-Identifier: MIT
#include "processmodel.hpp"

#include "format.hpp"

#include <pml/clock.hpp>
#include <pml/shm.hpp>

#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace {

constexpr uint64_t kIdleAfterNs = 1'500'000'000;   // no heartbeat for this long -> idle (same as the overlay)
// The rate is a delta over the time since the last sample. A refresh() right after a poll (setters,
// QML) would divide a handful of presents by a few milliseconds, so such a sample is skipped and the
// baseline stays where it was.
constexpr uint64_t kMinRateWindowNs = 100'000'000;
constexpr uint32_t kAllPanels = pml::kPanelFps | pml::kPanelFrameTimeGraph | pml::kPanelCpu | pml::kPanelGpu |
                                pml::kPanelLatency | pml::kPanelSwapchain | pml::kPanelDropped | pml::kPanelVrr;

} // namespace

struct ProcessModel::Entry
{
	int pid = 0;
	std::string segmentName;
	std::unique_ptr<pml::Segment> segment;

	// Immutable per segment (re-read when the file is replaced).
	QString name, exePath, application, engine, layerVersion;

	// Changes while the process runs.
	QString runtime, swapchain, presentMode;
	int swapchainCount = 0, width = 0, height = 0;
	bool hdr = false;
	double presentRate = 0;
	bool idle = true;
	bool overlayAvailable = false, overlayVisible = false;
	int overlayPosition = 0;
	double overlayScale = 0;
	int overlayPanels = 0;

	// Rate baseline.
	uint64_t lastSeq = 0;
	uint64_t lastNs = 0;
};

ProcessModel::ProcessModel(QObject *parent) : QAbstractListModel(parent)
{
	timer_.setInterval(500);
	connect(&timer_, &QTimer::timeout, this, &ProcessModel::refresh);
	refresh();
	timer_.start();
}

ProcessModel::~ProcessModel() = default;

int ProcessModel::rowCount(const QModelIndex &parent) const
{
	return parent.isValid() ? 0 : int(entries_.size());
}

QHash<int, QByteArray> ProcessModel::roleNames() const
{
	return {
		{PidRole, "pid"},
		{NameRole, "name"},
		{ExePathRole, "exePath"},
		{ApplicationRole, "application"},
		{EngineRole, "engine"},
		{RuntimeRole, "runtime"},
		{SwapchainRole, "swapchain"},
		{SwapchainCountRole, "swapchainCount"},
		{WidthRole, "width"},
		{HeightRole, "height"},
		{PresentModeRole, "presentMode"},
		{HdrRole, "hdr"},
		{PresentRateRole, "presentRate"},
		{IdleRole, "idle"},
		{OverlayAvailableRole, "overlayAvailable"},
		{OverlayVisibleRole, "overlayVisible"},
		{OverlayPositionRole, "overlayPosition"},
		{OverlayScaleRole, "overlayScale"},
		{OverlayPanelsRole, "overlayPanels"},
		{LayerVersionRole, "layerVersion"},
	};
}

QVariant ProcessModel::data(const QModelIndex &index, int role) const
{
	if (!index.isValid() || index.parent().isValid() || index.row() < 0 || index.row() >= int(entries_.size()))
		return {};
	const Entry &e = *entries_[size_t(index.row())];
	switch (role)
	{
	case PidRole: return e.pid;
	case NameRole: return e.name;
	case ExePathRole: return e.exePath;
	case ApplicationRole: return e.application;
	case EngineRole: return e.engine;
	case RuntimeRole: return e.runtime;
	case SwapchainRole: return e.swapchain;
	case SwapchainCountRole: return e.swapchainCount;
	case WidthRole: return e.width;
	case HeightRole: return e.height;
	case PresentModeRole: return e.presentMode;
	case HdrRole: return e.hdr;
	case PresentRateRole: return e.presentRate;
	case IdleRole: return e.idle;
	case OverlayAvailableRole: return e.overlayAvailable;
	case OverlayVisibleRole: return e.overlayVisible;
	case OverlayPositionRole: return e.overlayPosition;
	case OverlayScaleRole: return e.overlayScale;
	case OverlayPanelsRole: return e.overlayPanels;
	case LayerVersionRole: return e.layerVersion;
	default: return {};
	}
}

int ProcessModel::count() const { return int(entries_.size()); }

int ProcessModel::pollIntervalMs() const { return timer_.interval(); }

void ProcessModel::setPollIntervalMs(int ms)
{
	ms = std::clamp(ms, 50, 60'000);
	if (ms == timer_.interval())
		return;
	timer_.setInterval(ms);
	Q_EMIT pollIntervalMsChanged();
}

int ProcessModel::indexOfPid(int pid) const
{
	for (size_t i = 0; i < entries_.size(); ++i)
		if (entries_[i]->pid == pid)
			return int(i);
	return -1;
}

QVariantMap ProcessModel::get(int row) const
{
	QVariantMap map;
	if (row < 0 || row >= int(entries_.size()))
		return map;
	const QHash<int, QByteArray> names = roleNames();
	for (auto it = names.cbegin(); it != names.cend(); ++it)
		map.insert(QString::fromLatin1(it.value()), data(index(row), it.key()));
	return map;
}

ProcessModel::Entry *ProcessModel::find(int pid)
{
	const int row = indexOfPid(pid);
	return row < 0 ? nullptr : entries_[size_t(row)].get();
}

const ProcessModel::Entry *ProcessModel::find(int pid) const
{
	const int row = indexOfPid(pid);
	return row < 0 ? nullptr : entries_[size_t(row)].get();
}

namespace {

// Strings that do not change while the segment lives.
void readStatic(const pml::Segment &seg, QString *name, QString *exePath, QString *application, QString *engine,
                QString *layerVersion)
{
	const pml::SegmentHeader &h = seg.header();
	*name = QString::fromStdString(pml::headerString(h.processName));
	*exePath = QString::fromStdString(pml::headerString(h.exePath));
	*application = QString::fromStdString(pml::headerString(h.applicationName));
	*engine = QString::fromStdString(pml::headerString(h.engineName));
	*layerVersion = QString::fromStdString(pml::headerString(h.layerVersion));
}

} // namespace

// Refreshes one entry from its segment and emits dataChanged for the roles that changed.
void ProcessModel::updateEntry(Entry &e, uint64_t nowNs)
{
	const pml::Segment &seg = *e.segment;
	const pml::SegmentHeader &h = seg.header();
	QList<int> changed;
	auto set = [&changed](auto &field, const auto &value, int role) {
		if (field != value)
		{
			field = value;
			changed.append(role);
		}
	};

	// Newest live swapchain.
	pml::SwapchainInfo newest = {};
	bool haveNewest = false;
	int live = 0;
	const uint32_t used = std::min(pml::atomicLoad(h.swapchainHighWater), pml::kMaxSwapchains);
	for (uint32_t i = 0; i < used; ++i)
	{
		pml::SwapchainInfo info;
		if (!pml::readSwapchain(seg, i, &info) || info.destroyedNs != 0)
			continue;
		++live;
		if (!haveNewest || info.createdNs > newest.createdNs ||
		    (info.createdNs == newest.createdNs && info.serial > newest.serial))
		{
			newest = info;
			haveNewest = true;
		}
	}
	set(e.swapchainCount, live, SwapchainCountRole);
	set(e.runtime, haveNewest ? pml::gui::runtimeName(newest) : QString(), RuntimeRole);
	set(e.swapchain, haveNewest ? pml::gui::swapchainSummary(newest) : QString(), SwapchainRole);
	set(e.width, haveNewest ? int(newest.width) : 0, WidthRole);
	set(e.height, haveNewest ? int(newest.height) : 0, HeightRole);
	set(e.presentMode, haveNewest ? pml::gui::presentModeName(newest.presentMode) : QString(), PresentModeRole);
	set(e.hdr, haveNewest && pml::gui::isHdrColorSpace(newest.colorSpace), HdrRole);

	// Present rate: delta of writeSeq (one record per present of any swapchain) over the elapsed time.
	const uint64_t seq = pml::atomicLoad(h.writeSeq);
	if (e.lastNs == 0 || seq < e.lastSeq)
	{
		e.lastSeq = seq;
		e.lastNs = nowNs;
	}
	else if (nowNs > e.lastNs && nowNs - e.lastNs >= kMinRateWindowNs)
	{
		const double rate = double(seq - e.lastSeq) / (double(nowNs - e.lastNs) * 1e-9);
		set(e.presentRate, rate, PresentRateRole);
		e.lastSeq = seq;
		e.lastNs = nowNs;
	}
	const uint64_t heartbeat = pml::atomicLoad(h.heartbeatNs, std::memory_order_relaxed);
	set(e.idle, heartbeat == 0 || (nowNs > heartbeat && nowNs - heartbeat > kIdleAfterNs), IdleRole);
	if (e.idle && e.presentRate != 0)
		set(e.presentRate, 0.0, PresentRateRole);   // the last interval's rate would be stale news

	const pml::OverlayControl ctl = pml::readOverlayControl(seg);
	set(e.overlayAvailable, ctl.available != 0, OverlayAvailableRole);
	set(e.overlayVisible, ctl.visible != 0, OverlayVisibleRole);
	set(e.overlayPosition, int(ctl.position), OverlayPositionRole);
	set(e.overlayScale, double(ctl.scalePermille) / 1000.0, OverlayScaleRole);
	set(e.overlayPanels, int(ctl.panels), OverlayPanelsRole);

	if (!changed.isEmpty())
	{
		const int row = indexOfPid(e.pid);
		if (row >= 0)
			Q_EMIT dataChanged(index(row), index(row), changed);
	}
}

void ProcessModel::refresh()
{
	const uint64_t now = pml::monotonicNs();
	const int countBefore = int(entries_.size());
	const std::vector<pml::SegmentListing> listed = pml::listSegments();
	auto isListed = [&listed](const std::string &name) {
		return std::any_of(listed.begin(), listed.end(), [&](const pml::SegmentListing &l) { return l.name == name; });
	};

	// Removals first (back to front keeps the rows below valid), so a re-created pid is not matched
	// against its dead predecessor.
	for (int row = int(entries_.size()) - 1; row >= 0; --row)
	{
		Entry &e = *entries_[size_t(row)];
		if (!isListed(e.segmentName) || pml::segmentIsStale(*e.segment, now))
		{
			beginRemoveRows({}, row, row);
			entries_.erase(entries_.begin() + row);
			endRemoveRows();
			continue;
		}
		if (e.segment->replacedOnDisk())
		{
			// The process re-created its segment (execve keeps the pid): map the new file.
			std::unique_ptr<pml::Segment> fresh = pml::Segment::open(e.segmentName);
			if (!fresh)
				continue;   // still being created: keep the old mapping, try again next time
			e.segment = std::move(fresh);
			e.lastNs = 0;
			e.lastSeq = 0;
			QString name, exePath, application, engine, layerVersion;
			readStatic(*e.segment, &name, &exePath, &application, &engine, &layerVersion);
			e.name = name;
			e.exePath = exePath;
			e.application = application;
			e.engine = engine;
			e.layerVersion = layerVersion;
			Q_EMIT dataChanged(index(row), index(row),
			                   {NameRole, ExePathRole, ApplicationRole, EngineRole, LayerVersionRole});
		}
	}

	for (const pml::SegmentListing &l : listed)
	{
		if (find(l.pid))
			continue;
		std::unique_ptr<pml::Segment> seg = pml::Segment::open(l.name);   // failures are normal: still initializing
		if (!seg || pml::segmentIsStale(*seg, now))
			continue;
		auto e = std::make_unique<Entry>();
		e->pid = l.pid;
		e->segmentName = l.name;
		e->segment = std::move(seg);
		readStatic(*e->segment, &e->name, &e->exePath, &e->application, &e->engine, &e->layerVersion);
		// Fill the dynamic part before the row appears, so views never see a half-empty row (the
		// entry is not listed yet, so this emits nothing).
		updateEntry(*e, now);
		const int row = int(entries_.size());
		beginInsertRows({}, row, row);
		entries_.push_back(std::move(e));
		endInsertRows();
	}

	for (auto &e : entries_)
		updateEntry(*e, now);
	if (int(entries_.size()) != countBefore)
		Q_EMIT countChanged();
}

namespace {

// Read-modify-write of the control block, like `presentmon overlay` (collector/src/overlay_cmd.cpp):
// the layer re-reads the fields when `generation` moves.
template <class F>
bool modifyOverlay(pml::Segment &seg, F &&change)
{
	pml::OverlayControl ctl = pml::readOverlayControl(seg);
	if (!change(ctl))
		return false;
	pml::writeOverlayControl(seg, ctl);
	return true;
}

} // namespace

bool ProcessModel::setOverlayVisible(int pid, bool visible)
{
	Entry *e = find(pid);
	if (!e || !modifyOverlay(*e->segment, [&](pml::OverlayControl &c) { c.visible = visible ? 1 : 0; return true; }))
		return false;
	updateEntry(*e, pml::monotonicNs());
	return true;
}

bool ProcessModel::setOverlayPosition(int pid, int position)
{
	Entry *e = find(pid);
	if (!e || position < int(pml::OverlayPosition::TopLeft) || position > int(pml::OverlayPosition::BottomRight))
		return false;
	modifyOverlay(*e->segment, [&](pml::OverlayControl &c) { c.position = uint32_t(position); return true; });
	updateEntry(*e, pml::monotonicNs());
	return true;
}

bool ProcessModel::setOverlayScale(int pid, double scale)
{
	Entry *e = find(pid);
	if (!e || !std::isfinite(scale) || scale < 0)
		return false;
	// 0 = automatic (from the swapchain height); otherwise the range the overlay can render sensibly.
	const uint32_t permille = scale == 0 ? 0 : uint32_t(std::lround(std::clamp(scale, 0.5, 4.0) * 1000.0));
	modifyOverlay(*e->segment, [&](pml::OverlayControl &c) { c.scalePermille = permille; return true; });
	updateEntry(*e, pml::monotonicNs());
	return true;
}

bool ProcessModel::setOverlayPanels(int pid, int panels)
{
	Entry *e = find(pid);
	if (!e || panels < 0)
		return false;
	modifyOverlay(*e->segment, [&](pml::OverlayControl &c) { c.panels = uint32_t(panels) & kAllPanels; return true; });
	updateEntry(*e, pml::monotonicNs());
	return true;
}

int ProcessModel::busiestPid() const
{
	int best = 0;
	double bestRate = 0;
	for (const auto &e : entries_)
		if (!e->idle && e->presentRate > bestRate)
		{
			best = e->pid;
			bestRate = e->presentRate;
		}
	return best;
}
