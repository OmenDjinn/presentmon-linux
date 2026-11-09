// SPDX-License-Identifier: MIT
// Recorder (QML singleton): records one process to CSV by running the presentmon CLI as a
// subprocess (the same, tested code path as `presentmon --process_id P --output_file F`), so the
// GUI's files are identical to the CLI's. Stop sends SIGTERM, on which the CLI flushes and closes
// the CSV. Files are named like Intel PresentMon's: <folder>/pmcap-<exe>-YYMMDD-HHMMSS.csv.
//
// API contract with the QML: the properties, invokables and signals below.
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

class Recorder : public QObject
{
	Q_OBJECT
	QML_ELEMENT
	QML_SINGLETON
	Q_PROPERTY(bool recording READ recording NOTIFY stateChanged)
	Q_PROPERTY(int pid READ pid NOTIFY stateChanged)
	Q_PROPERTY(QString processName READ processName NOTIFY stateChanged)
	Q_PROPERTY(QString outputFile READ outputFile NOTIFY stateChanged)
	Q_PROPERTY(double elapsedSeconds READ elapsedSeconds NOTIFY progress)
	Q_PROPERTY(qint64 framesWritten READ framesWritten NOTIFY progress)   // CSV data rows so far
	Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
	Q_PROPERTY(QString lastFile READ lastFile NOTIFY finished)            // the most recent finished capture

	// Options (bound and persisted by the Settings page).
	Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY optionsChanged)   // default <Documents>/PresentMon
	Q_PROPERTY(int durationSeconds READ durationSeconds WRITE setDurationSeconds NOTIFY optionsChanged)  // 0 = until stopped
	Q_PROPERTY(bool v1Metrics READ v1Metrics WRITE setV1Metrics NOTIFY optionsChanged)  // false: PresentMon 2.x columns (default)
	Q_PROPERTY(bool stopWhenProcessExits READ stopWhenProcessExits WRITE setStopWhenProcessExits NOTIFY optionsChanged) // default true
	Q_PROPERTY(QString presentmonPath READ presentmonPath NOTIFY optionsChanged)   // the CLI that is run; "" = not found

public:
	explicit Recorder(QObject *parent = nullptr);
	~Recorder() override;   // stops a running capture (SIGTERM, waits up to 3 s)

	bool recording() const;
	int pid() const;
	QString processName() const;
	QString outputFile() const;
	double elapsedSeconds() const;
	qint64 framesWritten() const;
	QString lastError() const;
	QString lastFile() const;

	QString folder() const;
	void setFolder(const QString &folder);
	int durationSeconds() const;
	void setDurationSeconds(int seconds);
	bool v1Metrics() const;
	void setV1Metrics(bool on);
	bool stopWhenProcessExits() const;
	void setStopWhenProcessExits(bool on);
	QString presentmonPath() const;
	// Tests: run this binary instead of the one found next to the GUI / in PATH.
	void setPresentmonPath(const QString &path);

	// Starts recording pid (false + lastError if already recording, no CLI, or the folder cannot be
	// created). processName is used for the file name.
	Q_INVOKABLE bool start(int pid, const QString &processName);
	// Stops a running capture (no-op otherwise). finished() follows when the CLI has exited.
	Q_INVOKABLE void stop();
	Q_INVOKABLE void toggle(int pid, const QString &processName);

	// The command line start() would run (tests, and shown in the UI as a hint).
	Q_INVOKABLE QStringList commandFor(int pid, const QString &outputFile) const;

Q_SIGNALS:
	void stateChanged();
	void progress();
	void lastErrorChanged();
	void optionsChanged();
	void finished(const QString &file);

private:
	void setError(const QString &error);
	void onFinished(int exitCode, QProcess::ExitStatus status);
	void updateProgress();

	QProcess *process_ = nullptr;
	bool recording_ = false;
	bool stopRequested_ = false;
	double elapsed_ = 0;
	qint64 newlines_ = 0;       // line breaks seen in the CSV so far (rows = newlines - 1 for the header)
	QByteArray stderr_;         // the CLI's stderr, for the error message
	QTimer progressTimer_;
	QElapsedTimer clock_;
	int pid_ = 0;
	QString processName_;
	QString outputFile_;
	qint64 frames_ = 0;
	qint64 scannedBytes_ = 0;
	QString lastError_;
	QString lastFile_;
	QString folder_;
	int durationSeconds_ = 0;
	bool v1Metrics_ = false;
	bool stopWhenProcessExits_ = true;
	QString presentmonPath_;
	bool presentmonPathOverridden_ = false;
};
