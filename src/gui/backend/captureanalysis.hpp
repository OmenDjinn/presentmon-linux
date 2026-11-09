// SPDX-License-Identifier: MIT
// CaptureAnalysis: one CSV capture, loaded in a worker thread (csvreader.hpp) and exposed to QML:
// summary numbers (same definitions as the in-game overlay), frame-time and display-time series for
// the chart, and a frame-time histogram. Create one per capture to compare several.
//
// Values that are not available are NaN. API contract with the QML: the properties below.
#pragma once

#include "frameseries.hpp"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <memory>

class CaptureAnalysis : public QObject
{
	Q_OBJECT
	QML_ELEMENT
	Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)   // setting it starts loading
	Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
	Q_PROPERTY(bool loaded READ loaded NOTIFY loadedChanged)              // numbers below are valid
	Q_PROPERTY(QString error READ error NOTIFY loadedChanged)

	Q_PROPERTY(QString application READ application NOTIFY loadedChanged)
	Q_PROPERTY(QString runtime READ runtime NOTIFY loadedChanged)
	Q_PROPERTY(QString presentMode READ presentMode NOTIFY loadedChanged)
	Q_PROPERTY(QString frameColumn READ frameColumn NOTIFY loadedChanged)
	Q_PROPERTY(QStringList warnings READ warnings NOTIFY loadedChanged)

	Q_PROPERTY(int frames READ frames NOTIFY loadedChanged)
	Q_PROPERTY(double durationSeconds READ durationSeconds NOTIFY loadedChanged)
	Q_PROPERTY(double avgFps READ avgFps NOTIFY loadedChanged)
	Q_PROPERTY(double avgMs READ avgMs NOTIFY loadedChanged)
	Q_PROPERTY(double minMs READ minMs NOTIFY loadedChanged)
	Q_PROPERTY(double maxMs READ maxMs NOTIFY loadedChanged)
	Q_PROPERTY(double p50Ms READ p50Ms NOTIFY loadedChanged)
	Q_PROPERTY(double p95Ms READ p95Ms NOTIFY loadedChanged)
	Q_PROPERTY(double p99Ms READ p99Ms NOTIFY loadedChanged)
	Q_PROPERTY(double low1Fps READ low1Fps NOTIFY loadedChanged)
	Q_PROPERTY(double low01Fps READ low01Fps NOTIFY loadedChanged)
	Q_PROPERTY(double gqi READ gqi NOTIFY loadedChanged)
	Q_PROPERTY(bool hasDisplay READ hasDisplay NOTIFY loadedChanged)
	Q_PROPERTY(double displayFps READ displayFps NOTIFY loadedChanged)
	Q_PROPERTY(double droppedPct READ droppedPct NOTIFY loadedChanged)
	// VRR verdict (pml/metrics/vrr.hpp) of the whole capture, without the driver's VRR state.
	Q_PROPERTY(int vrrScore READ vrrScore NOTIFY loadedChanged)
	Q_PROPERTY(QString vrrState READ vrrState NOTIFY loadedChanged)
	Q_PROPERTY(QString vrrVerdict READ vrrVerdict NOTIFY loadedChanged)
	Q_PROPERTY(QString vrrCause READ vrrCause NOTIFY loadedChanged)
	Q_PROPERTY(QString vrrDetail READ vrrDetail NOTIFY loadedChanged)
	// Experience score (pml/metrics/experience.hpp) over the same frames.
	Q_PROPERTY(int experienceScore READ experienceScore NOTIFY loadedChanged)           // 0-100, -1 = none
	Q_PROPERTY(int pacingScore READ pacingScore NOTIFY loadedChanged)                   // judder + stutters only, -1 = none
	Q_PROPERTY(QString experienceVerdict READ experienceVerdict NOTIFY loadedChanged)   // "smooth" ... "very rough"
	Q_PROPERTY(QString experienceCause READ experienceCause NOTIFY loadedChanged)       // main cause, "" from 85 up
	Q_PROPERTY(QString experienceDetail READ experienceDetail NOTIFY loadedChanged)     // the numbers behind the score

	// x = seconds since the first frame, y = ms.
	Q_PROPERTY(FrameSeries *frameTimes READ frameTimes CONSTANT)
	Q_PROPERTY(FrameSeries *displayTimes READ displayTimes CONSTANT)
	// Share of frames (percent) per histogramBinMs-wide bin, from 0 ms; the last bin holds everything above.
	Q_PROPERTY(QList<double> histogram READ histogram NOTIFY loadedChanged)
	Q_PROPERTY(double histogramBinMs READ histogramBinMs NOTIFY loadedChanged)

public:
	explicit CaptureAnalysis(QObject *parent = nullptr);
	~CaptureAnalysis() override;

	QString path() const;
	void setPath(const QString &path);
	bool loading() const;
	bool loaded() const;
	QString error() const;
	QString application() const;
	QString runtime() const;
	QString presentMode() const;
	QString frameColumn() const;
	QStringList warnings() const;
	int frames() const;
	double durationSeconds() const;
	double avgFps() const;
	double avgMs() const;
	double minMs() const;
	double maxMs() const;
	double p50Ms() const;
	double p95Ms() const;
	double p99Ms() const;
	double low1Fps() const;
	double low01Fps() const;
	double gqi() const;
	bool hasDisplay() const;
	double displayFps() const;
	double droppedPct() const;
	int vrrScore() const;
	QString vrrState() const;
	QString vrrVerdict() const;
	QString vrrCause() const;
	QString vrrDetail() const;
	int experienceScore() const;
	int pacingScore() const;
	QString experienceVerdict() const;
	QString experienceCause() const;
	QString experienceDetail() const;
	FrameSeries *frameTimes() const;
	FrameSeries *displayTimes() const;
	QList<double> histogram() const;
	double histogramBinMs() const;

	// Tests: block until the current load has finished (true if it succeeded).
	bool waitForLoaded(int timeoutMs = 10000);

Q_SIGNALS:
	void pathChanged();
	void loadingChanged();
	void loadedChanged();

private:
	struct Impl;
	std::unique_ptr<Impl> d_;
};
