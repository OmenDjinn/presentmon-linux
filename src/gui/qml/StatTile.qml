// SPDX-License-Identifier: MIT
// One number of the live or capture summary: label, big value with unit, optional second line.
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Rectangle {
    id: tile

    property string label
    property string value
    property string unit
    property string detail
    property string toolTip
    property color accent: Kirigami.Theme.textColor

    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false
    color: Kirigami.Theme.backgroundColor
    radius: Kirigami.Units.cornerRadius
    border.color: Qt.alpha(Kirigami.Theme.textColor, 0.12)
    border.width: 1
    implicitWidth: Kirigami.Units.gridUnit * 9
    implicitHeight: column.implicitHeight + Kirigami.Units.largeSpacing * 2

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: Kirigami.Units.largeSpacing
        spacing: 0

        QQC2.Label {
            text: tile.label
            color: Kirigami.Theme.disabledTextColor
            font: Kirigami.Theme.smallFont
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Layout.fillWidth: true
            QQC2.Label {
                text: tile.value
                color: tile.accent
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 2
                font.weight: Font.DemiBold
                font.features: { "tnum": 1 }   // tabular digits: numbers do not jitter while updating
                // In a narrow tile a long value gets a smaller font (down to the normal size), then an
                // ellipsis, instead of running out of the tile.
                fontSizeMode: Text.HorizontalFit
                minimumPointSize: Kirigami.Theme.defaultFont.pointSize
                elide: Text.ElideRight
                Layout.fillWidth: true
                Layout.maximumWidth: implicitWidth
            }
            QQC2.Label {
                text: tile.unit
                visible: tile.unit !== "" && tile.value !== "–"
                color: Kirigami.Theme.disabledTextColor
                Layout.alignment: Qt.AlignBaseline
            }
        }
        QQC2.Label {
            text: tile.detail
            visible: tile.detail !== ""
            color: Kirigami.Theme.disabledTextColor
            font: Kirigami.Theme.smallFont
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
    }

    HoverHandler { id: hover }
    QQC2.ToolTip.visible: tile.toolTip !== "" && hover.hovered
    QQC2.ToolTip.text: tile.toolTip
    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
}
