// SPDX-License-Identifier: MIT
#include "recorder.hpp"

#include "format.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QStandardPaths>

namespace {

// <applicationDirPath>/presentmon if executable, else the first one in PATH ("" = none).
QString findPresentmon()
{
	const QFileInfo next(QCoreApplication::applicationDirPath() + QStringLiteral("/presentmon"));
	if (next.isFile() && next.isExecutable())
		return next.absoluteFilePath();
	return QStandardPaths::findExecutable(QStringLiteral("presentmon"));
}

// <base>.csv, or <base>-2.csv, <base>-3.csv ... when that exists.
QString uniqueFile(const QString &dir, const QString &fileName)
{
	QString path = dir + QLatin1Char('/') + fileName;
	if (!QFileInfo::exists(path))
		return path;
	const QString base = fileName.left(fileName.size() - 4);   // without ".csv"
	for (int i = 2;; ++i)
	{
		path = QStringLiteral("%1/%2-%3.csv").arg(dir, base).arg(i);
		if (!QFileInfo::exists(path))
			return path;
	}
}

} // namespace

Recorder::Recorder(QObject *parent) : QObject(parent)
{
	folder_ = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/PresentMon");
	presentmonPath_ = findPresentmon();
	progressTimer_.setInterval(250);
	connect(&progressTimer_, &QTimer::timeout, this, &Recorder::updateProgress);
}

Recorder::~Recorder()
{
	if (process_ && process_->state() != QProcess::NotRunning)
	{
		process_->terminate();
		if (!process_->waitForFinished(3000))
		{
			process_->kill();
			process_->waitForFinished(1000);
		}
	}
}

bool Recorder::recording() const { return recording_; }
int Recorder::pid() const { return pid_; }
QString Recorder::processName() const { return processName_; }
QString Recorder::outputFile() const { return outputFile_; }
double Recorder::elapsedSeconds() const { return elapsed_; }
qint64 Recorder::framesWritten() const { return frames_; }
QString Recorder::lastError() const { return lastError_; }
QString Recorder::lastFile() const { return lastFile_; }
QString Recorder::folder() const { return folder_; }

void Recorder::setFolder(const QString &folder)
{
	if (folder == folder_)
		return;
	folder_ = folder;
	Q_EMIT optionsChanged();
}

int Recorder::durationSeconds() const { return durationSeconds_; }

void Recorder::setDurationSeconds(int seconds)
{
	seconds = qMax(0, seconds);
	if (seconds == durationSeconds_)
		return;
	durationSeconds_ = seconds;
	Q_EMIT optionsChanged();
}

bool Recorder::v1Metrics() const { return v1Metrics_; }

void Recorder::setV1Metrics(bool on)
{
	if (on == v1Metrics_)
		return;
	v1Metrics_ = on;
	Q_EMIT optionsChanged();
}

bool Recorder::stopWhenProcessExits() const { return stopWhenProcessExits_; }

void Recorder::setStopWhenProcessExits(bool on)
{
	if (on == stopWhenProcessExits_)
		return;
	stopWhenProcessExits_ = on;
	Q_EMIT optionsChanged();
}

QString Recorder::presentmonPath() const { return presentmonPath_; }

void Recorder::setPresentmonPath(const QString &path)
{
	presentmonPath_ = path;
	presentmonPathOverridden_ = true;
	Q_EMIT optionsChanged();
}

QStringList Recorder::commandFor(int pid, const QString &outputFile) const
{
	// Arguments only (the program is presentmonPath()). The CLI's default CSV already is the
	// PresentMon 2.x layout with MsBetweenPresents, so only the legacy 1.x layout needs a flag.
	QStringList a{QStringLiteral("--process_id"), QString::number(pid), QStringLiteral("--output_file"), outputFile,
	              QStringLiteral("--no_console_stats")};
	if (v1Metrics_)
		a << QStringLiteral("--v1_metrics");
	if (durationSeconds_ > 0)
		a << QStringLiteral("--timed") << QString::number(durationSeconds_) << QStringLiteral("--terminate_after_timed");
	if (stopWhenProcessExits_)
		a << QStringLiteral("--terminate_on_proc_exit");
	return a;
}

