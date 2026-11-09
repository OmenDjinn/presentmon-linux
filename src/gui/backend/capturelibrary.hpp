// SPDX-License-Identifier: MIT
// CaptureLibrary (QML singleton): the CSV files in the capture folder, newest first, kept up to
// date with a QFileSystemWatcher. Process name and time come from the pmcap-<exe>-YYMMDD-HHMMSS
// file name when it has that form, else from the file (modification time).
//
// API contract with the QML: the roles, properties and invokables below.
#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <vector>

class CaptureLibrary : public QAbstractListModel
{
	Q_OBJECT
	QML_ELEMENT
	QML_SINGLETON
	Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY folderChanged)
	Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
	enum Role
	{
		PathRole = Qt::UserRole + 1,   // QString: absolute path
		FileNameRole,                  // QString
		ProcessNameRole,               // QString ("" if unknown)
		DateTimeRole,                  // QDateTime
		SizeRole,                      // qint64 bytes
	};
	Q_ENUM(Role)

	explicit CaptureLibrary(QObject *parent = nullptr);

	int rowCount(const QModelIndex &parent = QModelIndex()) const override;
	QVariant data(const QModelIndex &index, int role) const override;
	QHash<int, QByteArray> roleNames() const override;

	QString folder() const;
	void setFolder(const QString &folder);
	int count() const;

	Q_INVOKABLE QVariantMap get(int row) const;
	Q_INVOKABLE int indexOfPath(const QString &path) const;
	Q_INVOKABLE void refresh();
	// Moves a capture to the desktop trash (recoverable). False if that failed.
	Q_INVOKABLE bool moveToTrash(const QString &path);
	Q_INVOKABLE void openFolder() const;                 // file manager at the folder
	Q_INVOKABLE void openFile(const QString &path) const;// default application for .csv

Q_SIGNALS:
	void folderChanged();
	void countChanged();

private:
	struct Item
	{
		QString path;
		QString fileName;
		QString processName;
		QDateTime when;
		qint64 size = 0;
	};
	std::vector<Item> items_;
	QString folder_;
	QFileSystemWatcher watcher_;
	QTimer debounce_;
};
