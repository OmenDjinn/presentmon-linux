// SPDX-License-Identifier: MIT
#include "hotkeys.hpp"

#include <KGlobalAccel>

#include <QAction>
#include <QGuiApplication>
#include <QKeySequence>

namespace {

// Tests (and anything else that sets PML_GUI_TEST) must never write the user's kglobalshortcutsrc.
bool inTest() { return qEnvironmentVariableIsSet("PML_GUI_TEST"); }

QString firstKey(const QAction *a)
{
	if (!a)
		return {};
	const auto keys = KGlobalAccel::self()->shortcut(a);
	return keys.isEmpty() ? QString() : keys.first().toString(QKeySequence::PortableText);
}

} // namespace

Hotkeys::Hotkeys(QObject *parent) : QObject(parent) {}

Hotkeys::~Hotkeys()
{
	// Deleting the actions only unregisters this session; the daemon keeps the user's chosen keys.
	if (enabled_)
		unregisterActions();
}

bool Hotkeys::enabled() const { return enabled_; }

void Hotkeys::setEnabled(bool on)
{
	if (inTest() || on == enabled_)
		return;
	enabled_ = on;
	if (on)
		registerActions();
	else
		unregisterActions();
	Q_EMIT changed();
}

bool Hotkeys::available() const { return available_; }
QString Hotkeys::recordShortcut() const { return available_ ? firstKey(recordAction_) : QString(); }
QString Hotkeys::overlayShortcut() const { return available_ ? firstKey(overlayAction_) : QString(); }

void Hotkeys::registerActions()
{
	if (recordAction_)
		return;
	// KGlobalAccel files the actions under this component (the entry in System Settings >
	// Shortcuts). The desktop file name is what the settings module uses to show the app's icon.
	QString component = QGuiApplication::desktopFileName();
	if (component.isEmpty())
		component = QCoreApplication::applicationName();
	const QString display = QGuiApplication::applicationDisplayName().isEmpty() ? component : QGuiApplication::applicationDisplayName();

	auto make = [&](const QString &id, const QString &text, const QKeySequence &def, auto signal) {
		auto *a = new QAction(text, this);
		a->setObjectName(id);   // KGlobalAccel's action id
		a->setProperty("componentName", component);
		a->setProperty("componentDisplayName", display);
		KGlobalAccel::self()->setDefaultShortcut(a, {def}, KGlobalAccel::Autoloading);
		KGlobalAccel::self()->setShortcut(a, {def}, KGlobalAccel::Autoloading);   // a stored user choice wins
		connect(a, &QAction::triggered, this, signal);
		return a;
	};
	recordAction_ = make(QStringLiteral("toggle-recording"), tr("Start or stop recording"), QKeySequence(Qt::META | Qt::Key_F11),
	                     &Hotkeys::recordToggleRequested);
	overlayAction_ = make(QStringLiteral("toggle-overlay"), tr("Show or hide the overlay"), QKeySequence(Qt::META | Qt::Key_F10),
	                      &Hotkeys::overlayToggleRequested);
	// The daemon answers asynchronously and the user can rebind in System Settings at any time.
	connect(KGlobalAccel::self(), &KGlobalAccel::globalShortcutChanged, this, [this](QAction *a, const QKeySequence &) {
		if (a == recordAction_ || a == overlayAction_)
			Q_EMIT changed();
	});
	available_ = true;
}

void Hotkeys::unregisterActions()
{
	// Not removeAllShortcuts(): that would erase the user's keys from kglobalshortcutsrc.
	disconnect(KGlobalAccel::self(), nullptr, this, nullptr);
	delete recordAction_;
	delete overlayAction_;
	recordAction_ = overlayAction_ = nullptr;
	available_ = false;
}
