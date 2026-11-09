// SPDX-License-Identifier: MIT
// One layer-enabled process in the Live page's list. Roles come from ProcessModel (through `model`,
// because its "width"/"height" roles would shadow the item's own properties).
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import PresentMon

QQC2.ItemDelegate {
    id: row

    required property int index
    required property var model

    property bool selected: false
    readonly property int pid: model.pid
    readonly property bool recordingThis: Recorder.recording && Recorder.pid === pid
    // Text colours that stay readable on the selection highlight.
    readonly property color mainColor: highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
    readonly property color dimColor: highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.disabledTextColor

    highlighted: selected
    padding: Kirigami.Units.largeSpacing

    contentItem: RowLayout {
        spacing: Kirigami.Units.largeSpacing

        ColumnLayout {
            spacing: 0
            Layout.fillWidth: true
            QQC2.Label {
                text: row.model.name
                color: row.mainColor
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            QQC2.Label {
                text: {
                    const m = row.model
                    const parts = [m.runtime]
                    if (m.width > 0)
                        parts.push(m.width + "×" + m.height)
                    if (m.presentMode)
                        parts.push(m.presentMode)
                    if (m.hdr)
                        parts.push("HDR")
                    return parts.filter(p => p).join(" · ")
                }
                color: row.dimColor
                font: Kirigami.Theme.smallFont
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            QQC2.Label {
                text: "PID " + row.pid
                color: row.dimColor
                font: Kirigami.Theme.smallFont
            }
        }

        ColumnLayout {
            spacing: 0
            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
            QQC2.Label {
                text: row.model.idle ? "idle" : Fmt.fps(row.model.presentRate)
                font.weight: Font.DemiBold
                font.features: { "tnum": 1 }
                color: row.model.idle ? row.dimColor : row.mainColor
                Layout.alignment: Qt.AlignRight
            }
            RowLayout {
                spacing: Kirigami.Units.smallSpacing
                Layout.alignment: Qt.AlignRight
                Kirigami.Icon {
                    source: "media-record"
                    visible: row.recordingThis
                    color: Kirigami.Theme.negativeTextColor
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: implicitWidth
                }
                Kirigami.Icon {
                    source: "view-visible"
                    color: row.mainColor
                    visible: row.model.overlayAvailable && row.model.overlayVisible
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: implicitWidth
                    opacity: 0.7
                }
                QQC2.Label {
                    text: "fps"
                    visible: !row.model.idle
                    color: row.dimColor
                    font: Kirigami.Theme.smallFont
                }
            }
        }
    }
}
