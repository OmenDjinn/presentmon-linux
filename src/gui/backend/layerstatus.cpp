// SPDX-License-Identifier: MIT
#include "layerstatus.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace {

constexpr auto kLayerName = "VK_LAYER_presentmon_linux";

QString envOr(const char *name, const QString &fallback)
{
	const QString v = qEnvironmentVariable(name);
	return v.isEmpty() ? fallback : v;
}

// The directories the Vulkan loader searches for implicit layer manifests on Linux, in its order.
QStringList manifestDirs()
{
	const QString home = QDir::homePath();
	QStringList base;
	base << envOr("XDG_CONFIG_HOME", home + QStringLiteral("/.config"));
	base << envOr("XDG_CONFIG_DIRS", QStringLiteral("/etc/xdg")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
	base << QStringLiteral("/etc");
	base << envOr("XDG_DATA_HOME", home + QStringLiteral("/.local/share"));
	base << envOr("XDG_DATA_DIRS", QStringLiteral("/usr/local/share:/usr/share")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
	QStringList dirs;
	for (const QString &b : base)
	{
		const QString d = QDir::cleanPath(b + QStringLiteral("/vulkan/implicit_layer.d"));
		if (!dirs.contains(d))
			dirs << d;
	}
	return dirs;
}

// The layer object of a manifest with our name (file_format 1.0/1.1 "layer", 1.1+ "layers").
QJsonObject findLayer(const QJsonObject &root)
{
	auto matches = [](const QJsonObject &o) { return o.value(QLatin1String("name")).toString() == QLatin1String(kLayerName); };
	const QJsonObject single = root.value(QLatin1String("layer")).toObject();
	if (matches(single))
		return single;
	for (const QJsonValue &v : root.value(QLatin1String("layers")).toArray())
		if (matches(v.toObject()))
			return v.toObject();
	return {};
}

bool libraryExists(const QString &libraryPath, const QString &manifestDir, QString *resolved)
{
	if (libraryPath.contains(QLatin1Char('/')))
	{
		*resolved = QDir::cleanPath(QDir::isAbsolutePath(libraryPath) ? libraryPath : manifestDir + QLatin1Char('/') + libraryPath);
		return QFileInfo(*resolved).isFile();
	}
	// A bare soname: the loader hands it to dlopen(), which searches the system library directories.
	*resolved = libraryPath;
	QStringList dirs = qEnvironmentVariable("LD_LIBRARY_PATH").split(QLatin1Char(':'), Qt::SkipEmptyParts);
	dirs << QStringLiteral("/usr/lib") << QStringLiteral("/usr/lib64") << QStringLiteral("/usr/lib/x86_64-linux-gnu")
	     << QStringLiteral("/lib") << QStringLiteral("/lib64") << QStringLiteral("/usr/local/lib");
	for (const QString &d : dirs)
		if (QFileInfo(d + QLatin1Char('/') + libraryPath).isFile())
			return true;
	return false;
}

} // namespace

LayerStatus::LayerStatus(QObject *parent) : QObject(parent) { refresh(); }

bool LayerStatus::installed() const { return installed_; }
QString LayerStatus::manifestPath() const { return manifestPath_; }
QString LayerStatus::libraryPath() const { return libraryPath_; }
bool LayerStatus::libraryFound() const { return libraryFound_; }
QString LayerStatus::cliPath() const { return cliPath_; }
QString LayerStatus::steamLaunchOptions() const { return QStringLiteral("PRESENTMON_LAYER=1 PROTON_ENABLE_WAYLAND=1 %command%"); }
QString LayerStatus::nativeLaunchOptions() const { return QStringLiteral("PRESENTMON_LAYER=1 %command%"); }
QString LayerStatus::shellPrefix() const { return QStringLiteral("PRESENTMON_LAYER=1"); }

void LayerStatus::refresh()
{
	bool installed = false, found = false;
	QString manifest, library;

	for (const QString &dirPath : manifestDirs())
	{
		const QDir dir(dirPath);
		if (!dir.exists())
			continue;
		for (const QFileInfo &fi : dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name))
		{
			QFile f(fi.absoluteFilePath());
			if (!f.open(QIODevice::ReadOnly))
				continue;
			const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
			if (!doc.isObject())
				continue;
			const QJsonObject layer = findLayer(doc.object());
			if (layer.isEmpty())
				continue;
			installed = true;
			manifest = fi.absoluteFilePath();
			const QString lib = layer.value(QLatin1String("library_path")).toString();
			if (!lib.isEmpty())
				found = libraryExists(lib, fi.absolutePath(), &library);
			break;
		}
		if (installed)
			break;   // the loader uses the first manifest it finds for a layer name
	}

	const QString cli = [] {
		const QFileInfo next(QCoreApplication::applicationDirPath() + QStringLiteral("/presentmon"));
		if (next.isFile() && next.isExecutable())
			return next.absoluteFilePath();
		return QStandardPaths::findExecutable(QStringLiteral("presentmon"));
	}();

	if (installed == installed_ && manifest == manifestPath_ && library == libraryPath_ && found == libraryFound_ && cli == cliPath_)
		return;
	installed_ = installed;
	manifestPath_ = manifest;
	libraryPath_ = library;
	libraryFound_ = found;
	cliPath_ = cli;
	Q_EMIT changed();
}

void LayerStatus::copyToClipboard(const QString &text) const
{
	if (QClipboard *cb = QGuiApplication::clipboard())
		cb->setText(text);
}
