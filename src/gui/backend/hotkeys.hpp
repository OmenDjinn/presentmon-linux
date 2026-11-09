// SPDX-License-Identifier: MIT
// Hotkeys (QML singleton): KDE global shortcuts through KGlobalAccel, so recording and the overlay
// can be toggled while a game has focus. Registering writes the component's entries to the user's
// kglobalshortcutsrc (like any KDE app with global shortcuts); it only happens while `enabled` is
// true. The app enables it at start-up unless PML_GUI_TEST is set (tests never register anything).
// The keys can be changed in System Settings > Keyboard > Shortcuts.
//
// API contract with the QML: the properties and signals below.
#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

class QAction;

class Hotkeys : public QObject
{
	Q_OBJECT
	QML_ELEMENT
	QML_SINGLETON
	Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
	Q_PROPERTY(bool available READ available NOTIFY changed)           // registered with KGlobalAccel
	Q_PROPERTY(QString recordShortcut READ recordShortcut NOTIFY changed)   // e.g. "Meta+F11", "" = none
	Q_PROPERTY(QString overlayShortcut READ overlayShortcut NOTIFY changed) // e.g. "Meta+F10", "" = none

public:
	explicit Hotkeys(QObject *parent = nullptr);
	~Hotkeys() override;

	bool enabled() const;
	void setEnabled(bool on);
	bool available() const;
	QString recordShortcut() const;
	QString overlayShortcut() const;

Q_SIGNALS:
	void changed();
	void recordToggleRequested();
	void overlayToggleRequested();

private:
	void registerActions();
	void unregisterActions();

	bool enabled_ = false;
	bool available_ = false;
	QAction *recordAction_ = nullptr;
	QAction *overlayAction_ = nullptr;
};
