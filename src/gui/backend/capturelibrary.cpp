// SPDX-License-Identifier: MIT
#include "capturelibrary.hpp"

#include "format.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

#include <algorithm>

CaptureLibrary::CaptureLibrary(QObject *parent) : QAbstractListModel(parent)
{
	// A recording writes several files/entries in a burst (create, rename, delete): rescan once.
	debounce_.setSingleShot(true);
	debounce_.setInterval(300);
	connect(&debounce_, &QTimer::timeout, this, &CaptureLibrary::refresh);
	connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this]() { debounce_.start(); });
	// A directory signal fires on create/remove only; a growing capture (size, mtime) needs the files.
	connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this]() { debounce_.start(); });
}

int CaptureLibrary::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(items_.size()); }

QVariant CaptureLibrary::data(const QModelIndex &index, int role) const
{
	if (!index.isValid() || index.row() < 0 || size_t(index.row()) >= items_.size())
		return {};
	const Item &it = items_[size_t(index.row())];
	switch (role)
	{
	case Qt::DisplayRole:
	case FileNameRole: return it.fileName;
	case PathRole: return it.path;
	case ProcessNameRole: return it.processName;
	case DateTimeRole: return it.when;
	case SizeRole: return it.size;
	default: return {};
	}
}

QHash<int, QByteArray> CaptureLibrary::roleNames() const
{
	return {{PathRole, "path"}, {FileNameRole, "fileName"}, {ProcessNameRole, "processName"}, {DateTimeRole, "dateTime"}, {SizeRole, "size"}};
}

QString CaptureLibrary::folder() const { return folder_; }

void CaptureLibrary::setFolder(const QString &folder)
{
	if (folder == folder_)
		return;
	folder_ = folder;
	Q_EMIT folderChanged();
	refresh();
}

int CaptureLibrary::count() const { return int(items_.size()); }

QVariantMap CaptureLibrary::get(int row) const
{
	if (row < 0 || size_t(row) >= items_.size())
		return {};
	const Item &it = items_[size_t(row)];
	return {{"path", it.path}, {"fileName", it.fileName}, {"processName", it.processName}, {"dateTime", it.when}, {"size", it.size}};
}

int CaptureLibrary::indexOfPath(const QString &path) const
{
	const QString abs = QFileInfo(path).absoluteFilePath();
	for (size_t i = 0; i < items_.size(); ++i)
		if (items_[i].path == abs)
			return int(i);
	return -1;
}

void CaptureLibrary::refresh()
{
	// Watch the folder, or while it does not exist yet (the Recorder creates it on the first
	// capture) its closest existing parent, so the first file shows up without a restart.
	QStringList watched = watcher_.directories();
	QString target;
	if (!folder_.isEmpty())
	{
		// QDir::cdUp() fails on a directory that does not exist (it would stay on the missing folder
		// and watch nothing), so walk up the path text to the closest directory that exists. When the
		// Recorder creates the folder, that parent's directoryChanged re-targets the watch here.
		QString t = QDir::cleanPath(QDir(folder_).absolutePath());
		while (t.size() > 1 && !QFileInfo(t).isDir())
		{
			const qsizetype slash = t.lastIndexOf(QLatin1Char('/'));
			t = slash <= 0 ? QStringLiteral("/") : t.left(slash);
		}
		target = t;
	}
	if (watched != QStringList{target} && !(target.isEmpty() && watched.isEmpty()))
	{
		if (!watched.isEmpty())
			watcher_.removePaths(watched);
		if (!target.isEmpty())
			watcher_.addPath(target);
	}

	std::vector<Item> fresh;
	if (!folder_.isEmpty())
	{
		const QDir dir(folder_);
		const auto infos = dir.entryInfoList({QStringLiteral("*.csv")}, QDir::Files | QDir::NoDotAndDotDot);
		fresh.reserve(size_t(infos.size()));
		for (const QFileInfo &fi : infos)
		{
			Item it;
			it.path = fi.absoluteFilePath();
			it.fileName = fi.fileName();
			QDateTime when;
			if (!pml::gui::parseCaptureFileName(it.fileName, &it.processName, &when) || !when.isValid())
				when = fi.lastModified();
			it.when = when;
			it.size = fi.size();
			fresh.push_back(std::move(it));
		}
		std::sort(fresh.begin(), fresh.end(), [](const Item &a, const Item &b) {
			return a.when != b.when ? a.when > b.when : a.fileName > b.fileName;
		});
	}

	// Watch the files themselves so a CSV that grows (or finishes growing) updates its row.
	{
		QStringList want;
		for (const Item &it : fresh)
			want << it.path;
		const QStringList have = watcher_.files();
		QStringList stale;
		for (const QString &p : have)
			if (!want.contains(p))
				stale << p;
		if (!stale.isEmpty())
			watcher_.removePaths(stale);
		QStringList add;
		for (const QString &p : want)
			if (!have.contains(p))
				add << p;
		if (!add.isEmpty())
			watcher_.addPaths(add);
	}

	const int oldCount = int(items_.size());

	// Bring items_ to `fresh` with row operations so views keep their scroll position and selection.
	auto indexIn = [](const std::vector<Item> &v, const QString &path, size_t from) -> int {
		for (size_t i = from; i < v.size(); ++i)
			if (v[i].path == path)
				return int(i);
		return -1;
	};
	// 1. rows that vanished
	for (int i = int(items_.size()) - 1; i >= 0; --i)
		if (indexIn(fresh, items_[size_t(i)].path, 0) < 0)
		{
			beginRemoveRows({}, i, i);
			items_.erase(items_.begin() + i);
			endRemoveRows();
		}
	// 2. insert / move / update, front to back
	for (size_t i = 0; i < fresh.size(); ++i)
	{
		if (i < items_.size() && items_[i].path == fresh[i].path)
		{
			const Item &o = items_[i];
			if (o.size != fresh[i].size || o.when != fresh[i].when || o.processName != fresh[i].processName)
			{
				items_[i] = fresh[i];
				Q_EMIT dataChanged(index(int(i)), index(int(i)));
			}
			continue;
		}
		const int at = indexIn(items_, fresh[i].path, i + 1);
		if (at >= 0)
		{
			beginMoveRows({}, at, at, {}, int(i));
			Item moved = std::move(items_[size_t(at)]);
			items_.erase(items_.begin() + at);
			items_.insert(items_.begin() + qsizetype(i), std::move(moved));
			endMoveRows();
			items_[i] = fresh[i];
			Q_EMIT dataChanged(index(int(i)), index(int(i)));
		}
		else
		{
			beginInsertRows({}, int(i), int(i));
			items_.insert(items_.begin() + qsizetype(i), fresh[i]);
			endInsertRows();
		}
	}
	if (int(items_.size()) != oldCount)
		Q_EMIT countChanged();
}

bool CaptureLibrary::moveToTrash(const QString &path)
{
	if (!QFileInfo(path).isFile() || !path.endsWith(QLatin1String(".csv"), Qt::CaseInsensitive))
		return false;   // never trash anything but a capture file
	const bool ok = QFile::moveToTrash(path);
	if (ok)
		refresh();
	return ok;
}

void CaptureLibrary::openFolder() const
{
	if (!folder_.isEmpty())
		QDesktopServices::openUrl(QUrl::fromLocalFile(folder_));
}

void CaptureLibrary::openFile(const QString &path) const
{
	QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}
