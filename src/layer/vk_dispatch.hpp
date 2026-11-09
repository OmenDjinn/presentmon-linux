// SPDX-License-Identifier: MIT
// Dispatch tables for the layer: the next layer's / driver's entry points we call. Loaded through
// the next layer's vkGetInstanceProcAddr / vkGetDeviceProcAddr. The layer-util tables of pyrofling
// (third_party/pyrofling/layer-util, MIT) lack several functions we need (present timing queue size,
// GetDeviceQueue2, surface creation hooks, ...), so these are our own; the X-macro approach keeps the
// list in one place. The overlay can load anything else it needs through getDeviceProcAddr.
#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

namespace pml::layer {

// Platform surface creation functions are hooked with an opaque create-info pointer so that this
// layer needs no window-system headers (the calling convention is identical).
using PFN_CreateSurfaceGeneric = VkResult(VKAPI_PTR *)(VkInstance, const void *, const VkAllocationCallbacks *,
                                                       VkSurfaceKHR *);

#define PML_INSTANCE_FUNCTIONS(X) \
	X(DestroyInstance) \
	X(CreateDevice) \
	X(GetDeviceProcAddr) \
	X(EnumerateDeviceExtensionProperties) \
	X(GetPhysicalDeviceQueueFamilyProperties) \
	X(GetPhysicalDeviceProperties) \
	X(GetPhysicalDeviceMemoryProperties) \
	X(GetPhysicalDeviceSurfaceCapabilities2KHR) \
	X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
	X(DestroySurfaceKHR)

struct InstanceDispatch
{
	PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
#define X(name) PFN_vk##name name = nullptr;
	PML_INSTANCE_FUNCTIONS(X)
#undef X
	PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;      // core or KHR
	PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;  // core or KHR
	PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR GetPhysicalDeviceCalibrateableTimeDomainsKHR = nullptr;
	// Next layer's surface creators (null when the window system is not available).
	PFN_CreateSurfaceGeneric CreateWaylandSurfaceKHR = nullptr;
	PFN_CreateSurfaceGeneric CreateXcbSurfaceKHR = nullptr;
	PFN_CreateSurfaceGeneric CreateXlibSurfaceKHR = nullptr;
	PFN_CreateSurfaceGeneric CreateDisplayPlaneSurfaceKHR = nullptr;
	PFN_CreateSurfaceGeneric CreateHeadlessSurfaceEXT = nullptr;
};

#define PML_DEVICE_FUNCTIONS(X) \
	X(DestroyDevice) \
	X(GetDeviceQueue) \
	X(GetDeviceQueue2) \
	X(CreateSwapchainKHR) \
	X(DestroySwapchainKHR) \
	X(GetSwapchainImagesKHR) \
	X(QueuePresentKHR) \
	X(QueueSubmit) \
	X(QueueSubmit2) \
	X(SetSwapchainPresentTimingQueueSizeEXT) \
	X(GetSwapchainTimingPropertiesEXT) \
	X(GetSwapchainTimeDomainPropertiesEXT) \
	X(GetPastPresentationTimingEXT) \
	X(GetCalibratedTimestampsKHR) \
	X(SetLatencySleepModeNV) \
	X(LatencySleepNV) \
	X(WaitForPresentKHR) \
	X(WaitForPresent2KHR)

struct DeviceDispatch
{
	PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
	PFN_vkQueueSubmit2 QueueSubmit2KHR = nullptr;  // VK_KHR_synchronization2 name (may be a different entry point)
#define X(name) PFN_vk##name name = nullptr;
	PML_DEVICE_FUNCTIONS(X)
#undef X
};

void loadInstanceDispatch(InstanceDispatch *table, VkInstance instance, PFN_vkGetInstanceProcAddr gipa);
void loadDeviceDispatch(DeviceDispatch *table, VkDevice device, PFN_vkGetDeviceProcAddr gdpa);

// The loader stores its dispatch table pointer in the first word of every dispatchable handle.
template <class Handle>
inline void *dispatchKey(Handle handle)
{
	return *reinterpret_cast<void **>(handle);
}

} // namespace pml::layer
