// SPDX-License-Identifier: MIT
// The frame cap of one process (src/layer/frame_limiter.hpp): off, a fixed rate or auto (just under the VRR maximum),
// where the wait goes, whether the game's Reflex may carry it, and what the layer reports is in force. Writes the cap
// control block (ProcessModel.setCap), the same as `presentmon cap ...`; the layer picks it up on the next present.
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import PresentMon

ColumnLayout {
    id: controls

    property int pid: 0
    property bool available: false
    property int mode: 0            // pml::CapMode: 0 off, 1 fixed, 2 auto
    property real fps: 0            // fixed cap, or auto's upper bound (0 = none)
    property int placement: 0       // pml::CapPlacement: 0 late, 1 early
    property bool reflex: true
    property real effective: 0      // cap in force (0 = none)
    property int method: 0          // pml::CapMethod: 1 late, 2 early, 3 Reflex, 4 held on the game's frame thread
    property real waitMs: 0
    property int vrr: 0             // auto: 0 unknown, 1 on, 2 off
    property real refreshHz: 0

    // The value the spin box starts from when switching to Fixed with no rate set yet.
    readonly property int defaultFps: refreshHz > 0 ? Math.floor(refreshHz - refreshHz * refreshHz / 3600) : 141

    function apply(newMode, newFps, newPlacement, newReflex) {
        ProcessModel.setCap(controls.pid, newMode, newFps, newPlacement, newReflex)
    }

    readonly property string status: {
        if (!controls.available)
            return ""
        if (controls.mode === 0)
            return "No cap."
        if (controls.effective > 0) {
            const rate = Fmt.num(controls.effective, controls.effective % 1 === 0 ? 0 : 2) + " fps"
            if (controls.method === 3)
                return "In force: " + rate + " through the game's Reflex (the wait happens at the start of the game's frame)."
            const where = controls.method === 2 ? "before the present (early)"
                        : controls.method === 4 ? "on the game's frame-pacing thread, where DXVK's own limiter waits"
                                                : "after the present (late)"
            return "In force: " + rate + ", waiting " + where + ", " + Fmt.ms(controls.waitMs) + " ms per frame."
        }
        if (controls.mode === 2)
            return controls.vrr === 2 ? "Auto: VRR is off on every screen, so nothing is capped."
                                      : "Auto: waiting for the screens' VRR state."
        return "Waiting for the next frame."
    }

    spacing: Kirigami.Units.smallSpacing

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: controls.pid !== 0 && !controls.available
        type: Kirigami.MessageType.Information
        text: "This game runs a layer without frame caps. Restart it to use them."
    }

    Kirigami.FormLayout {
        Layout.fillWidth: false
        Layout.alignment: Qt.AlignLeft
        enabled: controls.pid !== 0 && controls.available

        QQC2.ComboBox {
            id: modeBox
            Kirigami.FormData.label: "Cap:"
            model: ["Off", "Fixed frame rate", "Auto (just under the VRR maximum)"]
            currentIndex: controls.mode
            onActivated: index => {
                const rate = index === 1 && controls.fps < 1 ? controls.defaultFps : controls.fps
                controls.apply(index, index === 2 && controls.mode !== 2 ? 0 : rate, controls.placement, controls.reflex)
                currentIndex = Qt.binding(() => controls.mode)
            }
        }

        RowLayout {
            Kirigami.FormData.label: controls.mode === 2 ? "At most:" : "Frame rate:"
            visible: controls.mode !== 0
            QQC2.CheckBox {
                id: limitBox
                visible: controls.mode === 2
                text: "Limit to"
                checked: controls.fps > 0
                onToggled: {
                    controls.apply(2, checked ? Math.max(1, rateBox.value) : 0, controls.placement, controls.reflex)
                    checked = Qt.binding(() => controls.fps > 0)
                }
            }
            QQC2.SpinBox {
                id: rateBox
                from: 1
                to: 10000
                editable: true
                enabled: controls.mode === 1 || controls.fps > 0
                value: controls.fps > 0 ? Math.round(controls.fps) : controls.defaultFps
                onValueModified: {
                    controls.apply(controls.mode, value, controls.placement, controls.reflex)
                    value = Qt.binding(() => controls.fps > 0 ? Math.round(controls.fps) : controls.defaultFps)
                }
            }
            QQC2.Label { text: "fps" }
        }

        QQC2.ComboBox {
            Kirigami.FormData.label: "Wait:"
            visible: controls.mode !== 0
            model: ["After the present (lowest latency)", "Before the present (most even)"]
            currentIndex: controls.placement
            onActivated: index => {
                controls.apply(controls.mode, controls.fps, index, controls.reflex)
                currentIndex = Qt.binding(() => controls.placement)
            }
        }

        QQC2.CheckBox {
            Kirigami.FormData.label: "Reflex:"
            visible: controls.mode !== 0
            enabled: controls.placement === 0
            text: "Use the game's Reflex when it has it"
            checked: controls.reflex
            onToggled: {
                controls.apply(controls.mode, controls.fps, controls.placement, checked)
                checked = Qt.binding(() => controls.reflex)
            }
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            QQC2.ToolTip.text: "With Reflex on in the game and the wait after the present, the cap goes into Reflex's own frame interval, so the game waits before it reads input: lower latency than any wait at the present. Reflex only spaces the frames' starts evenly; \"Before the present\" spaces the frames themselves evenly and never uses it."
        }
    }

    QQC2.Label {
        visible: controls.status !== ""
        text: controls.status
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }
    QQC2.Label {
        visible: controls.available && controls.mode !== 0 && (controls.method === 1 || controls.method === 2)
        text: "In DXVK and VKD3D games with V-Sync off the wait sits on the translation layer's submit thread, like MangoHud's, and adds about a frame of latency (measured: +9 ms at 100 fps). With V-Sync on the cap holds the game's own present wait instead and costs nothing; a game's own limiter, DXVK_FRAME_RATE or Reflex also avoid it."
        color: Kirigami.Theme.disabledTextColor
        font: Kirigami.Theme.smallFont
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }
}
