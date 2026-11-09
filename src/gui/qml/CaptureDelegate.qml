// SPDX-License-Identifier: MIT
// One CSV in the Captures list: compare checkbox, process, date, size. Roles from CaptureLibrary.
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
    property bool compared: false
    signal compareToggled(bool on)

    highlighted: selected
    padding: Kirigami.Units.largeSpacing

    contentItem: RowLayout {
        spacing: Kirigami.Units.largeSpacing
        QQC2.CheckBox {
            checked: row.compared
            // Ticked = shown on the right (one capture: its details; several: the comparison). Bound to the
            // page's selection again after the click: a 7th capture is not added (at most 6), and the
            // box must not stay ticked as if it were.
            onToggled: {
                row.compareToggled(checked)
                checked = Qt.binding(() => row.compared)
            }
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.text: row.compared ? "Remove from the view" : "Add to the comparison"
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
        ColumnLayout {
            spacing: 0
            Layout.fillWidth: true
            QQC2.Label {
                text: row.model.processName !== "" ? row.model.processName : row.model.fileName
                color: row.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            QQC2.Label {
                text: Fmt.dateTime(row.model.dateTime) + " · " + Fmt.bytes(row.model.size)
                color: row.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.disabledTextColor
                font: Kirigami.Theme.smallFont
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }
    }
}
