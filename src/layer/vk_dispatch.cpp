// SPDX-License-Identifier: MIT
#include "vk_dispatch.hpp"

namespace pml::layer {

void loadInstanceDispatch(InstanceDispatch *t, VkInstance instance, PFN_vkGetInstanceProcAddr gipa)
{
	*t = {};
	t->GetInstanceProcAddr = gipa;
#define X(name) t->name = reinterpret_cast<PFN_vk##name>(gipa(instance, "vk" #name));
	PML_INSTANCE_FUNCTIONS(X)
#undef X
	t->GetPhysicalDeviceFeatures2 =
		reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(instance, "vkGetPhysicalDeviceFeatures2"));
	if (!t->GetPhysicalDeviceFeatures2)
		t->GetPhysicalDeviceFeatures2 =
			reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(instance, "vkGetPhysicalDeviceFeatures2KHR"));
	t->GetPhysicalDeviceProperties2 =
		reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2"));
	if (!t->GetPhysicalDeviceProperties2)
		t->GetPhysicalDeviceProperties2 =
			reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2KHR"));
	t->GetPhysicalDeviceCalibrateableTimeDomainsKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
		gipa(instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"));
	auto surface = [&](const char *name) { return reinterpret_cast<PFN_CreateSurfaceGeneric>(gipa(instance, name)); };
	t->CreateWaylandSurfaceKHR = surface("vkCreateWaylandSurfaceKHR");
	t->CreateXcbSurfaceKHR = surface("vkCreateXcbSurfaceKHR");
	t->CreateXlibSurfaceKHR = surface("vkCreateXlibSurfaceKHR");
	t->CreateDisplayPlaneSurfaceKHR = surface("vkCreateDisplayPlaneSurfaceKHR");
	t->CreateHeadlessSurfaceEXT = surface("vkCreateHeadlessSurfaceEXT");
}

void loadDeviceDispatch(DeviceDispatch *t, VkDevice device, PFN_vkGetDeviceProcAddr gdpa)
{
	*t = {};
	t->GetDeviceProcAddr = gdpa;
#define X(name) t->name = reinterpret_cast<PFN_vk##name>(gdpa(device, "vk" #name));
	PML_DEVICE_FUNCTIONS(X)
#undef X
	if (!t->GetDeviceQueue2)  // core 1.1 name first, nothing else to try
		t->GetDeviceQueue2 = nullptr;
	t->QueueSubmit2KHR = reinterpret_cast<PFN_vkQueueSubmit2>(gdpa(device, "vkQueueSubmit2KHR"));
}

} // namespace pml::layer
