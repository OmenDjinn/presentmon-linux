// SPDX-License-Identifier: MIT
// A FrameTimeChart with axis labels, a legend and an optional time-window picker.
// Live mode (follow): x labels count back from now ("−5 s"); otherwise they are capture time (m:ss, or
// seconds for captures under a minute).
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import PresentMon

ColumnLayout {
    id: panel

    property string title
    property var series: []          // FrameSeries objects
    property var names: []           // legend label per series
    property var colors: [Kirigami.Theme.highlightColor, Kirigami.Theme.positiveTextColor,
                          Kirigami.Theme.neutralTextColor, Kirigami.Theme.negativeTextColor,
                          "#9b59b6", "#1abc9c", "#e67e22", "#7f8c8d"]
    property bool follow: true
    property real windowSeconds: 10
    property var windowChoices: []   // e.g. [5, 10, 30, 60]; empty: no picker
    property var referenceLines: []
    property string referenceLabel: ""
    signal windowPicked(real seconds)

    spacing: Kirigami.Units.smallSpacing

    // Title and time-window picker on one line, the legend below it: in a narrow window the legend
    // wraps onto more lines and never runs under the picker or widens the page.
    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Kirigami.Units.largeSpacing
        Kirigami.Heading {
            text: panel.title
            level: 4
            visible: text !== ""
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.minimumWidth: 0
        }
        QQC2.ComboBox {
            visible: panel.windowChoices.length > 0
            model: panel.windowChoices.map(s => s + " s")
            currentIndex: Math.max(0, panel.windowChoices.indexOf(panel.windowSeconds))
            onActivated: index => panel.windowPicked(panel.windowChoices[index])
        }
    }
    Flow {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Kirigami.Units.largeSpacing
        Repeater {
            model: panel.names
            Row {
                required property int index
                required property string modelData
                spacing: Kirigami.Units.smallSpacing
                Rectangle {
                    width: Kirigami.Units.gridUnit
                    height: 3
                    radius: 1.5
                    color: panel.colors[index % panel.colors.length]
                    anchors.verticalCenter: parent.verticalCenter
                }
                QQC2.Label {
                    text: modelData
                    font: Kirigami.Theme.smallFont
                }
            }
        }
        Row {
            visible: panel.referenceLabel !== "" && panel.referenceLines.length > 0
            spacing: Kirigami.Units.smallSpacing
            Rectangle {
                width: Kirigami.Units.gridUnit
                height: 1
                color: chart.referenceColor
                anchors.verticalCenter: parent.verticalCenter
            }
            QQC2.Label { text: panel.referenceLabel; font: Kirigami.Theme.smallFont }
        }
    }

    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: Kirigami.Units.gridUnit * 10

        // y labels on the left, x labels below; the chart fills the rest.
        readonly property real labelWidth: Kirigami.Units.gridUnit * 2.5
        readonly property real labelHeight: Kirigami.Units.gridUnit * 1.2

        FrameTimeChart {
            id: chart
            x: parent.labelWidth
            y: Kirigami.Units.smallSpacing
            width: parent.width - parent.labelWidth
            height: parent.height - parent.labelHeight - y
            series: panel.series
            colors: panel.colors
            follow: panel.follow
            windowSeconds: panel.follow ? panel.windowSeconds : 0
            referenceLines: panel.referenceLines
            gridColor: Qt.alpha(Kirigami.Theme.textColor, 0.10)
            referenceColor: Qt.alpha(Kirigami.Theme.textColor, 0.45)
            lineWidth: 1.5
        }

        Rectangle {   // frame around the plot area
            x: chart.x
            y: chart.y
            width: chart.width
            height: chart.height
            color: "transparent"
            border.color: Qt.alpha(Kirigami.Theme.textColor, 0.15)
        }

        Repeater {
            model: chart.yTicks
            QQC2.Label {
                required property real modelData
                x: chart.x - width - Kirigami.Units.smallSpacing
                // Centred on its grid line, but never below the plot (the x labels live there).
                y: Math.min(chart.y + chart.yToPixel(modelData) - height / 2, chart.y + chart.height - height)
                text: Fmt.tick(modelData, chart.yTicks.length > 1 ? chart.yTicks[1] - chart.yTicks[0] : 1)
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
            }
        }
        QQC2.Label {
            x: 0
            y: chart.y - height - 1
            visible: false   // unit shown in the title instead
            text: "ms"
        }
        Repeater {
            model: chart.xTicks
            QQC2.Label {
                required property real modelData
                // Centred on its tick, but kept inside the plot's width (clear of the y labels).
                x: Math.max(chart.x, Math.min(chart.x + chart.width - width, chart.x + chart.xToPixel(modelData) - width / 2))
                y: chart.y + chart.height + 2
                visible: x >= chart.x - width / 2 && x + width <= chart.x + chart.width + width / 2
                text: {
                    // Captures under a minute in seconds, with the decimals the tick step needs: m:ss
                    // would print "0:00" for every tick of a one-second capture.
                    if (!panel.follow)
                        return chart.visibleXMax < 60
                               ? Fmt.tick(modelData, chart.xTicks.length > 1 ? chart.xTicks[1] - chart.xTicks[0] : 1) + " s"
                               : Fmt.duration(modelData)
                    const back = Math.round(chart.visibleXMax - modelData)   // a tick just left of the edge rounds to 0
                    return back <= 0 ? "now" : "−" + back + " s"
                }
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
            }
        }
    }
}
