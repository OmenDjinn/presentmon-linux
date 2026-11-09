// SPDX-License-Identifier: MIT
// presentmon-gui: desktop front end for presentmon-linux (Qt Quick + Kirigami).
//
// Options (also the test hooks):
//   --page live|captures|settings   page to open
//   --pid <pid>                     process to select on the Live page
//   --capture <file.csv>            open a capture on the Captures page (repeat to compare)
//   --screenshot <file.png>         save the window after --screenshot-delay ms (default 3000), then quit
//   --size WxH                      initial window size (tests of narrow and wide layouts)
// Environment:
//   PML_GUI_APP_ID=<id>   Wayland app id / desktop file name (default presentmon-gui). Test runs use
//                         presentmon-gui-test, so a KWin window rule can keep test windows out of the way.
//   PML_GUI_TEST=1        test run: never registers global shortcuts, settings go to Qt's test
//                         locations (~/.qttest) instead of ~/.config.
#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QtQml/qqmlextensionplugin.h>

#include <cstdio>

Q_IMPORT_QML_PLUGIN(PresentMonPlugin)

int main(int argc, char *argv[])
{
	// Breeze controls under Plasma (qqc2-desktop-style), unless the user picked a style.
	if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
		QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));

	QApplication app(argc, argv);
	QApplication::setApplicationName(QStringLiteral("presentmon-gui"));
	QApplication::setOrganizationName(QStringLiteral("presentmon-linux"));
	QApplication::setApplicationDisplayName(QStringLiteral("PresentMon"));
	QApplication::setApplicationVersion(QStringLiteral(PML_VERSION));
	QGuiApplication::setDesktopFileName(qEnvironmentVariable("PML_GUI_APP_ID", QStringLiteral("presentmon-gui")));
	QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("presentmon-gui"),
	                                             QIcon(QStringLiteral(":/presentmon-gui/presentmon-gui.svg"))));

	QCommandLineParser parser;
	parser.setApplicationDescription(QStringLiteral("Live frame times, overlay control and CSV captures for presentmon-linux"));
	parser.addHelpOption();
	parser.addVersionOption();
	const QCommandLineOption pageOpt(QStringLiteral("page"), QStringLiteral("Page to open: live, captures or settings."), QStringLiteral("page"), QStringLiteral("live"));
	const QCommandLineOption pidOpt(QStringLiteral("pid"), QStringLiteral("Process to select on the Live page."), QStringLiteral("pid"));
	const QCommandLineOption captureOpt(QStringLiteral("capture"), QStringLiteral("CSV capture to open (repeat to compare)."), QStringLiteral("file"));
	const QCommandLineOption shotOpt(QStringLiteral("screenshot"), QStringLiteral("Save the window to a PNG and quit."), QStringLiteral("file"));
	const QCommandLineOption delayOpt(QStringLiteral("screenshot-delay"), QStringLiteral("Milliseconds before the screenshot (default 3000)."), QStringLiteral("ms"), QStringLiteral("3000"));
	const QCommandLineOption sizeOpt(QStringLiteral("size"), QStringLiteral("Initial window size, e.g. 900x600 (tests)."), QStringLiteral("WxH"));
	parser.addOptions({pageOpt, pidOpt, captureOpt, shotOpt, delayOpt, sizeOpt});
	parser.process(app);

	const bool testMode = qEnvironmentVariableIntValue("PML_GUI_TEST") != 0;
	// Test runs keep their settings (QML Settings / QSettings) out of the user's ~/.config.
	if (testMode)
		QStandardPaths::setTestModeEnabled(true);

	QQmlApplicationEngine engine;
	QVariantMap initial = {
		{QStringLiteral("startPage"), parser.value(pageOpt)},
		{QStringLiteral("startPid"), parser.value(pidOpt).toInt()},
		{QStringLiteral("startCaptures"), parser.values(captureOpt)},
		{QStringLiteral("testMode"), testMode},
	};
	const QStringList size = parser.value(sizeOpt).split(QLatin1Char('x'));
	if (size.size() == 2 && size[0].toInt() > 0 && size[1].toInt() > 0)
	{
		initial.insert(QStringLiteral("width"), size[0].toInt());
		initial.insert(QStringLiteral("height"), size[1].toInt());
	}
	engine.setInitialProperties(initial);
	QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
	                 [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
	engine.loadFromModule("PresentMon", "Main");
	if (engine.rootObjects().isEmpty())
		return 1;

	const QString shot = parser.value(shotOpt);
	if (!shot.isEmpty())
	{
		auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
		QTimer::singleShot(parser.value(delayOpt).toInt(), &app, [window, shot] {
			const QImage image = window ? window->grabWindow() : QImage();
			const bool ok = !image.isNull() && image.save(shot);
			std::fprintf(stderr, "[presentmon-gui] screenshot %s: %s (%dx%d)\n", ok ? "saved" : "FAILED",
			             qPrintable(shot), image.width(), image.height());
			QCoreApplication::exit(ok ? 0 : 2);
		});
	}
	return app.exec();
}
