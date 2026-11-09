// SPDX-License-Identifier: MIT
// The seam between the layer core and the in-game overlay (src/layer/overlay/).
//
// The core calls the functions in pml::layer::overlay at fixed points. Without the overlay
// (PML_BUILD_OVERLAY=OFF) overlay_stub.cpp provides no-op definitions; the overlay component
// provides the real ones and is linked into VkLayer_presentmon as target `pml_overlay`.
//
// Rules for implementers
//   * All calls below are made by the layer, on application threads. Nothing is called from a
//     thread the layer owns (there is none in M1).
//   * Use ONLY `DeviceContext::table` / `instanceTable` / the next-layer proc addrs for Vulkan
//     calls (never the loader's exported symbols: that would re-enter this layer).
//   * Never throw. Never block on the GPU in onPresent (no vkQueueWaitIdle / vkDeviceWaitIdle).
//   * Failures must degrade to "no overlay on this swapchain", not to a crashed application.
#pragma once

#include <vulkan/vulkan.h>

#include "pml/protocol.hpp"
#include "pml/shm.hpp"
#include "vk_dispatch.hpp"

#include <vector>

namespace pml::layer {

// One per VkDevice, valid from onDeviceCreated until onDeviceDestroying returned.
struct DeviceContext
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	uint32_t apiVersion = 0;                              // VkApplicationInfo::apiVersion of the instance
	const InstanceDispatch *instanceTable = nullptr;      // calls go down to the next layer / driver
	const DeviceDispatch *deviceTable = nullptr;
	PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;  // the NEXT layer's
	PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;      // the NEXT layer's
	PFN_vkSetDeviceLoaderData setDeviceLoaderData = nullptr;  // call on every dispatchable object (VkCommandBuffer...) the overlay creates through the table
	void *internal = nullptr;                             // layer private
	void *userData = nullptr;                             // free for the overlay (set in onDeviceCreated)
};

// One per VkSwapchainKHR, valid from onSwapchainCreated until onSwapchainDestroying returned.
struct SwapchainContext
{
	DeviceContext *device = nullptr;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkSwapchainCreateInfoKHR createInfo = {};   // as passed to the driver (after onSwapchainCreateInfo); pNext cleared
	pml::SwapchainInfo info = {};               // what was published in the segment (serial, format, stages, ...)
	pml::Segment *segment = nullptr;            // never null in these callbacks; overlay control lives in segment->header().overlay
	void *userData = nullptr;                   // free for the overlay
};

// Passed to onPresent for each swapchain of a vkQueuePresentKHR call, in call order, before the
// layer calls down.
struct PresentContext
{
	SwapchainContext *swapchain = nullptr;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;
	uint32_t imageIndex = 0;
	// Semaphores the present will wait on. The overlay may replace the whole list, typically with a
	// single semaphore its own queue submission signals after waiting on the previous contents (a
	// binary semaphore can be waited on once, so the old ones must not stay in the list). With
	// several swapchains in one present the list passed to the next swapchain is the replaced one.
	std::vector<VkSemaphore> *waits = nullptr;
	// Out: set when the overlay drew into this image, and the CPU time it took (ns).
	bool drawn = false;
	uint64_t cpuNs = 0;
};

namespace overlay {

// False when PRESENTMON_OVERLAY=off: then no other function is called and the swapchain is not touched.
bool enabled();

void onDeviceCreated(DeviceContext &device);
void onDeviceDestroying(DeviceContext &device);

// Before vkCreateSwapchainKHR: adjust a local copy of the create info (e.g. add COLOR_ATTACHMENT usage).
// Return false to leave this swapchain without an overlay (the usage must then not be changed).
bool onSwapchainCreateInfo(DeviceContext &device, VkSwapchainCreateInfoKHR *info);
// After successful creation. Set pml::kSwapchainOverlay in swapchain.info.timingFlags if resources exist;
// call pml::setOverlayAvailable(*segment, true) once the overlay can draw.
void onSwapchainCreated(SwapchainContext &swapchain);
// Before vkDestroySwapchainKHR (the GPU may still use the images: the application is responsible for idling).
void onSwapchainDestroying(SwapchainContext &swapchain);

void onPresent(PresentContext &present);

// Called, in present order per swapchain, for every finished record, with the swapchain's broker
// lock held: keep it short, never call back into Vulkan entry points of the layer. May run on the
// application's present thread or on a thread calling vkGetPastPresentationTimingEXT.
void onRecord(const pml::FrameRecord &record, const pml::SwapchainInfo &swapchain);

} // namespace overlay

// Layer services for the overlay.
pml::Segment *processSegment();                                   // null until the first swapchain exists
uint32_t queueFamilyIndex(const DeviceContext &device, VkQueue queue);

} // namespace pml::layer
