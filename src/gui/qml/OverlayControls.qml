// SPDX-License-Identifier: MIT
// The in-game overlay of one process: shown or hidden, corner, size and panels. Writes the overlay
// control block in the process's segment (ProcessModel.setOverlay*), the same as
// `presentmon overlay ...`; the layer picks it up on the next present.
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import PresentMon

ColumnLayout {
    id: controls

    property int pid: 0
    property bool available: false
    property bool overlayVisible: false
    property int position: 1        // pml::OverlayPosition
    property real scale: 0          // 0 = automatic
    property int panels: 0          // pml::OverlayPanel bits, 0 = default set

    // pml::OverlayPanel bits (src/common/include/pml/protocol.hpp) and the default set the layer
    // uses for panels == 0 (kDefaultPanels in src/layer/overlay/ui.hpp: all of them).
    readonly property var panelList: [
        { bit: 1, label: "Frame rate and lows" },
        { bit: 2, label: "Frame time graph" },
        { bit: 4, label: "CPU" },
        { bit: 8, label: "GPU" },
        { bit: 16, label: "Latency" },
        { bit: 32, label: "Swapchain" },
        { bit: 64, label: "Dropped frames" },
        { bit: 128, label: "VRR stability" }
    ]
    readonly property int defaultPanels: 1 | 2 | 4 | 8 | 16 | 32 | 64
    readonly property int effectivePanels: panels === 0 ? defaultPanels : panels

    spacing: Kirigami.Units.smallSpacing

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: controls.pid !== 0 && !controls.available
        type: Kirigami.MessageType.Information
        text: "The overlay is not loaded in this game (it was started with PRESENTMON_OVERLAY=off, or it has not drawn a frame yet)."
    }

    Kirigami.FormLayout {
        // Natural width at the left: a FormLayout fills and centres itself in a wider column.
        Layout.fillWidth: false
        Layout.alignment: Qt.AlignLeft
        enabled: controls.pid !== 0 && controls.available

        // After each change the controls are bound to the model again: a write that changes nothing (or
        // fails because the game exited) would otherwise leave the clicked state on screen.
        QQC2.Switch {
            Kirigami.FormData.label: "Overlay:"
            text: !controls.available ? "Not loaded" : (checked ? "Shown" : "Hidden")
            checked: controls.available && controls.overlayVisible
            onToggled: {
                ProcessModel.setOverlayVisible(controls.pid, checked)
                checked = Qt.binding(() => controls.available && controls.overlayVisible)
            }
        }

        QQC2.ComboBox {
            Kirigami.FormData.label: "Corner:"
            model: ["Top left", "Top right", "Bottom left", "Bottom right"]
            currentIndex: controls.position
            onActivated: index => {
                ProcessModel.setOverlayPosition(controls.pid, index)
                currentIndex = Qt.binding(() => controls.position)
            }
        }

        RowLayout {
            Kirigami.FormData.label: "Size:"
            QQC2.CheckBox {
                id: autoScale
                text: "Automatic"
                checked: controls.scale === 0
                onToggled: {
                    ProcessModel.setOverlayScale(controls.pid, checked ? 0 : scaleSlider.value)
                    checked = Qt.binding(() => controls.scale === 0)
                }
            }
            QQC2.Slider {
                id: scaleSlider
                enabled: !autoScale.checked
                from: 0.5
                to: 3
                stepSize: 0.25
                snapMode: QQC2.Slider.SnapAlways
                value: controls.scale === 0 ? 1 : controls.scale
                onMoved: ProcessModel.setOverlayScale(controls.pid, value)
                Layout.preferredWidth: Kirigami.Units.gridUnit * 8
            }
            QQC2.Label {
                text: autoScale.checked ? "" : "×" + Fmt.num(scaleSlider.value, 2)
                font.features: { "tnum": 1 }
            }
        }

        ColumnLayout {
            Kirigami.FormData.label: "Panels:"
            Kirigami.FormData.labelAlignment: Qt.AlignTop
            spacing: 0
            Repeater {
                model: controls.panelList
                QQC2.CheckBox {
                    required property var modelData
                    text: modelData.label
                    checked: (controls.effectivePanels & modelData.bit) !== 0
                    onToggled: {
                        let bits = controls.effectivePanels
                        bits = checked ? (bits | modelData.bit) : (bits & ~modelData.bit)
                        // All off would leave an empty box; the layer treats 0 as the default set,
                        // so keep at least the frame rate. Unchecking that last box then changes
                        // nothing, and the rebinding shows it checked again.
                        ProcessModel.setOverlayPanels(controls.pid, bits === 0 ? 1 : bits)
                        checked = Qt.binding(() => (controls.effectivePanels & modelData.bit) !== 0)
                    }
                }
            }
        }
    }
}
