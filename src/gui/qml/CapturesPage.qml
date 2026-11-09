// SPDX-License-Identifier: MIT
// Captures page: the CSVs in the capture folder. One selected: its numbers (same definitions as the
// overlay), frame times over the whole capture and the frame-time distribution. Several ticked:
// side by side, with the change against the first.
import QtQml.Models
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import PresentMon

Kirigami.Page {
    id: page

    property var initialPaths: []
    signal liveRequested()

    // Selected captures (paths). One: details; several: comparison (first = baseline).
    property var selectedPaths: []
    property var analyses: []
    readonly property int maxCompared: 6
    readonly property var colors: [Kirigami.Theme.highlightColor, Kirigami.Theme.negativeTextColor,
                                   Kirigami.Theme.positiveTextColor, Kirigami.Theme.neutralTextColor,
                                   "#9b59b6", "#1abc9c"]
    readonly property bool loading: analyses.some(a => a.loading)
    // The file the Recorder is writing right now: never auto-selected or trashed, reloaded when done.
    readonly property string recordingFile: Recorder.recording ? Recorder.outputFile : ""
    readonly property var trashablePaths: {
        CaptureLibrary.count
        return selectedPaths.filter(p => CaptureLibrary.indexOfPath(p) >= 0 && p !== recordingFile)
    }
    // Why Move to Trash leaves (some of) the selection alone, shown on the page: a disabled toolbar
    // button shows no tooltip.
    readonly property string trashNote: {
        const kept = selectedPaths.length - trashablePaths.length
        if (kept === 0)
            return ""
        if (kept === 1 && recordingFile !== "" && selectedPaths.indexOf(recordingFile) >= 0)
            return "Still being recorded: read again when the recording stops."
        return selectedPaths.length === 1 ? "Not in the capture folder, so Move to Trash does not apply to it."
                                          : "Captures from outside the capture folder are not moved to the Trash."
    }

    title: "Captures"
    padding: 0

    actions: [
        Kirigami.Action {
            text: "Open folder"
            icon.name: "folder-open"
            onTriggered: CaptureLibrary.openFolder()
        },
        Kirigami.Action {
            text: "Move to Trash"
            icon.name: "edit-delete"
            // Only captures of the capture folder: a CSV opened from elsewhere (--capture) is not ours to delete.
            enabled: page.trashablePaths.length > 0
            onTriggered: trashDialog.open()
        },
        Kirigami.Action {
            text: "Refresh"
            icon.name: "view-refresh"
            onTriggered: CaptureLibrary.refresh()
        }
    ]

    function samePaths(a, b) {
        return a.length === b.length && a.every((x, i) => x === b[i])
    }
    function selectPaths(paths) {
        // Each file once (--capture a.csv --capture a.csv is one capture, not a comparison with itself).
        const p = paths.filter((x, i) => paths.indexOf(x) === i).slice(0, maxCompared)
        if (!samePaths(p, selectedPaths))   // re-selecting the shown capture must not reload it
            selectedPaths = p
    }
    // The checkboxes: ticked = shown. Ticking a second capture turns the details into a comparison.
    function toggleCompare(path, on) {
        if (on && selectedPaths.length >= maxCompared && selectedPaths.indexOf(path) < 0) {
            applicationWindow().showPassiveNotification("At most " + maxCompared + " captures can be compared")
            return
        }
        const p = selectedPaths.filter(x => x !== path)
        if (on)
            p.push(path)
        selectPaths(p)
    }
    function baseName(path) {
        return path.substring(path.lastIndexOf("/") + 1)
    }

    Component.onCompleted: {
        if (initialPaths && initialPaths.length > 0)
            selectPaths(initialPaths)
        else {
            // The newest finished capture (not the one a running recording is still writing).
            for (let i = 0; i < CaptureLibrary.count; i++) {
                const p = CaptureLibrary.get(i).path
                if (p !== recordingFile) {
                    selectPaths([p])
                    break
                }
            }
        }
    }

    // One CaptureAnalysis per selected path (loads in a worker thread), kept while its path stays
    // selected: ticking one more capture loads only that one. (A JS array as the Instantiator's model
    // would recreate, and re-read, every selected capture on each change.)
    ListModel { id: pathModel }
    Instantiator {
        id: loader
        model: pathModel
        delegate: CaptureAnalysis {
            required property string filePath
            path: filePath
        }
        onObjectAdded: page.rebuild()
        onObjectRemoved: page.rebuild()
    }
    onSelectedPathsChanged: syncModel()
    function syncModel() {
        for (let i = pathModel.count - 1; i >= 0; i--)
            if (selectedPaths.indexOf(pathModel.get(i).filePath) < 0)
                pathModel.remove(i)
        for (const p of selectedPaths) {
            let found = false
            for (let i = 0; i < pathModel.count && !found; i++)
                found = pathModel.get(i).filePath === p
            if (!found)
                pathModel.append({ filePath: p })
        }
        rebuild()
    }
    // The analyses in selection order: the first is the comparison's baseline.
    function rebuild() {
        const byPath = {}
        for (let i = 0; i < loader.count; i++) {
            const o = loader.objectAt(i)
            if (o)
                byPath[o.path] = o
        }
        analyses = selectedPaths.map(p => byPath[p]).filter(o => o !== undefined)
    }
    // A selected capture that was still being written: load it again now that it is complete.
    function reload(path) {
        for (let i = 0; i < pathModel.count; i++) {
            if (pathModel.get(i).filePath === path) {
                pathModel.remove(i)
                break
            }
        }
        syncModel()
    }
    Connections {
        target: Recorder
        function onFinished(file) {
            if (page.selectedPaths.indexOf(file) >= 0)
                page.reload(file)
        }
    }

    Kirigami.PromptDialog {
        id: trashDialog
        title: "Move to Trash"
        subtitle: page.trashablePaths.length === 1
                  ? "Move " + page.baseName(page.trashablePaths[0]) + " to the Trash?"
                  : "Move these " + page.trashablePaths.length + " captures to the Trash?"
        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel
        onAccepted: {
            const paths = page.trashablePaths
            page.selectedPaths = page.selectedPaths.filter(p => paths.indexOf(p) < 0)
            for (const p of paths)
                CaptureLibrary.moveToTrash(p)
        }
    }

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 6
        visible: CaptureLibrary.count === 0 && page.selectedPaths.length === 0
        icon.name: "document-open-recent"
        text: "No captures yet"
        explanation: "Record a game on the Live page. Captures are saved to " + CaptureLibrary.folder + "."
        helpfulAction: Kirigami.Action {
            text: "Go to Live"
            icon.name: "view-statistics"
            onTriggered: page.liveRequested()
        }
    }

    RowLayout {
        anchors.fill: parent
        visible: CaptureLibrary.count > 0 || page.selectedPaths.length > 0
        spacing: 0

        QQC2.ScrollView {
            // Only when the folder has captures: a CSV opened from elsewhere gets the whole width.
            visible: CaptureLibrary.count > 0
            Layout.preferredWidth: Math.min(Kirigami.Units.gridUnit * 17, page.width * 0.3)
            Layout.minimumWidth: Kirigami.Units.gridUnit * 8
            Layout.fillHeight: true
            ListView {
                id: list
                model: CaptureLibrary
                clip: true
                delegate: CaptureDelegate {
                    width: ListView.view.width
                    selected: page.selectedPaths.length === 1 && page.selectedPaths[0] === model.path
                    compared: page.selectedPaths.indexOf(model.path) >= 0
                    onClicked: page.selectPaths([model.path])
                    onCompareToggled: on => page.toggleCompare(model.path, on)
                }
            }
        }

        Kirigami.Separator {
            visible: CaptureLibrary.count > 0
            Layout.fillHeight: true
        }

        QQC2.ScrollView {
            id: detailScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

            ColumnLayout {
                width: detailScroll.availableWidth
                spacing: Kirigami.Units.largeSpacing

                Kirigami.PlaceholderMessage {
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.gridUnit * 3
                    visible: page.selectedPaths.length === 0
                    text: "Select a capture"
                    explanation: "Tick several to compare them."
                }

                // ---- one capture ----
                ColumnLayout {
                    id: single
                    readonly property var a: page.analyses.length === 1 ? page.analyses[0] : null
                    visible: a !== null
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.largeSpacing * 2
                    spacing: Kirigami.Units.largeSpacing

                    ColumnLayout {
                        spacing: 0
                        Layout.fillWidth: true
                        Kirigami.Heading {
                            text: single.a ? (single.a.application || page.baseName(single.a.path)) : ""
                            level: 1
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        QQC2.Label {
                            text: {
                                if (!single.a)
                                    return ""
                                const parts = [page.baseName(single.a.path)]
                                if (single.a.runtime) parts.push(single.a.runtime)
                                if (single.a.presentMode) parts.push(single.a.presentMode)
                                if (single.a.loaded) parts.push(single.a.frames.toLocaleString(Qt.locale(), "f", 0) + " frames in " + Fmt.duration(single.a.durationSeconds))
                                return parts.join(" · ")
                            }
                            color: Kirigami.Theme.disabledTextColor
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }
                        QQC2.Label {
                            visible: page.trashNote !== ""
                            text: page.trashNote
                            color: Kirigami.Theme.disabledTextColor
                            font: Kirigami.Theme.smallFont
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                    }
                    QQC2.BusyIndicator {
                        visible: single.a && single.a.loading
                        running: visible
                    }
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: single.a && !single.a.loading && single.a.error !== ""
                        type: Kirigami.MessageType.Error
                        text: single.a ? "Cannot read this capture: " + single.a.error : ""
                    }
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: single.a && single.a.warnings.length > 0
                        type: Kirigami.MessageType.Warning
                        text: single.a ? single.a.warnings.join("\n") : ""
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        visible: single.a && single.a.loaded
                        // 5 tiles: one row, or rows of 3, 2 or 1, from the space the page has.
                        readonly property real avail: detailScroll.availableWidth - Kirigami.Units.largeSpacing * 4
                        columns: avail >= Kirigami.Units.gridUnit * 45 ? 5 : (avail >= Kirigami.Units.gridUnit * 27 ? 3 : (avail >= Kirigami.Units.gridUnit * 16 ? 2 : 1))
                        columnSpacing: Kirigami.Units.largeSpacing
                        rowSpacing: Kirigami.Units.largeSpacing
                        StatTile {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            label: "AVERAGE"
                            value: Fmt.fps(single.a ? single.a.avgFps : NaN)
                            unit: "fps"
                            detail: Fmt.ms(single.a ? single.a.avgMs : NaN) + " ms per frame"
                        }
                        StatTile {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            label: "1% LOW"
                            value: Fmt.fps(single.a ? single.a.low1Fps : NaN)
                            unit: "fps"
                            detail: "0.1% low " + Fmt.fps(single.a ? single.a.low01Fps : NaN) + " fps"
                            toolTip: "Frame rate of the average of the slowest 1 % (0.1 %) of the frames."
                        }
                        StatTile {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            label: "99TH PERCENTILE"
                            value: Fmt.ms(single.a ? single.a.p99Ms : NaN)
                            unit: "ms"
                            detail: "95th " + Fmt.ms(single.a ? single.a.p95Ms : NaN) + " ms"
                        }
                        StatTile {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            label: "SLOWEST FRAME"
                            value: Fmt.ms(single.a ? single.a.maxMs : NaN)
                            unit: "ms"
                            detail: "median " + Fmt.ms(single.a ? single.a.p50Ms : NaN) + " ms"
                        }
                        StatTile {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            label: "DISPLAYED"
                            value: single.a && single.a.hasDisplay ? Fmt.fps(single.a.displayFps) : Fmt.na
                            unit: "fps"
                            detail: single.a && single.a.hasDisplay ? Fmt.pct(single.a.droppedPct) + " % dropped" : "no display times"
                        }
                    }

                    ChartPanel {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 15
                        visible: single.a && single.a.loaded
                        title: "Frame times (ms)"
                        follow: false
                        series: single.a ? (single.a.hasDisplay ? [single.a.frameTimes, single.a.displayTimes] : [single.a.frameTimes]) : []
                        names: single.a && single.a.hasDisplay ? ["Between presents", "Between display changes"] : ["Between presents"]
                    }

                    Kirigami.Heading {
                        visible: single.a && single.a.loaded
                        text: "Distribution"
                        level: 4
                    }
                    Histogram {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 8
                        visible: single.a && single.a.loaded
                        bins: single.a ? single.a.histogram : []
                        binMs: single.a ? single.a.histogramBinMs : 1
                    }

                    RowLayout {
                        visible: single.a !== null
                        QQC2.Button {
                            text: "Open CSV"
                            icon.name: "document-open"
                            onClicked: CaptureLibrary.openFile(single.a.path)
                        }
                    }
                }

                // ---- comparison ----
                ColumnLayout {
                    id: compare
                    visible: page.analyses.length > 1
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.largeSpacing * 2
                    spacing: Kirigami.Units.largeSpacing

                    Kirigami.Heading {
                        text: "Comparing " + page.analyses.length + " captures"
                        level: 1
                    }
                    QQC2.Label {
                        visible: page.trashNote !== ""
                        text: page.trashNote
                        color: Kirigami.Theme.disabledTextColor
                        font: Kirigami.Theme.smallFont
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    QQC2.BusyIndicator {
                        visible: page.loading
                        running: visible
                    }
                    Repeater {
                        model: page.analyses
                        Kirigami.InlineMessage {
                            required property var modelData
                            Layout.fillWidth: true
                            visible: !modelData.loading && modelData.error !== ""
                            type: Kirigami.MessageType.Error
                            text: page.baseName(modelData.path) + ": " + modelData.error
                        }
                    }
                    Flickable {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredHeight: compareTable.implicitHeight + (hbar.visible ? hbar.height : 0)
                        contentWidth: compareTable.implicitWidth
                        contentHeight: compareTable.implicitHeight
                        flickableDirection: Flickable.HorizontalFlick
                        boundsBehavior: Flickable.StopAtBounds
                        clip: true
                        QQC2.ScrollBar.horizontal: QQC2.ScrollBar {
                            id: hbar
                            policy: compareTable.implicitWidth > parent.width ? QQC2.ScrollBar.AlwaysOn : QQC2.ScrollBar.AlwaysOff
                        }
                        CompareTable {
                            id: compareTable
                            analyses: page.analyses
                            colors: page.colors
                        }
                    }
                    ChartPanel {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 16
                        title: "Frame times (ms)"
                        follow: false
                        colors: page.colors
                        series: page.analyses.map(a => a.frameTimes)
                        names: page.analyses.map(a => page.baseName(a.path).replace(/^pmcap-/, "").replace(/\.csv$/, ""))
                    }
                }
            }
        }
    }
}
