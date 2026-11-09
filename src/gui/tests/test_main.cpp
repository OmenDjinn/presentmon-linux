// SPDX-License-Identifier: MIT
// pml_gui_tests: GTest inside a QGuiApplication (offscreen platform: no window ever opens).
// HOME points at a temporary directory for the whole run, so nothing a test does (Qt's test-mode
// settings and trash under ~/.qttest, caches, QStandardPaths locations) lands in the user's home.
#include <QGuiApplication>
#include <QTemporaryDir>

#include <gtest/gtest.h>

int main(int argc, char **argv)
{
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
		qputenv("QT_QPA_PLATFORM", "offscreen");
	qputenv("PML_GUI_TEST", "1");

	QTemporaryDir home(QDir::tempPath() + QStringLiteral("/pml_gui_tests-XXXXXX"));
	if (!home.isValid())
		return 2;
	qputenv("HOME", home.path().toUtf8());
	for (const char *var : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"})
		qunsetenv(var);

	QGuiApplication app(argc, argv);
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
