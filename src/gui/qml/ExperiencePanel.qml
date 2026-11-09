// SPDX-License-Identifier: MIT
// The experience score (pml/metrics/experience.hpp): score out of 100 with a meter, the verdict and its main
// cause, and the points each term took off. Laid out like VrrPanel. Used for the live session and for a capture.
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Rectangle {
    id: panel

    property int score: -1          // 0-100, -1 = none
    property string verdict: ""     // "smooth", "minor issues", "rough", "very rough"
    property string cause: ""       // "low frame rate", "judder (uneven motion)", "stutters", "high latency"
    property string detail: ""
    property string source: ""

    readonly property color scoreColor: score >= 85 ? Kirigami.Theme.positiveTextColor
                                      : score >= 65 ? Kirigami.Theme.neutralTextColor
                                      : Kirigami.Theme.negativeTextColor
    readonly property string headline: {
        if (score < 0)
            return "Not enough displayed frames yet"
        return verdict.charAt(0).toUpperCase() + verdict.slice(1)
    }
    readonly property string explanation: {
        if (score < 0)
            return "The score needs display times (VK_EXT_present_timing) and at least 50 displayed frames."
        if (cause === "low frame rate")
            return "Few frames reach the screen. The benefit of more frames is roughly linear in frame time and flattens above about 90 fps, so the step up matters most at low rates."
        if (cause.indexOf("judder") === 0)
            return "Motion on screen does not move in even steps: frames stay on screen for unequal times compared with the game's own timeline (for example 90 fps on a fixed 120 Hz screen, or uneven frame times). VRR, a frame cap the refresh rate divides, or steadier frame times fix it."
        if (cause === "stutters")
            return "Single frames stay on screen much longer, or the motion jumps (hitches). Their size matters more than how often they come: under about 50 ms they are hardly noticed, 150 ms or more is disruptive."
        if (cause === "high latency")
            return "It takes long from the start of a frame to its first pixel. A frame cap or Reflex keeps the game from queuing frames; V-Sync without VRR adds a refresh or more."
        return "Frames reach the screen often, evenly and quickly."
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
                text: "EXPERIENCE"
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
                text: panel.headline + (panel.cause !== "" ? " — " + panel.cause : "")
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
