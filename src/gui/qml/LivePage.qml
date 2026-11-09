// SPDX-License-Identifier: MIT
// Live page: the layer-enabled processes on the left; for the selected one, the overlay's numbers,
// a scrolling frame-time chart, recording and the in-game overlay controls.
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import PresentMon

Kirigami.Page {
    id: page

    property var settings
    property int selectedPid: 0
    signal openCaptureRequested(string file)
    signal settingsRequested()

    // The selected row's roles (reactive: the delegate's model object).
    readonly property var current: list.currentItem ? list.currentItem.model : null
    // Bumped on every structural change of ProcessModel: a process can exit and another appear in
    // the same poll, which leaves `count` unchanged.
    property int modelRevision: 0

    title: "Live"
    padding: 0

    LiveSession {
        id: live
        pid: page.selectedPid
        running: page.isCurrentPage
        historySeconds: page.settings ? page.settings.historySeconds : 10
    }

    // Keep a selection while processes come and go: the busiest one when the selected one is gone.
    function ensureSelection() {
        if (ProcessModel.count === 0)
            return
        if (ProcessModel.indexOfPid(selectedPid) >= 0)
            return
        const busy = ProcessModel.busiestPid()
        selectedPid = busy !== 0 ? busy : ProcessModel.get(0).pid
    }
    Connections {
        target: ProcessModel
        function onRowsInserted() { page.processesChanged() }
        function onRowsRemoved() { page.processesChanged() }
        function onModelReset() { page.processesChanged() }
    }
    function processesChanged() {
        modelRevision++
        ensureSelection()
    }
    Component.onCompleted: ensureSelection()

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 6
        visible: ProcessModel.count === 0
        icon.name: "view-statistics"
        text: "No game is running with the PresentMon layer"
        explanation: "Start a Vulkan game (native, or DXVK / VKD3D-Proton under Proton) with PRESENTMON_LAYER=1 in its environment. "
                     + "For a Steam game, put this into its launch options:\n\n" + LayerStatus.steamLaunchOptions
        helpfulAction: Kirigami.Action {
            text: "Copy launch options"
            icon.name: "edit-copy"
            onTriggered: LayerStatus.copyToClipboard(LayerStatus.steamLaunchOptions)
        }
    }

    RowLayout {
        anchors.fill: parent
        visible: ProcessModel.count > 0
        spacing: 0

        QQC2.ScrollView {
            // A share of the page in narrow windows, so the details keep room.
            Layout.preferredWidth: Math.min(Kirigami.Units.gridUnit * 16, page.width * 0.3)
            Layout.minimumWidth: Kirigami.Units.gridUnit * 8
            Layout.fillHeight: true
            ListView {
                id: list
                model: ProcessModel
                clip: true
                // Re-evaluated when rows come and go (indices shift).
                currentIndex: { page.modelRevision; return ProcessModel.indexOfPid(page.selectedPid) }
                delegate: ProcessDelegate {
                    width: ListView.view.width
                    selected: pid === page.selectedPid
                    onClicked: page.selectedPid = pid
                }
            }
        }

        Kirigami.Separator { Layout.fillHeight: true }

        QQC2.ScrollView {
            id: detailScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff

            ColumnLayout {
                width: detailScroll.availableWidth
                spacing: Kirigami.Units.largeSpacing

                // Header: process and swapchain
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.largeSpacing * 2
                    Layout.bottomMargin: 0
                    spacing: 0
                    Kirigami.Heading {
                        text: page.current ? page.current.name : ""
                        level: 1
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    QQC2.Label {
                        text: {
                            const parts = []
                            if (live.swapchain !== "")
                                parts.push(live.swapchain)
                            if (!isNaN(live.refreshHz))
                                parts.push(Fmt.num(live.refreshHz, 0) + " Hz")
                            if (live.swapchainCount > 1)
                                parts.push(live.swapchainCount + " swapchains (showing the busiest)")
                            if (page.current)
                                parts.push("PID " + page.current.pid)
                            return parts.join(" · ")
                        }
                        color: Kirigami.Theme.disabledTextColor
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: Kirigami.Units.largeSpacing * 2
                    Layout.rightMargin: Kirigami.Units.largeSpacing * 2
                    spacing: Kirigami.Units.smallSpacing
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: live.attachError !== ""
                        type: Kirigami.MessageType.Error
                        text: "Cannot read this process: " + live.attachError
                    }
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: live.attached && live.idle
                        type: Kirigami.MessageType.Information
                        text: "The game is not presenting frames (paused, minimized, loading, or it exited)."
                    }
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: live.lostRecords > 0
                        type: Kirigami.MessageType.Warning
                        text: live.lostRecords + " frames were overwritten before they could be read; the numbers skip them."
                    }
                }

                // The overlay's numbers
                GridLayout {
                    id: tiles
                    Layout.fillWidth: true
                    Layout.leftMargin: Kirigami.Units.largeSpacing * 2
                    Layout.rightMargin: Kirigami.Units.largeSpacing * 2
                    // 8 tiles: one row of 8, two of 4, four of 2 or eight of 1 (never a lone tile on a
                    // row). From the space the page has, not from what the tiles would like.
                    readonly property real avail: detailScroll.availableWidth - Kirigami.Units.largeSpacing * 4
                    columns: avail >= Kirigami.Units.gridUnit * 72 ? 8 : (avail >= Kirigami.Units.gridUnit * 36 ? 4 : (avail >= Kirigami.Units.gridUnit * 16 ? 2 : 1))
                    columnSpacing: Kirigami.Units.largeSpacing
                    rowSpacing: Kirigami.Units.largeSpacing

                    StatTile {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        label: "FRAME RATE"
                        value: Fmt.fps(live.presentFps)
                        unit: "fps"
                        detail: live.hasDisplayInfo ? "displayed " + Fmt.fps(live.displayFps) + " fps" : "presents per second"
                        toolTip: "Presents per second over the last second; below, distinct screen updates per second."
                    }
                    StatTile {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        label: "FRAME TIME"
                        value: Fmt.ms(live.avgMs)
                        unit: "ms"
                        detail: "p99 " + Fmt.ms(live.p99Ms) + " ms"
                        toolTip: "Average time between presents over the last 10 s, and its 99th percentile (p99)."
                    }
                    StatTile {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        label: "1% LOW"
                        value: Fmt.fps(live.low1Fps)
                        unit: "fps"
                        detail: "0.1% low " + Fmt.fps(live.low01Fps) + " fps, GQI " + Fmt.num(live.gqi, 2)
                        toolTip: "Frame rate of the average of the slowest 1 % (0.1 %) of the frames of the last 10 s. %1".arg("Gaming Quality Index (Blackbird PC Tech): (1% low / 60) x (1 - (average - 1% low) / average). 1 or more: 1% lows at 60 fps or better, with little spread. Its 1% low is time-weighted (the frame at which the slowest frames add up to 1 % of the time), so it can differ from the 1% low shown here.")
                    }
                    StatTile {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        label: "LATENCY"
                        value: Fmt.ms(live.displayLatencyMs)
                        unit: "ms"
                        detail: "present→screen " + Fmt.ms(live.untilDisplayedMs) + " ms"
                        toolTip: "Display latency: from the frame's CPU start to the screen (PresentMon DisplayLatency). Needs display times (Wayland)."
                    }
                    StatTile {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        label: "CPU BUSY"
                        value: Fmt.ms(live.cpuBusyMs)
                        unit: "ms"
                        detail: Fmt.ms(live.cpuWaitMs) + " ms wait"
                        toolTip: "CPU time per frame spent between presents (busy) and inside the present call (wait). Under DXVK this is DXVK's presenter thread."
                    }
                    StatTile {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        label: "GPU BUSY"
                        value: Fmt.ms(live.gpuBusyMs)
                        unit: "ms"
                        detail: isNaN(live.gpuBusyMs) ? "no GPU data" : Fmt.ms(live.gpuWaitMs) + " ms wait"
                        toolTip: "GPU time per frame spent on the game's work (busy; idle gaps excluded), and the rest of the time from the frame's first GPU work until it was ready (wait). PresentMon GPUBusy / GPUWait."
                    }
                    StatTile {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        label: "GPU LATENCY"
                        value: Fmt.ms(live.gpuLatencyMs)
                        unit: "ms"
                        detail: "CPU start→GPU start"
                        toolTip: "From the frame's CPU start to the start of its first GPU work (PresentMon GPULatency)."
                    }
                    StatTile {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        label: "DROPPED"
                        value: Fmt.pct(live.droppedPct)
                        unit: "%"
                        detail: live.hasDisplayInfo ? "frames never shown" : "needs display times"
                        toolTip: "Presented frames that never reached the screen (replaced by a newer one), last 10 s."
                    }
                }

                VrrPanel {
                    Layout.fillWidth: true
                    Layout.leftMargin: Kirigami.Units.largeSpacing * 2
                    Layout.rightMargin: Kirigami.Units.largeSpacing * 2
                    visible: live.hasDisplayInfo
                    score: live.vrrScore
                    state_: live.vrrState
                    verdict: live.vrrVerdict
                    cause: live.vrrCause
                    detail: live.vrrDetail
                    source: live.vrrSource
                }

                ExperiencePanel {
                    Layout.fillWidth: true
                    Layout.leftMargin: Kirigami.Units.largeSpacing * 2
                    Layout.rightMargin: Kirigami.Units.largeSpacing * 2
                    visible: live.hasDisplayInfo
                    score: live.experienceScore
                    verdict: live.experienceVerdict
                    cause: live.experienceCause
                    detail: live.experienceDetail
                    source: "Last 10 s of displayed frames. How the score is built: docs/ARCHITECTURE.md, 3.4.3."
                }

                ChartPanel {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 16
                    Layout.leftMargin: Kirigami.Units.largeSpacing * 2
                    Layout.rightMargin: Kirigami.Units.largeSpacing * 2
                    title: "Frame times (ms)"
                    follow: true
                    series: live.hasDisplayInfo ? [live.frameTimes, live.displayTimes] : [live.frameTimes]
                    names: live.hasDisplayInfo ? ["Between presents", "Between display changes"] : ["Between presents"]
                    windowSeconds: live.historySeconds
                    windowChoices: [5, 10, 30, 60]
                    onWindowPicked: seconds => {
                        if (page.settings)
                            page.settings.historySeconds = seconds
                        else
                            live.historySeconds = seconds
                    }
                    referenceLines: isNaN(live.refreshHz) ? [] : [1000 / live.refreshHz]
                    referenceLabel: "Refresh interval"
                }

                GridLayout {
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.largeSpacing * 2
                    Layout.topMargin: 0
                    columns: detailScroll.availableWidth > Kirigami.Units.gridUnit * 50 ? 2 : 1
                    columnSpacing: Kirigami.Units.gridUnit
                    rowSpacing: Kirigami.Units.gridUnit

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: Kirigami.Units.smallSpacing
                        Kirigami.Heading { text: "Record"; level: 3 }
                        QQC2.Label {
                            text: "Writes a PresentMon CSV to " + Recorder.folder
                            color: Kirigami.Theme.disabledTextColor
                            font: Kirigami.Theme.smallFont
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }
                        RecordControls {
                            Layout.fillWidth: true
                            pid: page.current ? page.current.pid : 0
                            processName: page.current ? page.current.name : ""
                            settings: page.settings
                            onOpenCaptureRequested: file => page.openCaptureRequested(file)
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: Kirigami.Units.smallSpacing
                        Kirigami.Heading { text: "In-game overlay"; level: 3 }
                        OverlayControls {
                            Layout.fillWidth: true
                            pid: page.current ? page.current.pid : 0
                            available: page.current ? page.current.overlayAvailable : false
                            overlayVisible: page.current ? page.current.overlayVisible : false
                            position: page.current ? page.current.overlayPosition : 1
                            scale: page.current ? page.current.overlayScale : 0
                            panels: page.current ? page.current.overlayPanels : 0
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: Kirigami.Units.smallSpacing
                        Kirigami.Heading { text: "Frame cap"; level: 3 }
                        FrameCapControls {
                            Layout.fillWidth: true
                            pid: page.current ? page.current.pid : 0
                            available: page.current ? page.current.capAvailable : false
                            mode: page.current ? page.current.capMode : 0
                            fps: page.current ? page.current.capFps : 0
                            placement: page.current ? page.current.capPlacement : 0
                            reflex: page.current ? page.current.capReflex : true
                            effective: page.current ? page.current.capEffective : 0
                            method: page.current ? page.current.capMethod : 0
                            waitMs: page.current ? page.current.capWaitMs : 0
                            vrr: page.current ? page.current.capVrr : 0
                            refreshHz: page.current ? page.current.capRefresh : 0
                        }
                    }
                }
            }
        }
    }
}
