// SPDX-License-Identifier: MIT
// Record the selected process to CSV (through the presentmon CLI, see Recorder), with the duration
// picker, live progress, the result and errors.
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import PresentMon

ColumnLayout {
    id: controls

    property int pid: 0
    property string processName
    property var settings
    property string hotkey: Hotkeys.available ? Hotkeys.recordShortcut : ""
    signal openCaptureRequested(string file)

    readonly property bool recordingThis: Recorder.recording && Recorder.pid === pid
    readonly property bool recordingOther: Recorder.recording && Recorder.pid !== pid
    readonly property var durations: [0, 10, 30, 60, 120, 300]

    spacing: Kirigami.Units.smallSpacing

    RowLayout {
        spacing: Kirigami.Units.largeSpacing

        QQC2.Button {
            text: controls.recordingThis ? "Stop" : "Record"
            icon.name: controls.recordingThis ? "media-playback-stop" : "media-record"
            enabled: controls.recordingThis || (!Recorder.recording && controls.pid !== 0 && Recorder.presentmonPath !== "")
            onClicked: controls.recordingThis ? Recorder.stop() : Recorder.start(controls.pid, controls.processName)
            QQC2.ToolTip.visible: hovered && controls.hotkey !== ""
            QQC2.ToolTip.text: "Global shortcut: " + controls.hotkey
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }

        QQC2.ComboBox {
            enabled: !Recorder.recording
            model: controls.durations.map(s => s === 0 ? "Until stopped" : (s < 60 ? s + " s" : (s / 60) + " min"))
            currentIndex: Math.max(0, controls.durations.indexOf(Recorder.durationSeconds))
            onActivated: index => {
                if (controls.settings)
                    controls.settings.recordSeconds = controls.durations[index]
                else
                    Recorder.durationSeconds = controls.durations[index]
            }
        }

        RowLayout {
            visible: controls.recordingThis
            spacing: Kirigami.Units.smallSpacing
            Rectangle {
                width: Kirigami.Units.gridUnit * 0.6
                height: width
                radius: width / 2
                color: Kirigami.Theme.negativeTextColor
                SequentialAnimation on opacity {
                    running: controls.recordingThis
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.3; duration: 600 }
                    NumberAnimation { to: 1; duration: 600 }
                }
            }
            QQC2.Label {
                text: Fmt.duration(Recorder.elapsedSeconds) +
                      (Recorder.durationSeconds > 0 ? " / " + Fmt.duration(Recorder.durationSeconds) : "") +
                      " · " + Recorder.framesWritten.toLocaleString(Qt.locale(), "f", 0) + " frames"
                font.features: { "tnum": 1 }
            }
        }
        QQC2.Label {
            visible: controls.recordingOther
            text: "Recording " + Recorder.processName + "…"
            color: Kirigami.Theme.disabledTextColor
        }
        QQC2.Label {
            visible: !Recorder.recording && controls.hotkey !== ""
            text: controls.hotkey
            color: Kirigami.Theme.disabledTextColor
            font: Kirigami.Theme.smallFont
        }
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: Recorder.presentmonPath === ""
        type: Kirigami.MessageType.Warning
        text: "The presentmon command-line tool was not found next to this app or in PATH, so recording is not possible."
    }
    // No showCloseButton on these two: Kirigami's close button assigns visible = false, which replaces
    // the binding and would hide every later error or result. Dismiss sets a flag instead, reset by
    // the next recording.
    Kirigami.InlineMessage {
        id: errorMessage
        property bool dismissed: false
        Layout.fillWidth: true
        visible: !Recorder.recording && Recorder.lastError !== "" && !dismissed
        type: Kirigami.MessageType.Error
        text: Recorder.lastError
        actions: [
            Kirigami.Action {
                text: "Dismiss"
                icon.name: "dialog-close"
                onTriggered: errorMessage.dismissed = true
            }
        ]
    }
    Kirigami.InlineMessage {
        id: saved
        property bool dismissed: false
        Layout.fillWidth: true
        visible: !Recorder.recording && Recorder.lastError === "" && Recorder.lastFile !== "" && !dismissed
        type: Kirigami.MessageType.Positive
        text: "Saved " + Recorder.lastFile.substring(Recorder.lastFile.lastIndexOf("/") + 1)
        actions: [
            Kirigami.Action {
                text: "Open"
                icon.name: "document-open"
                onTriggered: controls.openCaptureRequested(Recorder.lastFile)
            },
            Kirigami.Action {
                text: "Dismiss"
                icon.name: "dialog-close"
                onTriggered: saved.dismissed = true
            }
        ]
    }
    Connections {
        target: Recorder
        function onStateChanged() {
            if (Recorder.recording) {
                errorMessage.dismissed = false
                saved.dismissed = false
            }
        }
        function onFinished() {
            errorMessage.dismissed = false
            saved.dismissed = false
        }
        function onLastErrorChanged() { errorMessage.dismissed = false }
    }
}
