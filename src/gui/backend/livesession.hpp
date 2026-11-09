// SPDX-License-Identifier: MIT
// LiveSession: live numbers and frame-time history of one process. Maps the process's segment
// (presentmon-linux.<pid>), reads its ring with its own cursor (several readers are fine, e.g. a
// presentmon capture at the same time), and runs one pml::layer::overlay::OverlayStats per
// swapchain, so every number equals the in-game overlay's. It follows the busiest live swapchain
// (most presents in the last second). OverlayStats' row observers fill the two FrameSeries.
//
// Values that are not available are NaN (QML shows a dash). API contract with the QML: the
// properties and invokables below.
#pragma once

#include "frameseries.hpp"

#include <QObject>
#include <QString>
#include "drmvrr.hpp"

#include <QTimer>

#include <functional>
#include <vector>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <memory>

class LiveSession : public QObject
{
	Q_OBJECT
	QML_ELEMENT
	Q_PROPERTY(int pid READ pid WRITE setPid NOTIFY pidChanged)                 // 0 = none
	Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged) // false: stop polling (page hidden)
	Q_PROPERTY(double historySeconds READ historySeconds WRITE setHistorySeconds NOTIFY historySecondsChanged) // default 10

	Q_PROPERTY(bool attached READ attached NOTIFY attachedChanged)        // segment mapped
	Q_PROPERTY(QString processName READ processName NOTIFY attachedChanged)
	Q_PROPERTY(QString attachError READ attachError NOTIFY attachedChanged)

	Q_PROPERTY(QString swapchain READ swapchain NOTIFY swapchainChanged)  // summary of the followed swapchain
	Q_PROPERTY(int swapchainCount READ swapchainCount NOTIFY swapchainChanged)  // live swapchains seen
	Q_PROPERTY(double refreshHz READ refreshHz NOTIFY swapchainChanged)   // from refreshDurationNs, NaN if unknown

	Q_PROPERTY(bool valid READ valid NOTIFY statsChanged)          // enough samples for the numbers
	Q_PROPERTY(bool idle READ idle NOTIFY statsChanged)            // the app stopped presenting
	Q_PROPERTY(bool hasDisplayInfo READ hasDisplayInfo NOTIFY statsChanged)
	Q_PROPERTY(double presentFps READ presentFps NOTIFY statsChanged)
	Q_PROPERTY(double displayFps READ displayFps NOTIFY statsChanged)
	Q_PROPERTY(double avgMs READ avgMs NOTIFY statsChanged)
	Q_PROPERTY(double p99Ms READ p99Ms NOTIFY statsChanged)
	Q_PROPERTY(double low1Fps READ low1Fps NOTIFY statsChanged)
	Q_PROPERTY(double low01Fps READ low01Fps NOTIFY statsChanged)
	Q_PROPERTY(double cpuBusyMs READ cpuBusyMs NOTIFY statsChanged)
	Q_PROPERTY(double cpuWaitMs READ cpuWaitMs NOTIFY statsChanged)
	Q_PROPERTY(double gpuBusyMs READ gpuBusyMs NOTIFY statsChanged)
	Q_PROPERTY(double gpuWaitMs READ gpuWaitMs NOTIFY statsChanged)
	Q_PROPERTY(double gpuLatencyMs READ gpuLatencyMs NOTIFY statsChanged)
	Q_PROPERTY(double displayLatencyMs READ displayLatencyMs NOTIFY statsChanged)
	Q_PROPERTY(double untilDisplayedMs READ untilDisplayedMs NOTIFY statsChanged)
	Q_PROPERTY(double readyToScreenMs READ readyToScreenMs NOTIFY statsChanged)
	Q_PROPERTY(double droppedPct READ droppedPct NOTIFY statsChanged)
	Q_PROPERTY(qulonglong lostRecords READ lostRecords NOTIFY statsChanged)  // ring records overwritten before read
	// VRR verdict (pml/metrics/vrr.hpp) over the last 10 s of displayed frames, with the driver's VRR state.
	Q_PROPERTY(int vrrScore READ vrrScore NOTIFY statsChanged)            // 0-100, -1 = none
	Q_PROPERTY(QString vrrState READ vrrState NOTIFY statsChanged)        // "no data", "unknown", "off", "idle", "active"
	Q_PROPERTY(QString vrrVerdict READ vrrVerdict NOTIFY statsChanged)    // "stable" ... "very unstable", "" without score
	Q_PROPERTY(QString vrrCause READ vrrCause NOTIFY statsChanged)        // main cause, "" when stable
	Q_PROPERTY(QString vrrDetail READ vrrDetail NOTIFY statsChanged)      // the numbers behind the score
	Q_PROPERTY(QString vrrSource READ vrrSource NOTIFY statsChanged)      // what the driver says, and where

	// x = seconds since the session attached, y = ms. Trimmed to historySeconds.
	Q_PROPERTY(FrameSeries *frameTimes READ frameTimes CONSTANT)    // MsBetweenPresents, one point per present
	Q_PROPERTY(FrameSeries *displayTimes READ displayTimes CONSTANT)// MsBetweenDisplayChange, one per displayed frame

public:
	explicit LiveSession(QObject *parent = nullptr);
	~LiveSession() override;

	int pid() const;
	void setPid(int pid);
	bool running() const;
	void setRunning(bool running);
	double historySeconds() const;
	void setHistorySeconds(double seconds);

	bool attached() const;
	QString processName() const;
	QString attachError() const;
	QString swapchain() const;
	int swapchainCount() const;
	double refreshHz() const;

	bool valid() const;
	bool idle() const;
	bool hasDisplayInfo() const;
	double presentFps() const;
	double displayFps() const;
	double avgMs() const;
	double p99Ms() const;
	double low1Fps() const;
	double low01Fps() const;
	double cpuBusyMs() const;
	double cpuWaitMs() const;
	double gpuBusyMs() const;
	double gpuWaitMs() const;
	double gpuLatencyMs() const;
	double displayLatencyMs() const;
	double untilDisplayedMs() const;
	double readyToScreenMs() const;
	double droppedPct() const;
	qulonglong lostRecords() const;
	int vrrScore() const;
	QString vrrState() const;
	QString vrrVerdict() const;
	QString vrrCause() const;
	QString vrrDetail() const;
	QString vrrSource() const;

	FrameSeries *frameTimes() const;
	FrameSeries *displayTimes() const;

	Q_INVOKABLE void clearHistory();

	// Tests: the CLOCK_MONOTONIC time the series' x = 0 refers to (the attach time, 0 if detached).
	uint64_t attachNs() const;

	// Tests: one poll step now (reads the ring, updates stats and series) with a given clock.
	void tickForTest(uint64_t nowNs);
	// Tests: where the outputs' VRR state comes from (default: the DRM devices, readDrmOutputs()).
	void setOutputSourceForTest(std::function<std::vector<pml::gui::OutputVrr>()> source);

Q_SIGNALS:
	void pidChanged();
	void runningChanged();
	void historySecondsChanged();
	void attachedChanged();
	void swapchainChanged();
	void statsChanged();

private:
	void tick();
	void step(uint64_t nowNs);
	void attach();
	void detach();

	struct Impl;
	std::unique_ptr<Impl> d_;
	QTimer timer_;
};
