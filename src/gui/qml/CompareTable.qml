// SPDX-License-Identifier: MIT
// Side-by-side numbers of several captures (CaptureAnalysis objects), with the change against the
// first one; green where it is better (higher fps, lower frame time), red where worse.
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import PresentMon

GridLayout {
    id: table

    property var analyses: []
    property var colors: []

    // key: CaptureAnalysis property; higher: true if a higher value is better; kind: formatter.
    readonly property var metrics: [
        { label: "Average frame rate", key: "avgFps", unit: "fps", higher: true, kind: "fps" },
        { label: "1% low", key: "low1Fps", unit: "fps", higher: true, kind: "fps" },
        { label: "0.1% low", key: "low01Fps", unit: "fps", higher: true, kind: "fps" },
        { label: "Average frame time", key: "avgMs", unit: "ms", higher: false, kind: "ms" },
        { label: "95th percentile", key: "p95Ms", unit: "ms", higher: false, kind: "ms" },
        { label: "99th percentile", key: "p99Ms", unit: "ms", higher: false, kind: "ms" },
        { label: "Slowest frame", key: "maxMs", unit: "ms", higher: false, kind: "ms" },
        { label: "Displayed frame rate", key: "displayFps", unit: "fps", higher: true, kind: "fps" },
        { label: "Dropped", key: "droppedPct", unit: "%", higher: false, kind: "pct" },
        { label: "Frames", key: "frames", unit: "", higher: true, kind: "int" },
        { label: "Duration", key: "durationSeconds", unit: "", higher: true, kind: "duration" }
    ]

    function format(kind, v) {
        if (kind === "fps") return Fmt.fps(v)
        if (kind === "ms") return Fmt.ms(v)
        if (kind === "pct") return Fmt.pct(v)
        if (kind === "duration") return Fmt.duration(v)
        return isNaN(v) ? Fmt.na : Number(v).toLocaleString(Qt.locale(), "f", 0)
    }

    columns: 1 + analyses.length
    columnSpacing: Kirigami.Units.gridUnit
    rowSpacing: Kirigami.Units.smallSpacing

    // Header row: capture names with their chart colour.
    QQC2.Label { text: "" }
    Repeater {
        model: table.analyses
        RowLayout {
            required property int index
            required property var modelData
            spacing: Kirigami.Units.smallSpacing
            Rectangle {
                width: Kirigami.Units.gridUnit * 0.8
                height: 3
                color: table.colors[index % table.colors.length]
            }
            QQC2.Label {
                // pmcap-<exe>-YYMMDD-HHMMSS.csv -> "<exe>-YYMMDD-HHMMSS": two captures of one game differ by time.
                text: modelData.path.substring(modelData.path.lastIndexOf("/") + 1).replace(/^pmcap-/, "").replace(/\.csv$/, "")
                      + (index === 0 ? " (baseline)" : "")
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                Layout.maximumWidth: Kirigami.Units.gridUnit * 12
            }
        }
    }

    // One row per metric: label, then one cell per capture.
    Repeater {
        model: table.metrics.length * (1 + table.analyses.length)
        QQC2.Label {
            required property int index
            readonly property int metric: Math.floor(index / (1 + table.analyses.length))
            readonly property int column: index % (1 + table.analyses.length)
            readonly property var m: table.metrics[metric]
            readonly property var a: column > 0 ? table.analyses[column - 1] : null
            readonly property real value: a && a.loaded ? a[m.key] : NaN
            readonly property real base: table.analyses.length > 0 && table.analyses[0].loaded ? table.analyses[0][m.key] : NaN
            readonly property string change: column > 1 && m.kind !== "int" && m.kind !== "duration" ? Fmt.delta(base, value) : ""
            readonly property bool better: m.higher ? value > base : value < base

            text: column === 0 ? m.label
                               : table.format(m.kind, value) + (m.unit !== "" && !isNaN(value) ? " " + m.unit : "")
                                 + (change !== "" ? "   " + change : "")
            color: column === 0 ? Kirigami.Theme.disabledTextColor
                 : change === "" || value === base ? Kirigami.Theme.textColor
                 : better ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor
            font.features: { "tnum": 1 }
        }
    }
}
