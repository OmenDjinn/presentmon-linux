// SPDX-License-Identifier: MIT
// ProcessModel (QML singleton): the layer-enabled processes, found by polling /dev/shm for
// presentmon-linux segments every 500 ms, with a cheap present rate per process (from the
// segment's writeSeq, no ring reading), the newest swapchain's description and the overlay state.
// It also writes the overlay control block (the same IPC as `presentmon overlay ...`).
//
// API contract with the QML (keep names stable): the roles below, count, indexOfPid, get,
// setOverlay*, busiestPid, refresh.
#pragma once

#include <QAbstractListModel>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace pml {
class Segment;
}

class ProcessModel : public QAbstractListModel
{
	Q_OBJECT
	QML_ELEMENT
	QML_SINGLETON
	Q_PROPERTY(int count READ count NOTIFY countChanged)
	Q_PROPERTY(int pollIntervalMs READ pollIntervalMs WRITE setPollIntervalMs NOTIFY pollIntervalMsChanged)

public:
	enum Role
	{
		PidRole = Qt::UserRole + 1,
		NameRole,             // QString: process name (exe basename; Windows exe name under Wine)
		ExePathRole,          // QString
		ApplicationRole,      // QString: VkApplicationInfo name ("" if none)
		EngineRole,           // QString: VkApplicationInfo engine name ("DXVK", "vkd3d", "" ...)
		RuntimeRole,          // QString: "Vulkan" / "DXVK" / "VKD3D-Proton" (newest live swapchain)
		SwapchainRole,        // QString: swapchainSummary() of the newest live swapchain ("" if none)
		SwapchainCountRole,   // int: live swapchains
		WidthRole,            // int
		HeightRole,           // int
		PresentModeRole,      // QString
		HdrRole,              // bool
		PresentRateRole,      // double: presents per second over the last poll interval (all swapchains)
		IdleRole,             // bool: no present for 1.5 s
		OverlayAvailableRole, // bool: overlay loaded in that process (PRESENTMON_OVERLAY != off)
		OverlayVisibleRole,   // bool
		OverlayPositionRole,  // int: pml::OverlayPosition (0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right)
		OverlayScaleRole,     // double: 0 = automatic
		OverlayPanelsRole,    // int: pml::OverlayPanel bits, 0 = default set
		LayerVersionRole,     // QString
	};
	Q_ENUM(Role)

	explicit ProcessModel(QObject *parent = nullptr);
	~ProcessModel() override;

	int rowCount(const QModelIndex &parent = QModelIndex()) const override;
	QVariant data(const QModelIndex &index, int role) const override;
	QHash<int, QByteArray> roleNames() const override;

	int count() const;
	int pollIntervalMs() const;
	void setPollIntervalMs(int ms);

	// Row of a pid, -1 if not listed.
	Q_INVOKABLE int indexOfPid(int pid) const;
	// All roles of a row as a map keyed by role name ({} for a bad row).
	Q_INVOKABLE QVariantMap get(int row) const;

	// Overlay control of one process. False if the process is not listed (any more) or the write failed.
	Q_INVOKABLE bool setOverlayVisible(int pid, bool visible);
	Q_INVOKABLE bool setOverlayPosition(int pid, int position);
	Q_INVOKABLE bool setOverlayScale(int pid, double scale);   // 0 = automatic, else 0.5 .. 4
	Q_INVOKABLE bool setOverlayPanels(int pid, int panels);    // pml::OverlayPanel bits, 0 = default

	// The process with the highest present rate that is not idle (target of the record hotkey), 0 if none.
	Q_INVOKABLE int busiestPid() const;

	// Poll /dev/shm now (also done by the timer).
	Q_INVOKABLE void refresh();

Q_SIGNALS:
	void countChanged();
	void pollIntervalMsChanged();

private:
	struct Entry;
	Entry *find(int pid);
	const Entry *find(int pid) const;
	void updateEntry(Entry &e, uint64_t nowNs);

	std::vector<std::unique_ptr<Entry>> entries_;
	QTimer timer_;
};
