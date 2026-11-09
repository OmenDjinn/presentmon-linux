// SPDX-License-Identifier: MIT
// Display names shared by the GUI's C++ types: present modes, colour spaces, runtimes, capture
// file names. No QML types here.
#pragma once

#include <pml/protocol.hpp>

#include <QDateTime>
#include <QString>

#include <cstdint>

namespace pml::gui {

// "FIFO", "MAILBOX", "IMMEDIATE", "FIFO_RELAXED", "FIFO_LATEST_READY", ... ("Unknown (n)" otherwise).
QString presentModeName(uint32_t vkPresentMode);

// "sRGB", "HDR10 (PQ)", "scRGB (linear)", "Display P3", ... ("Colour space n" otherwise).
QString colorSpaceName(uint32_t vkColorSpace);
bool isHdrColorSpace(uint32_t vkColorSpace);

// "Vulkan", "DXVK", "VKD3D-Proton", ... from the swapchain's engine name (as the CSV's PresentRuntime).
QString runtimeName(const pml::SwapchainInfo &info);

// One line for a swapchain: "2560×1440 · FIFO · HDR10 (PQ) · DXVK".
QString swapchainSummary(const pml::SwapchainInfo &info);

// Capture file name in Intel PresentMon's style: "pmcap-<exe>-YYMMDD-HHMMSS.csv", with characters
// that are awkward in file names replaced by '_'.
QString captureFileName(const QString &processName, const QDateTime &when);

// Parses a captureFileName(): fills processName and when, returns false for other names.
bool parseCaptureFileName(const QString &fileName, QString *processName, QDateTime *when);

} // namespace pml::gui
