// SPDX-License-Identifier: MIT
// No-op overlay, linked when the layer is built without src/layer/overlay.
#include "overlay_seam.hpp"

namespace pml::layer::overlay {

bool enabled() { return false; }
void onDeviceCreated(DeviceContext &) {}
void onDeviceDestroying(DeviceContext &) {}
bool onSwapchainCreateInfo(DeviceContext &, VkSwapchainCreateInfoKHR *) { return false; }
void onSwapchainCreated(SwapchainContext &) {}
void onSwapchainDestroying(SwapchainContext &) {}
void onPresent(PresentContext &) {}
void onRecord(const pml::FrameRecord &, const pml::SwapchainInfo &) {}

} // namespace pml::layer::overlay
