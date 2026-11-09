// SPDX-License-Identifier: MIT
// The window: a sidebar with the three pages, settings persistence, and the global shortcuts
// (record / overlay), which act on the process selected on the Live page or, if none, on the one
// presenting the most frames.
import QtCore
import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import PresentMon

Kirigami.ApplicationWindow {
    id: root

    // Set by main.cpp from the command line.
    property string startPage: "live"
    property int startPid: 0
    property var startCaptures: []
    property bool testMode: false

    // The process the Live page shows (0 = none); also the target of the global shortcuts.
    property int selectedPid: startPid
    property string currentPage: ""

    title: currentPage === "captures" ? i18nc("@title:window", "Captures") :
           currentPage === "settings" ? i18nc("@title:window", "Settings") : i18nc("@title:window", "Live")
    width: Kirigami.Units.gridUnit * 72
    height: Kirigami.Units.gridUnit * 46
    minimumWidth: Kirigami.Units.gridUnit * 40
    minimumHeight: Kirigami.Units.gridUnit * 28

    // No KI18n in this app: plain strings, same call shape as KDE apps use.
    function i18nc(context, text) { return text }

    Settings {
        id: appSettings
        category: "General"
        property string captureFolder: ""
        property int recordSeconds: 0
        property bool v1Metrics: false
        property bool stopOnExit: true
        property bool hotkeysEnabled: true
        property int historySeconds: 10
    }

    // Recorder options follow the settings both ways.
    Binding { target: Recorder; property: "durationSeconds"; value: appSettings.recordSeconds }
    Binding { target: Recorder; property: "v1Metrics"; value: appSettings.v1Metrics }
    Binding { target: Recorder; property: "stopWhenProcessExits"; value: appSettings.stopOnExit }
    Binding { target: CaptureLibrary; property: "folder"; value: Recorder.folder }

    Component.onCompleted: {
        if (appSettings.captureFolder !== "")
            Recorder.folder = appSettings.captureFolder
        Hotkeys.enabled = !testMode && appSettings.hotkeysEnabled
        showPage(startPage)
    }

    Connections {
        target: Hotkeys
        function onRecordToggleRequested() {
            if (Recorder.recording) {
                Recorder.stop()
                return
            }
            const pid = root.targetPid()
            if (pid === 0) {
                root.showPassiveNotification(i18nc("@info", "No game with the PresentMon layer is running"))
                return
            }
            const row = ProcessModel.indexOfPid(pid)
            Recorder.start(pid, row >= 0 ? ProcessModel.get(row).name : "process")
        }
        function onOverlayToggleRequested() {
            const pid = root.targetPid()
            const row = pid !== 0 ? ProcessModel.indexOfPid(pid) : -1
            if (row < 0) {
                root.showPassiveNotification(i18nc("@info", "No game with the PresentMon layer is running"))
                return
            }
            const p = ProcessModel.get(row)
            if (!p.overlayAvailable) {
                root.showPassiveNotification(i18nc("@info", "The overlay is not loaded in %1").replace("%1", () => p.name))
                return
            }
            ProcessModel.setOverlayVisible(pid, !p.overlayVisible)
        }
    }

    // Placeholders are filled with a function: a replacement string would expand "$&" and the like in
    // a file name or an error message.
    Connections {
        target: Recorder
        function onFinished(file) {
            const name = file.substring(file.lastIndexOf("/") + 1)
            root.showPassiveNotification(i18nc("@info", "Capture saved: %1").replace("%1", () => name), "long",
                                         i18nc("@action:button", "Open"), () => root.openCapture(file))
        }
        // Also for recordings started by the global shortcut, which nothing else would report (the
        // Live page shows the error too). lastErrorChanged also fires when the error is cleared.
        function onLastErrorChanged() {
            if (Recorder.lastError !== "")
                root.showPassiveNotification(i18nc("@info", "Recording failed: %1").replace("%1", () => Recorder.lastError), "long")
        }
    }

    // Selected process if it is still running, else the busiest one.
    function targetPid() {
        if (selectedPid !== 0 && ProcessModel.indexOfPid(selectedPid) >= 0)
            return selectedPid
        return ProcessModel.busiestPid()
    }

    // Each page is created once, with an Item parent, and kept: it keeps its state between visits (the
    // selected process, the compared captures), and pushing a Component would make Kirigami create it
    // under a QtObject, which Qt warns about on every page switch.
    property var pageObjects: ({})
    function pageObject(name) {
        let p = pageObjects[name]
        if (!p) {
            const comp = name === "captures" ? capturesPage : name === "settings" ? settingsPage : livePage
            p = comp.createObject(pageStack)
            pageObjects[name] = p
        }
        return p
    }

    function showPage(name) {
        name = name === "captures" || name === "settings" ? name : "live"
        if (name === currentPage && pageStack.depth > 0)
            return
        currentPage = name
        pageStack.clear()
        pageStack.push(pageObject(name))
    }

    function openCapture(file) {
        showPage("captures")
        if (pageStack.currentItem && pageStack.currentItem.selectPaths)
            pageStack.currentItem.selectPaths([file])
    }

    globalDrawer: Kirigami.GlobalDrawer {
        id: drawer
        isMenu: false
        modal: !root.wideScreen
        collapsible: true
        collapsed: !root.wideScreen
        showHeaderWhenCollapsed: true
        header: Kirigami.AbstractApplicationHeader {
            contentItem: Row {
                spacing: Kirigami.Units.smallSpacing
                leftPadding: Kirigami.Units.smallSpacing
                Kirigami.Icon {
                    source: "presentmon-gui"
                    fallback: "view-statistics"
                    width: Kirigami.Units.iconSizes.medium
                    height: width
                    anchors.verticalCenter: parent.verticalCenter
                }
                Kirigami.Heading {
                    visible: !drawer.collapsed
                    text: "PresentMon"
                    level: 2
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
        actions: [
            Kirigami.Action {
                text: root.i18nc("@action", "Live")
                icon.name: "view-statistics"
                checkable: true
                checked: root.currentPage === "live"
                onTriggered: root.showPage("live")
            },
            Kirigami.Action {
                text: root.i18nc("@action", "Captures")
                icon.name: "document-open-recent"
                checkable: true
                checked: root.currentPage === "captures"
                onTriggered: root.showPage("captures")
            },
            Kirigami.Action {
                text: root.i18nc("@action", "Settings")
                icon.name: "configure"
                checkable: true
                checked: root.currentPage === "settings"
                onTriggered: root.showPage("settings")
            }
        ]
    }

    Component {
        id: livePage
        LivePage {
            settings: appSettings
            selectedPid: root.selectedPid
            onSelectedPidChanged: root.selectedPid = selectedPid
            onOpenCaptureRequested: file => root.openCapture(file)
            onSettingsRequested: root.showPage("settings")
        }
    }
    Component {
        id: capturesPage
        CapturesPage {
            initialPaths: root.startCaptures
            onLiveRequested: root.showPage("live")
        }
    }
    Component {
        id: settingsPage
        SettingsPage {
            settings: appSettings
        }
    }
}
