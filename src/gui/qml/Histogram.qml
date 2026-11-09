// SPDX-License-Identifier: MIT
// Frame-time distribution of one capture: share of frames (percent) per bin, from CaptureAnalysis.
// Every bin has a fixed slot (x = index * slot width), like the labels below it. A Row positioner
// would skip the zero-height bars of empty bins and shift every later bar to the left.
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import PresentMon

ColumnLayout {
    id: hist

    property var bins: []        // percent per bin; the last one holds everything above
    property real binMs: 1
    property color barColor: Kirigami.Theme.highlightColor

    readonly property real maxPct: bins.length ? Math.max.apply(null, bins) : 0

    spacing: Kirigami.Units.smallSpacing

    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: Kirigami.Units.gridUnit * 6

        Item {
            id: bars
            anchors.fill: parent
            anchors.bottomMargin: Kirigami.Units.gridUnit * 1.2
            readonly property real slot: width / Math.max(1, hist.bins.length)

            Repeater {
                model: hist.bins
                Rectangle {
                    required property int index
                    required property real modelData
                    x: index * bars.slot
                    width: Math.max(1, bars.slot - 1)
                    height: hist.maxPct > 0 ? bars.height * modelData / hist.maxPct : 0
                    y: bars.height - height
                    color: hist.barColor
                    opacity: 0.85
                    HoverHandler { id: hover }
                    QQC2.ToolTip.visible: hover.hovered
                    QQC2.ToolTip.text: (index === hist.bins.length - 1 ? "≥ " + Fmt.ms(index * hist.binMs)
                                                                       : Fmt.ms(index * hist.binMs) + "–" + Fmt.ms((index + 1) * hist.binMs))
                                       + " ms: " + Fmt.num(modelData, 1) + " % of frames"
                }
            }
        }
        Repeater {   // x labels at about five places, under the left edge of their bin's slot
            model: hist.bins.length > 0 ? 5 : 0
            QQC2.Label {
                required property int index
                readonly property int bin: Math.round(index * (hist.bins.length - 1) / 4)
                x: Math.min(parent.width - width, Math.max(0, bin * bars.slot - width / 2))
                y: parent.height - height
                text: Fmt.tick(bin * hist.binMs, hist.binMs) + (index === 4 ? "+ ms" : "")
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
            }
        }
    }
}