bool Recorder::start(int pid, const QString &processName)
{
	if (recording_)
	{
		setError(tr("Already recording"));
		return false;
	}
	if (!presentmonPathOverridden_)
		presentmonPath_ = findPresentmon();   // may have been installed since start-up
	if (presentmonPath_.isEmpty() || !QFileInfo(presentmonPath_).isExecutable())
	{
		setError(tr("The presentmon command line tool was not found next to the app or in PATH"));
		return false;
	}
	if (pid <= 0)
	{
		setError(tr("No process selected"));
		return false;
	}
	if (!QDir().mkpath(folder_))
	{
		setError(tr("Cannot create the capture folder %1").arg(folder_));
		return false;
	}

	outputFile_ = uniqueFile(QDir(folder_).absolutePath(), pml::gui::captureFileName(processName, QDateTime::currentDateTime()));
	pid_ = pid;
	processName_ = processName;
	frames_ = 0;
	scannedBytes_ = 0;
	newlines_ = 0;
	elapsed_ = 0;
	stderr_.clear();
	stopRequested_ = false;
	if (!lastError_.isEmpty())
	{
		lastError_.clear();
		Q_EMIT lastErrorChanged();
	}

	process_ = new QProcess(this);
	process_->setProgram(presentmonPath_);
	process_->setArguments(commandFor(pid, outputFile_));
	process_->setStandardOutputFile(QProcess::nullDevice());
	QProcess *proc = process_;
	connect(proc, &QProcess::readyReadStandardError, this, [this, proc]() {
		stderr_ += proc->readAllStandardError();
		if (stderr_.size() > 64 * 1024)
			stderr_ = stderr_.right(32 * 1024);
	});
	connect(proc, &QProcess::finished, this, &Recorder::onFinished);
	connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
		if (e != QProcess::FailedToStart || proc != process_)
			return;   // other errors are followed by finished()
		recording_ = false;
		progressTimer_.stop();
		process_ = nullptr;
		proc->deleteLater();
		setError(tr("Could not start %1: %2").arg(presentmonPath_, proc->errorString()));
		Q_EMIT stateChanged();
	});
	proc->start();
	recording_ = true;
	clock_.start();
	progressTimer_.start();
	Q_EMIT stateChanged();
	Q_EMIT progress();
	return true;
}

void Recorder::stop()
{
	if (!recording_ || !process_ || stopRequested_)
		return;
	stopRequested_ = true;
	if (process_->state() == QProcess::NotRunning)
		return;
	process_->terminate();   // SIGTERM: the CLI flushes and closes the CSV
	// A hung CLI must not keep the UI in "recording" forever.
	QPointer<QProcess> proc = process_;
	QTimer::singleShot(5000, this, [proc]() {
		if (proc && proc->state() != QProcess::NotRunning)
			proc->kill();
	});
}

void Recorder::toggle(int pid, const QString &processName)
{
	if (recording_)
		stop();
	else
		start(pid, processName);
}

void Recorder::setError(const QString &error)
{
	lastError_ = error;
	Q_EMIT lastErrorChanged();
}

void Recorder::onFinished(int exitCode, QProcess::ExitStatus status)
{
	QProcess *proc = process_;
	progressTimer_.stop();
	if (proc)
		stderr_ += proc->readAllStandardError();
	updateProgress();   // the final row count
	recording_ = false;
	process_ = nullptr;
	if (proc)
		proc->deleteLater();

	const QString file = outputFile_;
	QString error;
	// A signal death (SIGTERM before the CLI installed its handler, or our SIGKILL fallback) reports
	// the signal number as exit code: that is a clean stop when we asked for it.
	const bool failed = status == QProcess::NormalExit ? exitCode != 0 : !stopRequested_;
	if (failed)
	{
		QStringList lines;
		for (const QByteArray &l : stderr_.split('\n'))
			if (l.startsWith("[presentmon]"))
				lines << QString::fromLocal8Bit(l).trimmed();
		error = lines.isEmpty() ? tr("presentmon exited with code %1").arg(exitCode) : lines.join(QLatin1Char('\n'));
	}
	const bool hasRows = frames_ > 0;
	if (!hasRows)
		QFile::remove(file);   // header only (or nothing): not a capture
	if (!failed && !hasRows)
		error = tr("No frames were recorded");

	Q_EMIT stateChanged();
	if (!error.isEmpty())
	{
		setError(error);
		return;
	}
	if (!lastError_.isEmpty())   // e.g. a refused second start() during this capture
	{
		lastError_.clear();
		Q_EMIT lastErrorChanged();
	}
	lastFile_ = file;
	Q_EMIT finished(file);
}

void Recorder::updateProgress()
{
	if (recording_ || process_)
		elapsed_ = double(clock_.elapsed()) / 1000.0;
	if (!outputFile_.isEmpty())
	{
		QFile f(outputFile_);
		if (f.open(QIODevice::ReadOnly) && f.seek(scannedBytes_))
		{
			// Only complete lines count: the CLI's stdio buffer can end in the middle of a row.
			char buf[64 * 1024];
			qint64 n;
			while ((n = f.read(buf, sizeof(buf))) > 0)
			{
				for (qint64 i = 0; i < n; ++i)
					newlines_ += buf[i] == '\n';
				scannedBytes_ += n;
			}
			frames_ = qMax<qint64>(0, newlines_ - 1);
		}
	}
	Q_EMIT progress();
}
