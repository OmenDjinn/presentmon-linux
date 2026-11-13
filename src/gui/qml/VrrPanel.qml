// SPDX-License-Identifier: MIT
// The VRR readout (pml/metrics/vrr.hpp), laid out like a monitor's refresh-rate OSD: the refresh rate in large
// digits, its swing, a bar over the panel's VRR range with the swing band and a marker at the current rate, then
// the verdict, why, the numbers behind it and where the VRR state comes from. Used for the live session (with the
// driver's VRR state) and for a capture (the median over the whole recording, without it).
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Rectangle {
    id: panel

    property bool live: true
    property real hz: NaN           // now (live) or the median (capture); the fixed rate when VRR is off
    property real swingHz: NaN
    property real lowHz: NaN        // 5th / 95th percentile of the per-refresh rates
    property real highHz: NaN
    property real minHz: NaN        // the panel's VRR floor (EDID), NaN = unknown
    property real maxHz: NaN        // the refresh rate (VRR maximum), NaN = unknown
    property int severity: -1       // 0 fine, 1 worth a look, 2 bad, -1 none
    property string state_: ""      // "no data", "unknown", "off", "idle", "active"
    property string verdict: ""     // "stable", "varying", "at max", "jittery", "stutter", "below range", "unstable",
                                    // "fixed", "no new frames"
    property string cause: ""
    property string detail: ""
    property string source: ""

    readonly property bool hasData: !isNaN(hz) && state_ !== "no data" && state_ !== ""
    readonly property bool fixed: state_ === "off"
    // Frames stopped coming: a verdict, and a rate only when the driver's behaviour at the floor is known.
    readonly property bool stalled: verdict === "no new frames"
    readonly property color verdictColor: severity >= 2 ? Kirigami.Theme.negativeTextColor
                                        : severity === 1 ? Kirigami.Theme.neutralTextColor
                                        : Kirigami.Theme.positiveTextColor
    readonly property string headline: {
        if (stalled)
            return "No new frames"
        if (!hasData)
            return "Not enough displayed frames yet"
        if (fixed)
            return "VRR is off"
        const v = verdict === "below range" ? "Below the VRR range" : verdict.charAt(0).toUpperCase() + verdict.slice(1)
        return v + (cause !== "" ? " — " + cause : "") + (state_ === "unknown" ? " (VRR state unknown)" : "")
    }
    readonly property string explanation: {
        if (stalled)
            return "No new frame has reached the screen for a while (a loading screen, a paused or stalled game). With VRR on the screen keeps showing the last frame at its lowest VRR rate."
        if (!hasData)
            return "The readout needs display times (VK_EXT_present_timing) and at least 30 displayed frames."
        if (fixed)
            return "The screen refreshes at a fixed rate, so the refresh rate cannot swing. KWin turns VRR on only while a fullscreen game is the active window on a VRR screen."
        if (verdict === "at max")
            return "The game runs at the screen's maximum refresh: VRR has nothing to vary. A frame cap a few fps lower keeps VRR in control."
        if (verdict === "stutter")
            return "Single frames stay on screen much longer than the rest (hitches): the refresh rate drops for that frame and jumps back."
        if (verdict === "below range")
            return "Frames come slower than the screen's lowest VRR rate. The screen cannot wait that long: it shows the previous frame again by itself, and a new frame that arrives meanwhile waits for that. So the screen refreshes more often than the game delivers frames (the rate above counts those refreshes, like the monitor's own counter), the rate jumps between the floor and far above it, and frames arrive unevenly. A common cause of VRR flicker."
        if (verdict === "jittery" || verdict === "unstable") {
            const how = verdict === "unstable" ? "The refresh rate jumps between frames often (15 % of frames or more): uneven motion, and flicker on OLED and VA panels, whose brightness depends on the refresh rate. "
                                               : "The refresh rate jumps between some frames (5-15 %). "
            if (cause === "uneven on screen, game steady")
                return how + "The game delivers frames evenly; the jumps are added after it (compositor, driver, or another screen's animation)."
            return how + "The game's own frame times jump around and VRR follows them. A frame cap below the average frame rate smooths this."
        }
        if (verdict === "varying")
            return "The refresh rate follows the game's frame rate as it rises and falls, smoothly: VRR doing its job. A frame cap holds it still."
        return "The refresh rate holds steady from frame to frame."
    }

    // The bar's scale: the panel's VRR range when known, else around what was measured.
    readonly property real scaleLo: !isNaN(minHz) ? Math.min(minHz, isNaN(lowHz) ? minHz : lowHz)
                                  : Math.max(0, Math.floor(Math.min(hz, isNaN(lowHz) ? hz : lowHz) * 0.8 / 10) * 10)
    readonly property real scaleHi: !isNaN(maxHz) ? Math.max(maxHz, isNaN(highHz) ? maxHz : highHz)
                                  : Math.ceil(Math.max(hz, isNaN(highHz) ? hz : highHz) * 1.1 / 10) * 10
    function frac(v) {
        if (isNaN(v) || !(scaleHi > scaleLo))
            return 0
        return Math.max(0, Math.min(1, (v - scaleLo) / (scaleHi - scaleLo)))
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
            Layout.preferredWidth: Kirigami.Units.gridUnit * 10
            Layout.maximumWidth: Kirigami.Units.gridUnit * 10
            QQC2.Label {
                text: panel.live ? "VRR REFRESH RATE" : "VRR REFRESH RATE (MEDIAN)"
                color: Kirigami.Theme.disabledTextColor
                font: Kirigami.Theme.smallFont
            }
            RowLayout {
                spacing: Kirigami.Units.smallSpacing
                QQC2.Label {
                    // VRR off with the fixed rate unknown (the game itself on the refresh grid): no number to show.
                    readonly property bool rateUnknown: panel.fixed && isNaN(panel.maxHz)
                    id: rateLabel
                    text: !panel.hasData ? "–" : rateUnknown ? "Off" : Math.round(panel.hz).toString()
                    color: panel.hasData ? Kirigami.Theme.textColor : Kirigami.Theme.disabledTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 2.4
                    font.weight: Font.DemiBold
                    font.features: { "tnum": 1 }
                }
                QQC2.Label {
                    text: "Hz"
                    visible: panel.hasData && !rateLabel.rateUnknown
                    color: Kirigami.Theme.disabledTextColor
                    Layout.alignment: Qt.AlignBaseline
                }
                QQC2.Label {
                    // The swing: how far the per-frame refresh rate moves around (5th to 95th percentile).
                    text: panel.fixed ? "fixed" : "±" + Math.round(panel.swingHz)
                    visible: panel.hasData && !rateLabel.rateUnknown && (panel.fixed || !isNaN(panel.swingHz))
                    color: panel.fixed ? Kirigami.Theme.disabledTextColor : panel.verdictColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.4
                    font.weight: Font.DemiBold
                    font.features: { "tnum": 1 }
                    Layout.alignment: Qt.AlignBaseline
                    Layout.leftMargin: Kirigami.Units.smallSpacing
                    QQC2.ToolTip.visible: swingHover.hovered
                    QQC2.ToolTip.text: panel.fixed ? "VRR is off: the refresh rate does not move."
                                                   : "Swing: half the spread of the per-frame refresh rate, 5th to 95th percentile (" + Math.round(panel.lowHz) + "–" + Math.round(panel.highHz) + " Hz)" + (panel.live ? " over the last 2 s." : " over the recording.")
                    HoverHandler { id: swingHover }
                }
            }
            QQC2.Label {
                text: panel.verdict
                visible: panel.hasData && !panel.fixed
                color: panel.verdictColor
                font.weight: Font.DemiBold
            }
            // Range bar: the scale (the panel's VRR range when known), the swing band, and a marker at the rate.
            Item {
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.smallSpacing
                implicitHeight: Kirigami.Units.gridUnit * 0.8
                visible: panel.hasData && !panel.fixed
                Rectangle {
                    id: track
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: Math.round(Kirigami.Units.smallSpacing * 1.5)
                    radius: height / 2
                    color: Qt.alpha(Kirigami.Theme.textColor, 0.12)
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    x: track.width * panel.frac(panel.lowHz)
                    width: Math.max(height, track.width * (panel.frac(panel.highHz) - panel.frac(panel.lowHz)))
                    height: track.height
                    radius: height / 2
                    color: Qt.alpha(panel.verdictColor, 0.45)
                    Behavior on x { NumberAnimation { duration: 200 } }
                    Behavior on width { NumberAnimation { duration: 200 } }
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    x: track.width * panel.frac(panel.hz) - width / 2
                    width: Math.max(2, Math.round(Kirigami.Units.smallSpacing / 2))
                    height: parent.height
                    radius: width / 2
                    color: panel.verdictColor
                    Behavior on x { NumberAnimation { duration: 200 } }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                visible: panel.hasData && !panel.fixed
                QQC2.Label {
                    text: Math.round(panel.scaleLo) + (isNaN(panel.minHz) ? "" : " (floor)")
                    color: Kirigami.Theme.disabledTextColor
                    font: Kirigami.Theme.smallFont
                }
                Item { Layout.fillWidth: true }
                QQC2.Label {
                    text: Math.round(panel.scaleHi) + (isNaN(panel.maxHz) ? "" : " (max)")
                    color: Kirigami.Theme.disabledTextColor
                    font: Kirigami.Theme.smallFont
                }
            }
        }

        ColumnLayout {
            spacing: Kirigami.Units.smallSpacing
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            QQC2.Label {
                text: panel.headline
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
