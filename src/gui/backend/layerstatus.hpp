// SPDX-License-Identifier: MIT
// LayerStatus (QML singleton): is the Vulkan layer installed where the loader finds it (implicit
// layer manifests in $XDG_DATA_HOME, $XDG_DATA_DIRS and /etc), does its library exist, and the
// launch options that enable it (Steam / Lutris / a shell). For the Settings page.
//
// API contract with the QML: the properties and invokables below.
#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

class LayerStatus : public QObject
{
	Q_OBJECT
	QML_ELEMENT
	QML_SINGLETON
	Q_PROPERTY(bool installed READ installed NOTIFY changed)        // a VkLayer_presentmon manifest was found
	Q_PROPERTY(QString manifestPath READ manifestPath NOTIFY changed)
	Q_PROPERTY(QString libraryPath READ libraryPath NOTIFY changed) // from the manifest's library_path
	Q_PROPERTY(bool libraryFound READ libraryFound NOTIFY changed)
	Q_PROPERTY(QString cliPath READ cliPath NOTIFY changed)         // presentmon CLI next to the GUI or in PATH ("" = none)
	Q_PROPERTY(QString steamLaunchOptions READ steamLaunchOptions CONSTANT)  // Proton on Wayland
	Q_PROPERTY(QString nativeLaunchOptions READ nativeLaunchOptions CONSTANT)// native Vulkan games
	Q_PROPERTY(QString shellPrefix READ shellPrefix CONSTANT)                // "PRESENTMON_LAYER=1"

public:
	explicit LayerStatus(QObject *parent = nullptr);

	bool installed() const;
	QString manifestPath() const;
	QString libraryPath() const;
	bool libraryFound() const;
	QString cliPath() const;
	QString steamLaunchOptions() const;
	QString nativeLaunchOptions() const;
	QString shellPrefix() const;

	Q_INVOKABLE void refresh();
	Q_INVOKABLE void copyToClipboard(const QString &text) const;

Q_SIGNALS:
	void changed();

private:
	bool installed_ = false;
	QString manifestPath_;
	QString libraryPath_;
	bool libraryFound_ = false;
	QString cliPath_;
};
