// SPDX-License-Identifier: MIT
// The VRR stability verdict (pml/metrics/vrr.hpp): score out of 100 with a meter, the verdict and its main
// cause, the numbers behind it and where the VRR state comes from. Used for the live session (with the
// driver's VRR state) and for a capture (without it).
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Rectangle {
    id: panel

    property int score: -1          // 0-100, -1 = none
    property string state_: ""      // "no data", "unknown", "off", "idle", "active"
    property string verdict: ""
    property string cause: ""
    property string detail: ""
    property string source: ""

    readonly property color scoreColor: score >= 85 ? Kirigami.Theme.positiveTextColor
                                      : score >= 65 ? Kirigami.Theme.neutralTextColor
                                      : Kirigami.Theme.negativeTextColor
    readonly property string headline: {
        if (state_ === "off")
            return "VRR is off"
        if (state_ === "no data" || state_ === "")
            return "Not enough displayed frames yet"
        const v = verdict.charAt(0).toUpperCase() + verdict.slice(1)
        return state_ === "idle" ? v + ", at the refresh cap" : v
    }
    readonly property string explanation: {
        if (state_ === "off")
            return "The screen refreshes at a fixed rate, so there is nothing to judge. KWin turns VRR on only while a fullscreen game is the active window on a VRR screen."
        if (state_ === "idle")
            return "The game runs at the screen's maximum refresh: VRR has nothing to vary. A frame cap a few fps lower keeps VRR in control."
        if (cause === "uneven on screen, game steady")
            return "The game delivers frames evenly, but they reach the screen unevenly: the jitter is added after the game (compositor or driver). Under VRR every jump is a refresh-rate jump, which can flicker on OLED and VA panels."
        if (cause === "game frame times swing")
            return "The game's own frame times jump around, and VRR follows them. A frame cap below the average frame rate smooths this."
        if (cause === "stutters")
            return "Single frames stay on screen much longer than the rest (hitches)."
        if (cause.indexOf("LFC") >= 0)
            return "The frame rate keeps crossing the screen's lowest VRR rate, where the driver starts repeating frames (LFC). Each crossing is a big refresh-rate jump."
        return "Frames reach the screen at a steady rhythm."
    }

    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false
    color: Kirigami.Theme.backgroundColor
    radius: Kirigami.Units.cornerRadius
    border.color: Qt.alpha(Kirigami.Theme.textColor, 0.12)
    border.width: 1
    implicitHeight: row.implicitHeight + Kirigami.Units.largeSpacing * 2

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: Kirigami.Units.largeSpacing
        spacing: Kirigami.Units.largeSpacing * 2

        ColumnLayout {
            spacing: 0
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: Kirigami.Units.gridUnit * 7
            QQC2.Label {
                text: "VRR STABILITY"
                color: Kirigami.Theme.disabledTextColor
                font: Kirigami.Theme.smallFont
            }
            RowLayout {
                spacing: Kirigami.Units.smallSpacing
                QQC2.Label {
                    text: panel.score >= 0 ? String(panel.score) : "–"
                    color: panel.score >= 0 ? panel.scoreColor : Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 2.4
                    font.weight: Font.DemiBold
                    font.features: { "tnum": 1 }
                }
                QQC2.Label {
                    text: "/ 100"
                    visible: panel.score >= 0
                    color: Kirigami.Theme.disabledTextColor
                    Layout.alignment: Qt.AlignBaseline
                }
            }
            // Meter: the score's share of the bar, in the verdict's colour.
            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.smallSpacing
                implicitHeight: Math.round(Kirigami.Units.smallSpacing * 1.5)
                radius: height / 2
                color: Qt.alpha(Kirigami.Theme.textColor, 0.12)
                visible: panel.score >= 0
                Rectangle {
                    width: parent.width * Math.max(0, Math.min(100, panel.score)) / 100
                    height: parent.height
                    radius: parent.radius
                    color: panel.scoreColor
                }
            }
        }

        ColumnLayout {
            spacing: Kirigami.Units.smallSpacing
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            QQC2.Label {
                text: panel.headline + (panel.cause !== "" && panel.state_ !== "off" ? " — " + panel.cause : "")
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            QQC2.Label {
                text: panel.explanation
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            QQC2.Label {
                text: panel.detail
                visible: panel.detail !== ""
                color: Kirigami.Theme.disabledTextColor
                font: Kirigami.Theme.smallFont
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            QQC2.Label {
                text: panel.source
                visible: panel.source !== ""
                color: Kirigami.Theme.disabledTextColor
                font: Kirigami.Theme.smallFont
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }
    }
}
