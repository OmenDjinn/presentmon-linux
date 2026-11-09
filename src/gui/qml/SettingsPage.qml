// SPDX-License-Identifier: MIT
// Settings: recording defaults, global shortcuts, the Vulkan layer's state and how to enable it.
import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import PresentMon

FormCard.FormCardPage {
    id: page

    property var settings

    readonly property var durations: [0, 10, 30, 60, 120, 300]
    readonly property var histories: [5, 10, 30, 60]

    title: "Settings"

    Component.onCompleted: LayerStatus.refresh()

    FormCard.FormHeader { title: "Recording" }
    FormCard.FormCard {
        FormCard.FormFolderDelegate {
            id: folderPicker
            label: "Capture folder"
            description: Recorder.folder
            // Encoded per segment: "#", "?" or "%" in a folder name would break a plain "file://" string.
            currentFolder: "file://" + Recorder.folder.split("/").map(encodeURIComponent).join("/")
            onAccepted: {
                const path = decodeURIComponent(selectedFolder.toString().replace(/^file:\/\//, ""))
                if (path !== "") {
                    page.settings.captureFolder = path
                    Recorder.folder = path
                }
            }
        }
        FormCard.FormDelegateSeparator {}
        FormCard.FormComboBoxDelegate {
            text: "Recording length"
            description: "Used by the Record button and the global shortcut."
            model: page.durations.map(s => s === 0 ? "Until stopped" : (s < 60 ? s + " seconds" : (s / 60) + (s === 60 ? " minute" : " minutes")))
            currentIndex: Math.max(0, page.durations.indexOf(page.settings.recordSeconds))
            onActivated: index => page.settings.recordSeconds = page.durations[index]
        }
        FormCard.FormDelegateSeparator {}
        FormCard.FormComboBoxDelegate {
            text: "CSV columns"
            description: "PresentMon 2.x columns are the default of current PresentMon; 1.x matches older tools."
            model: ["PresentMon 2.x", "PresentMon 1.x"]
            currentIndex: page.settings.v1Metrics ? 1 : 0
            onActivated: index => page.settings.v1Metrics = index === 1
        }
        FormCard.FormDelegateSeparator {}
        FormCard.FormSwitchDelegate {
            text: "Stop when the game exits"
            checked: page.settings.stopOnExit
            onToggled: page.settings.stopOnExit = checked
        }
    }

    FormCard.FormHeader { title: "Global shortcuts" }
    FormCard.FormCard {
        FormCard.FormSwitchDelegate {
            text: "Enable global shortcuts"
            description: "They work while a game has focus and act on the game selected on the Live page (or the busiest one)."
            checked: page.settings.hotkeysEnabled
            onToggled: {
                page.settings.hotkeysEnabled = checked
                Hotkeys.enabled = checked && !applicationWindow().testMode
            }
        }
        FormCard.FormDelegateSeparator {}
        FormCard.FormTextDelegate {
            text: "Start or stop recording"
            description: Hotkeys.available ? (Hotkeys.recordShortcut !== "" ? Hotkeys.recordShortcut : "No key assigned") : "Off"
        }
        FormCard.FormDelegateSeparator {}
        FormCard.FormTextDelegate {
            text: "Show or hide the overlay"
            description: Hotkeys.available ? (Hotkeys.overlayShortcut !== "" ? Hotkeys.overlayShortcut : "No key assigned") : "Off"
        }
        FormCard.FormSectionText {
            text: "Change the keys in System Settings → Keyboard → Shortcuts → PresentMon."
        }
    }

    FormCard.FormHeader { title: "Vulkan layer" }
    FormCard.FormCard {
        FormCard.FormTextDelegate {
            text: LayerStatus.installed ? "Installed" : "Not installed"
            description: LayerStatus.installed ? LayerStatus.manifestPath
                                               : "No VK_LAYER_presentmon_linux manifest in the Vulkan loader's layer folders. Install it with: cmake --install <build> --prefix ~/.local"
            icon.name: LayerStatus.installed ? "dialog-ok-apply" : "dialog-warning"
        }
        FormCard.FormDelegateSeparator { visible: LayerStatus.installed }
        FormCard.FormTextDelegate {
            visible: LayerStatus.installed
            text: "Layer library"
            description: LayerStatus.libraryPath + (LayerStatus.libraryFound ? "" : " (missing!)")
            icon.name: LayerStatus.libraryFound ? "" : "dialog-error"
        }
        FormCard.FormDelegateSeparator {}
        FormCard.FormTextDelegate {
            text: "presentmon command-line tool"
            description: LayerStatus.cliPath !== "" ? LayerStatus.cliPath : "Not found next to this app or in PATH (recording needs it)"
        }
        FormCard.FormDelegateSeparator {}
        FormCard.FormButtonDelegate {
            text: "Copy Steam launch options"
            description: LayerStatus.steamLaunchOptions
            icon.name: "edit-copy"
            onClicked: {
                LayerStatus.copyToClipboard(LayerStatus.steamLaunchOptions)
                applicationWindow().showPassiveNotification("Copied")
            }
        }
        FormCard.FormDelegateSeparator {}
        FormCard.FormButtonDelegate {
            text: "Copy launch options for native games"
            description: LayerStatus.nativeLaunchOptions
            icon.name: "edit-copy"
            onClicked: {
                LayerStatus.copyToClipboard(LayerStatus.nativeLaunchOptions)
                applicationWindow().showPassiveNotification("Copied")
            }
        }
        FormCard.FormSectionText {
            text: "The layer only loads in games started with PRESENTMON_LAYER=1. Display times need Wayland: Proton games also need PROTON_ENABLE_WAYLAND=1. PRESENTMON_OVERLAY=0 starts with the overlay hidden; PRESENTMON_OVERLAY=off does not load it."
        }
    }

    FormCard.FormHeader { title: "Live view" }
    FormCard.FormCard {
        FormCard.FormComboBoxDelegate {
            text: "Graph history"
            model: page.histories.map(s => s + " seconds")
            currentIndex: Math.max(0, page.histories.indexOf(page.settings.historySeconds))
            onActivated: index => page.settings.historySeconds = page.histories[index]
        }
    }

    FormCard.FormHeader { title: "About" }
    FormCard.FormCard {
        FormCard.FormTextDelegate {
            text: "PresentMon for Linux " + Qt.application.version
            description: "Frame metrics are computed with Intel PresentMon's own code (MIT). The live numbers are the in-game overlay's."
        }
    }
}
