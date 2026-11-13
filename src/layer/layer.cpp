// SPDX-License-Identifier: MIT
// VkLayer_presentmon: entry points, object tracking and the glue between the Vulkan API and the
// pure-logic TimingBroker. See docs/ARCHITECTURE.md, section 3.1.
//
// Parts of the instance/device creation plumbing follow third_party/pyrofling/latency-measurement-layer
// (MIT, Copyright (c) 2023 Hans-Kristian Arntzen).
//
// Non-invasiveness rules this file lives by:
//   * nothing is hidden from the application, no extra queue work, no waits;
//   * the application's pNext chains are never written (PresentChain / DeviceFeatureChain copy);
//   * every internal failure degrades to a pass-through of the original call.
#include "frame_cap.hpp"
#include "frame_limiter.hpp"
#include "gpu_timer.hpp"
#include "log.hpp"
#include "overlay_seam.hpp"
#include "present_chain.hpp"
#include "process_segment.hpp"
#include "timing_broker.hpp"
#include "vk_dispatch.hpp"

#include "pml/clock.hpp"
#include "pml/process_info.hpp"
#include "pml/shm.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <mutex>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#undef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT extern "C" __attribute__((visibility("default")))

namespace pml::layer {
namespace {

constexpr const char *kLayerVersion = "0.1.0";
// How long vkDestroySwapchainKHR waits for the last feedback reports. The broker's lock is free
// meanwhile (other threads are not held up). 8 ms was tried and loses the display times of the last
// frames of a FIFO swapchain (their reports come two or three refreshes after the present), which
// the CSV then drops, so the wait stays at 30 ms; it only costs time when reports are still pending.
constexpr uint32_t kTeardownWaitMs = 30;

// ---------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------

struct DeviceState;

struct InstanceState
{
	VkInstance instance = VK_NULL_HANDLE;
	InstanceDispatch dt;
	std::string engineName;
	std::string applicationName;
	uint32_t engineVersion = 0;
	uint32_t applicationVersion = 0;
	uint32_t apiVersion = VK_API_VERSION_1_0;
	bool hasSurfaceCaps2 = false;
};

class VkTimingDriver;

struct SwapchainState
{
	DeviceState *device = nullptr;
	VkSwapchainKHR handle = VK_NULL_HANDLE;
	uint64_t serial = 0;
	pml::SwapchainInfo info = {};
	uint32_t segmentSlot = UINT32_MAX;    // in the segment of process ownerPid
	int ownerPid = 0;                     // getpid() at creation: a fork()ed child does not own this swapchain's slot
	VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
	bool appTimingBit = false;
	bool layerTimingBit = false;
	bool managed = false;  // a broker handles this swapchain's timing
	SwapchainContext overlayCtx;
	std::atomic<bool> overlayActive{false};
	bool counted = false;  // included in DeviceState::managedSwapchains
	std::unique_ptr<VkTimingDriver> driver;
	std::unique_ptr<TimingBroker> broker;
	uint64_t refreshDurationAskedNs = 0;   // when info.refreshDurationNs was last asked for (updateRefreshDuration)
	std::atomic<uint64_t> cpuFrames{0};  // presents counted for the overlay's CPU-only records

	// Frame cap (frame_limiter.hpp). Presents to one swapchain are externally synchronized, so the limiter
	// needs no lock; it exists once a cap was in force on this swapchain.
	std::unique_ptr<FrameLimiter> limiter;
	// VK_NV_low_latency2 (Reflex) state of this swapchain: the app's own sleep mode, the cap interval the layer
	// merged into it, and when the app last called vkLatencySleepNV.
	struct LowLatency
	{
		std::mutex mutex;                    // the app's vkSetLatencySleepModeNV vs the layer's re-apply
		bool appSet = false;                 // the app called vkSetLatencySleepModeNV at least once
		bool appNull = true;                 // ... with null (mode off)
		VkLatencySleepModeInfoNV app = {VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
		uint32_t capUs = 0;                  // the layer's cap interval the driver holds (0 = none)
		bool layerApplied = false;           // the driver holds a mode the layer sent (to be restored)
		uint64_t retryAfterNs = 0;           // the layer's last vkSetLatencySleepModeNV failed: next try
		bool failLogged = false;
		std::atomic<uint64_t> lastSleepNs{0};   // the app's last successful vkLatencySleepNV
		std::atomic<uint64_t> sleeps{0};
	} lowLatency;
	// Frame cap held on the app's frame-pacing thread (late placement). DXVK's and vkd3d-proton's frame thread waits
	// for each present (vkWaitForPresentKHR/2KHR: V-Sync on) or polls the present timing (DXVK with
	// VK_EXT_present_timing: vkGetPastPresentationTimingEXT once per frame, every mode) before it releases the game
	// thread. Holding that call holds the game before its next frame, as their own limiters do; a wait on their
	// submit thread (in vkQueuePresentKHR) costs a frame of latency instead (measured: +9 ms at 100 fps).
	struct FrameHold
	{
		enum Kind : uint32_t
		{
			None = 0,
			PresentWait = 1,   // the app blocks in vkWaitForPresent*KHR for every present
			TimingPoll = 2,    // another thread than the presenting one polls the present timing every frame
		};
		std::mutex mutex;                          // the holding thread (one at a time)
		std::unique_ptr<FrameLimiter> limiter;     // under mutex
		uint64_t lastHeldId = 0;                   // under mutex: each present id is held once, in order
		uint64_t lastHeldPresent = 0;              // under mutex: `presents` at the last poll hold
		std::atomic<uint32_t> kind{None};          // set by the present path
		std::atomic<uint64_t> intervalNs{0};       // ... with the cap's interval (0 = not holding)
		std::atomic<uint64_t> lastPlanNs{0};       // ... when it last planned this swapchain (older: stale, no hold)
		// Present path only (presents of one swapchain are externally synchronized): is the hold pacing the game?
		uint64_t windowStartNs = 0;                // a window of 32 presents (0: none yet)
		uint32_t windowFrames = 0;
		bool warmedUp = false;                     // the first window after the hold starts is not judged
		uint64_t blockedUntilNs = 0;               // it was not working: the present-side wait caps until then
		bool ineffectiveLogged = false;
		std::atomic<uint32_t> presentThread{0};    // the thread that presents this swapchain
		std::atomic<uint64_t> presents{0};         // presents while a cap is active
		std::atomic<uint64_t> lastBlockingNs{0};   // the app's last blocking present wait (timeout >= 100 ms)
		std::atomic<uint32_t> blockingStreak{0};   // ... and how many came in a row (each within 250 ms of the last)
		std::atomic<uint64_t> lastPollNs{0};       // the app's last present-timing poll from another thread
		std::atomic<uint32_t> pollStreak{0};       // ... and how many came in a row
		std::atomic<uint64_t> pendingWaitNs{0};    // held since the last present (goes into the next record)
		std::atomic<uint64_t> meanWaitNs{0};       // limiter stats for the status
		std::atomic<uint32_t> releaseP99Ns{0};
		std::atomic<uint64_t> sleepFailures{0};
	} hold;
	uint32_t capLoggedKind = 0;
	uint32_t capLoggedMethod = 0;   // last frame cap method / cap logged for this swapchain (present thread)
	uint32_t capLoggedMilli = 0;
	bool capSleepFailLogged = false;
};

struct DeviceState
{
	DeviceContext ctx;
	std::shared_ptr<InstanceState> instance;
	DeviceDispatch dt;
	bool timingEnabled = false;       // extensions + features are on at the device
	bool appEnabledTiming = false;
	bool appEnabledId2 = false;
	bool appEnabledId1 = false;
	std::mutex mutex;
	std::unordered_map<VkQueue, uint32_t> queueFamilies;
	std::unordered_map<uint64_t, std::shared_ptr<SwapchainState>> swapchains;
	std::atomic<uint32_t> managedSwapchains{0};
	std::unique_ptr<GpuTimer> gpu;    // GPU timestamps around submissions (null: unavailable or PRESENTMON_GPU=0)
};

struct Globals
{
	std::mutex mutex;
	std::unordered_map<void *, std::shared_ptr<InstanceState>> instances;
	std::unordered_map<void *, std::shared_ptr<DeviceState>> devices;
	std::unordered_map<uint64_t, SurfaceKind> surfaces;
	std::atomic<uint64_t> swapchainSerial{0};
};

// Leaked on purpose: nothing here may run destructors while the process is shutting down.
Globals &globals()
{
	static Globals *g = new Globals;
	return *g;
}

std::shared_ptr<InstanceState> instanceFor(void *key)
{
	Globals &g = globals();
	std::lock_guard<std::mutex> lock(g.mutex);
	auto it = g.instances.find(key);
	return it == g.instances.end() ? nullptr : it->second;
}

std::shared_ptr<DeviceState> deviceFor(void *key)
{
	Globals &g = globals();
	std::lock_guard<std::mutex> lock(g.mutex);
	auto it = g.devices.find(key);
	return it == g.devices.end() ? nullptr : it->second;
}

SurfaceKind surfaceKindOf(VkSurfaceKHR surface)
{
	Globals &g = globals();
	std::lock_guard<std::mutex> lock(g.mutex);
	auto it = g.surfaces.find(uint64_t(surface));
	return it == g.surfaces.end() ? SurfaceKind::Unknown : it->second;
}

// ---------------------------------------------------------------------------------------
// Process segment
// ---------------------------------------------------------------------------------------

void segmentAtExit()
{
	Process &p = process();
	// A forked child inherits the atexit list but must not unlink the parent's segment.
	if (!p.segment || ::getpid() != p.ownerPid)
		return;
	pml::setState(*p.segment, pml::SegmentState::Exited);
	p.segment->unlink();
}

pml::OverlayControl initialOverlayControl()
{
	pml::OverlayControl c = {};
	c.generation = 1;
	c.visible = 1;
	const char *mode = std::getenv("PRESENTMON_OVERLAY");
	if (mode && (!std::strcmp(mode, "0") || !std::strcmp(mode, "off") || !std::strcmp(mode, "OFF")))
		c.visible = 0;
	if (const char *position = std::getenv("PRESENTMON_OVERLAY_POSITION"))
	{
		if (!std::strcmp(position, "top-right")) c.position = uint32_t(pml::OverlayPosition::TopRight);
		else if (!std::strcmp(position, "bottom-left")) c.position = uint32_t(pml::OverlayPosition::BottomLeft);
		else if (!std::strcmp(position, "bottom-right")) c.position = uint32_t(pml::OverlayPosition::BottomRight);
		else c.position = uint32_t(pml::OverlayPosition::TopLeft);
	}
	if (const char *scale = std::getenv("PRESENTMON_OVERLAY_SCALE"))
	{
		double value = std::atof(scale);
		if (value > 0.0 && value < 64.0)
			c.scalePermille = uint32_t(value * 1000.0 + 0.5);
	}
	return c;
}

// Child side of fork(): the globals' mutex may be held by a thread that no longer exists.
void resetGlobalsAfterFork()
{
	new (&globals().mutex) std::mutex;
}

// Creates the segment on first use. Returns null if it cannot be created (the layer then runs
// without records but keeps serving the application's present-timing calls).
pml::Segment *ensureSegment(const InstanceState &instance)
{
	installForkHandler(&resetGlobalsAfterFork);
	Process &p = process();
	std::lock_guard<std::mutex> lock(p.mutex);
	if (p.segment)
		return p.segment.get();
	if (p.attempted)
		return nullptr;
	p.attempted = true;

	pml::ProcessIdentity identity = pml::currentProcessIdentity();
	identity.engineName = instance.engineName;
	identity.applicationName = instance.applicationName;
	identity.engineVersion = instance.engineVersion;
	identity.applicationVersion = instance.applicationVersion;
	identity.apiVersion = instance.apiVersion;
	identity.layerVersion = kLayerVersion;

	std::string error;
	const pml::CapControl initialCap = CapRuntime::controlFromEnvironment();
	auto segment = pml::Segment::create(pml::segmentNameForPid(::getpid()), identity, pml::kDefaultRingCapacity,
	                                    initialOverlayControl(), &error, &initialCap);
	if (!segment)
	{
		PML_LOG(LogLevel::Error, "cannot create the shared-memory segment: %s", error.c_str());
		return nullptr;
	}
	p.ownerPid = ::getpid();
	p.ring = std::make_unique<pml::RingWriter>(*segment);
	p.segment = std::move(segment);
	std::atexit(segmentAtExit);
	PML_LOG(LogLevel::Info, "segment %s created", p.segment->name().c_str());
	return p.segment.get();
}

// ---------------------------------------------------------------------------------------
// Driver adapter for the broker
// ---------------------------------------------------------------------------------------

bool chooseTimeDomain(DeviceState &device, VkSwapchainKHR swapchain, uint32_t *domain, uint64_t *domainId,
                      uint64_t *counter);

class VkTimingDriver final : public TimingDriver
{
public:
	VkTimingDriver(DeviceState *device, VkSwapchainKHR swapchain) : device_(device), swapchain_(swapchain) {}

	VkResult getPast(VkPastPresentationTimingFlagsEXT flags, VkPastPresentationTimingPropertiesEXT *props) override
	{
		VkPastPresentationTimingInfoEXT info = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT};
		info.flags = flags;
		info.swapchain = swapchain_;
		return device_->dt.GetPastPresentationTimingEXT(device_->ctx.device, &info, props);
	}

	bool calibrate(VkTimeDomainKHR domain, uint64_t domainId, VkPresentStageFlagsEXT stage, uint64_t *monotonicNs,
	               uint64_t *domainNs) override
	{
		if (!device_->dt.GetCalibratedTimestampsKHR)
			return false;
		VkSwapchainCalibratedTimestampInfoEXT local = {VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT};
		local.swapchain = swapchain_;
		local.presentStage = stage;
		local.timeDomainId = domainId;
		VkCalibratedTimestampInfoKHR infos[2] = {};
		infos[0] = {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR};
		infos[1] = {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, domain};
		if (domain == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT || domain == VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT)
			infos[1].pNext = &local;
		uint64_t stamps[2] = {};
		uint64_t deviation = 0;
		if (device_->dt.GetCalibratedTimestampsKHR(device_->ctx.device, 2, infos, stamps, &deviation) != VK_SUCCESS)
			return false;
		*monotonicNs = stamps[0];
		*domainNs = stamps[1];
		return true;
	}

	uint64_t nowNs() override { return pml::monotonicNs(); }

	bool refreshTimeDomain(uint64_t *domainId, uint64_t *counter) override
	{
		uint32_t domain = 0;
		return chooseTimeDomain(*device_, swapchain_, &domain, domainId, counter);
	}

private:
	DeviceState *device_;
	VkSwapchainKHR swapchain_;
};

// ---------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------

VkLayerInstanceCreateInfo *findLayerInfo(const VkInstanceCreateInfo *ci, VkLayerFunction function)
{
	auto *info = static_cast<const VkLayerInstanceCreateInfo *>(ci->pNext);
	while (info && !(info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && info->function == function))
		info = static_cast<const VkLayerInstanceCreateInfo *>(info->pNext);
	return const_cast<VkLayerInstanceCreateInfo *>(info);
}

VkLayerDeviceCreateInfo *findLayerInfo(const VkDeviceCreateInfo *ci, VkLayerFunction function)
{
	auto *info = static_cast<const VkLayerDeviceCreateInfo *>(ci->pNext);
	while (info && !(info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && info->function == function))
		info = static_cast<const VkLayerDeviceCreateInfo *>(info->pNext);
	return const_cast<VkLayerDeviceCreateInfo *>(info);
}

bool hasName(const char *const *names, uint32_t count, const char *name)
{
	for (uint32_t i = 0; i < count; i++)
		if (!std::strcmp(names[i], name))
			return true;
	return false;
}

bool hasName(const std::vector<VkExtensionProperties> &props, const char *name)
{
	for (const auto &p : props)
		if (!std::strcmp(p.extensionName, name))
			return true;
	return false;
}

bool overlayLoaded()
{
	static const bool enabled = overlay::enabled();
	return enabled;
}

// ---------------------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL hookCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                  const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
	VkLayerInstanceCreateInfo *link = findLayerInfo(pCreateInfo, VK_LAYER_LINK_INFO);
	if (!link || !link->u.pLayerInfo)
		return VK_ERROR_INITIALIZATION_FAILED;
	// Before the layer's globals are first locked: a fork() at any later point resets them in the child.
	installForkHandler(&resetGlobalsAfterFork);
	PFN_vkGetInstanceProcAddr nextGipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	auto nextCreate = reinterpret_cast<PFN_vkCreateInstance>(nextGipa(VK_NULL_HANDLE, "vkCreateInstance"));
	if (!nextCreate)
		return VK_ERROR_INITIALIZATION_FAILED;

	// Extra instance extensions: VK_KHR_get_surface_capabilities2 (surface timing/present-id caps)
	// and VK_KHR_get_physical_device_properties2, when available and the app uses surfaces at all.
	std::vector<const char *> extensions(pCreateInfo->ppEnabledExtensionNames,
	                                     pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
	VkInstanceCreateInfo modified = *pCreateInfo;
	bool addedSurfaceCaps2 = false;
	auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
		nextGipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
	const bool usesSurfaces = hasName(extensions.data(), uint32_t(extensions.size()), VK_KHR_SURFACE_EXTENSION_NAME);
	if (enumerate && usesSurfaces)
	{
		uint32_t count = 0;
		std::vector<VkExtensionProperties> available;
		if (enumerate(nullptr, &count, nullptr) == VK_SUCCESS && count)
		{
			available.resize(count);
			if (enumerate(nullptr, &count, available.data()) < 0)
				available.clear();
			available.resize(std::min<size_t>(available.size(), count));
		}
		for (const char *wanted : {VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
		                           VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME})
		{
			if (!hasName(extensions.data(), uint32_t(extensions.size()), wanted) && hasName(available, wanted))
			{
				extensions.push_back(wanted);
				if (!std::strcmp(wanted, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME))
					addedSurfaceCaps2 = true;
			}
		}
		modified.enabledExtensionCount = uint32_t(extensions.size());
		modified.ppEnabledExtensionNames = extensions.data();
	}

	link->u.pLayerInfo = link->u.pLayerInfo->pNext;
	VkResult result = nextCreate(&modified, pAllocator, pInstance);
	if (result != VK_SUCCESS)
	{
		return result;
	}

	auto state = std::make_shared<InstanceState>();
	state->instance = *pInstance;
	loadInstanceDispatch(&state->dt, *pInstance, nextGipa);
	state->hasSurfaceCaps2 = addedSurfaceCaps2 ||
	                         hasName(pCreateInfo->ppEnabledExtensionNames, pCreateInfo->enabledExtensionCount,
	                                 VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
	if (const VkApplicationInfo *app = pCreateInfo->pApplicationInfo)
	{
		state->apiVersion = app->apiVersion;
		state->engineVersion = app->engineVersion;
		state->applicationVersion = app->applicationVersion;
		if (app->pEngineName)
			state->engineName = app->pEngineName;
		if (app->pApplicationName)
			state->applicationName = app->pApplicationName;
	}

	{
		Globals &g = globals();
		std::lock_guard<std::mutex> lock(g.mutex);
		g.instances[dispatchKey(*pInstance)] = state;
	}
	PML_LOG(LogLevel::Debug, "instance created (engine '%s', app '%s')", state->engineName.c_str(),
	        state->applicationName.c_str());
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hookDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
	void *key = dispatchKey(instance);
	std::shared_ptr<InstanceState> state;
	{
		Globals &g = globals();
		std::lock_guard<std::mutex> lock(g.mutex);
		auto it = g.instances.find(key);
		if (it != g.instances.end())
		{
			state = it->second;
			g.instances.erase(it);
		}
	}
	if (state && state->dt.DestroyInstance)
		state->dt.DestroyInstance(instance, pAllocator);
}

// ---- Surfaces -------------------------------------------------------------------------

template <PFN_CreateSurfaceGeneric InstanceDispatch::*Next, SurfaceKind Kind>
VKAPI_ATTR VkResult VKAPI_CALL hookCreateSurface(VkInstance instance, const void *pCreateInfo,
                                                 const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
	auto state = instanceFor(dispatchKey(instance));
	if (!state || !(state->dt.*Next))
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	VkResult result = (state->dt.*Next)(instance, pCreateInfo, pAllocator, pSurface);
	if (result == VK_SUCCESS && pSurface)
	{
		Globals &g = globals();
		std::lock_guard<std::mutex> lock(g.mutex);
		g.surfaces[uint64_t(*pSurface)] = Kind;
	}
	return result;
}

VKAPI_ATTR void VKAPI_CALL hookDestroySurface(VkInstance instance, VkSurfaceKHR surface,
                                              const VkAllocationCallbacks *pAllocator)
{
	auto state = instanceFor(dispatchKey(instance));
	{
		Globals &g = globals();
		std::lock_guard<std::mutex> lock(g.mutex);
		g.surfaces.erase(uint64_t(surface));
	}
	if (state && state->dt.DestroySurfaceKHR)
		state->dt.DestroySurfaceKHR(instance, surface, pAllocator);
}

// ---------------------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL hookCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
                                                const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
	auto instance = instanceFor(dispatchKey(physicalDevice));
	VkLayerDeviceCreateInfo *link = findLayerInfo(pCreateInfo, VK_LAYER_LINK_INFO);
	if (!instance || !link || !link->u.pLayerInfo)
		return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkGetInstanceProcAddr nextGipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr nextGdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	auto nextCreate = reinterpret_cast<PFN_vkCreateDevice>(nextGipa(instance->instance, "vkCreateDevice"));
	if (!nextCreate)
		return VK_ERROR_INITIALIZATION_FAILED;
	VkLayerDeviceCreateInfo *dataCallback = findLayerInfo(pCreateInfo, VK_LOADER_DATA_CALLBACK);

	std::vector<const char *> extensions(pCreateInfo->ppEnabledExtensionNames,
	                                     pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
	VkDeviceCreateInfo modified = *pCreateInfo;

	const bool usesSwapchain = hasName(extensions.data(), uint32_t(extensions.size()), VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	bool timingEnabled = false;
	const bool appHasTimingExt = hasName(extensions.data(), uint32_t(extensions.size()), VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
	const bool appHasId2Ext = hasName(extensions.data(), uint32_t(extensions.size()), VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
	const bool appHasId1Ext = hasName(extensions.data(), uint32_t(extensions.size()), VK_KHR_PRESENT_ID_EXTENSION_NAME);
	bool appTimingFeature = false, appId2Feature = false, appId1Feature = false;

	// Feature structs of ours, used when the app's chain has none.
	VkPhysicalDevicePresentTimingFeaturesEXT ourTiming = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
	VkPhysicalDevicePresentId2FeaturesKHR ourId2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
	bool wantTiming = false;

	if (const auto *f = findStruct<VkPhysicalDevicePresentTimingFeaturesEXT>(
			pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT))
		appTimingFeature = f->presentTiming != VK_FALSE;
	if (const auto *f = findStruct<VkPhysicalDevicePresentId2FeaturesKHR>(
			pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR))
		appId2Feature = f->presentId2 != VK_FALSE;
	if (const auto *f = findStruct<VkPhysicalDevicePresentIdFeaturesKHR>(
			pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR))
		appId1Feature = f->presentId != VK_FALSE;

	if (usesSwapchain && instance->hasSurfaceCaps2 && instance->dt.EnumerateDeviceExtensionProperties &&
	    instance->dt.GetPhysicalDeviceFeatures2)
	{
		uint32_t count = 0;
		std::vector<VkExtensionProperties> supported;
		if (instance->dt.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr) == VK_SUCCESS && count)
		{
			supported.resize(count);
			if (instance->dt.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, supported.data()) < 0)
				supported.clear();
			supported.resize(std::min<size_t>(supported.size(), count));
		}
		if (hasName(supported, VK_EXT_PRESENT_TIMING_EXTENSION_NAME) &&
		    hasName(supported, VK_KHR_PRESENT_ID_2_EXTENSION_NAME) &&
		    hasName(supported, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME))
		{
			VkPhysicalDevicePresentTimingFeaturesEXT haveTiming = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
			VkPhysicalDevicePresentId2FeaturesKHR haveId2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
			haveTiming.pNext = &haveId2;
			VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &haveTiming};
			instance->dt.GetPhysicalDeviceFeatures2(physicalDevice, &features2);

			wantTiming = haveTiming.presentTiming && haveId2.presentId2;
		}
	}

	// GPU timing converts its timestamps with VK_KHR_calibrated_timestamps (no feature to enable). It depends on
	// VK_KHR_get_physical_device_properties2, core from Vulkan 1.1: only added for 1.1+ instances.
	if (usesSwapchain && instance->apiVersion >= VK_API_VERSION_1_1 &&
	    !hasName(extensions.data(), uint32_t(extensions.size()), VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) &&
	    instance->dt.EnumerateDeviceExtensionProperties)
	{
		uint32_t count = 0;
		std::vector<VkExtensionProperties> supported;
		if (instance->dt.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr) == VK_SUCCESS && count)
		{
			supported.resize(count);
			if (instance->dt.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, supported.data()) < 0)
				supported.clear();
			supported.resize(std::min<size_t>(supported.size(), count));
		}
		if (hasName(supported, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME))
		{
			extensions.push_back(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
			modified.enabledExtensionCount = uint32_t(extensions.size());
			modified.ppEnabledExtensionNames = extensions.data();
		}
	}

	link->u.pLayerInfo = link->u.pLayerInfo->pNext;

	// Switch present timing on for the device. The application's chain is never written: a feature
	// structure of the app with the field FALSE is replaced by a patched copy, one that is missing
	// is complemented by ours in front. If the chain cannot be copied (unknown structure in the
	// way) the device is created as the application asked and the layer runs without timing.
	DeviceFeatureChain featureChain;
	if (wantTiming)
	{
		const DeviceFeatureChain::Request requests[2] = {
			{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
			 offsetof(VkPhysicalDevicePresentTimingFeaturesEXT, presentTiming)},
			{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR,
			 offsetof(VkPhysicalDevicePresentId2FeaturesKHR, presentId2)}};
		DeviceFeatureChain::Found found[2];
		const void *head = nullptr;
		VkStructureType unknownType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		if (featureChain.build(pCreateInfo->pNext, requests, 2, found, &head, &unknownType))
		{
			timingEnabled = true;
			for (const char *name : {VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, VK_KHR_PRESENT_ID_2_EXTENSION_NAME,
			                         VK_EXT_PRESENT_TIMING_EXTENSION_NAME})
				if (!hasName(extensions.data(), uint32_t(extensions.size()), name))
					extensions.push_back(name);
			modified.enabledExtensionCount = uint32_t(extensions.size());
			modified.ppEnabledExtensionNames = extensions.data();
			modified.pNext = head;
			if (found[0] == DeviceFeatureChain::Found::Missing)
			{
				ourTiming.presentTiming = VK_TRUE;
				ourTiming.pNext = const_cast<void *>(modified.pNext);
				modified.pNext = &ourTiming;
			}
			if (found[1] == DeviceFeatureChain::Found::Missing)
			{
				ourId2.presentId2 = VK_TRUE;
				ourId2.pNext = const_cast<void *>(modified.pNext);
				modified.pNext = &ourId2;
			}
		}
		else
		{
			PML_LOG(LogLevel::Warn, "device chain has a structure the layer cannot copy (sType %d): no present timing",
			        int(unknownType));
		}
	}

	VkResult result = nextCreate(physicalDevice, &modified, pAllocator, pDevice);
	if (result != VK_SUCCESS)
		return result;

	auto state = std::make_shared<DeviceState>();
	state->instance = instance;
	state->timingEnabled = timingEnabled;
	state->appEnabledTiming = appHasTimingExt && appTimingFeature;
	state->appEnabledId2 = appHasId2Ext && appId2Feature;
	state->appEnabledId1 = appHasId1Ext && appId1Feature;
	loadDeviceDispatch(&state->dt, *pDevice, nextGdpa);

	DeviceContext &ctx = state->ctx;
	ctx.instance = instance->instance;
	ctx.physicalDevice = physicalDevice;
	ctx.device = *pDevice;
	ctx.apiVersion = instance->apiVersion;
	ctx.instanceTable = &instance->dt;
	ctx.deviceTable = &state->dt;
	ctx.getInstanceProcAddr = nextGipa;
	ctx.getDeviceProcAddr = nextGdpa;
	ctx.setDeviceLoaderData = dataCallback ? dataCallback->u.pfnSetDeviceLoaderData : nullptr;
	ctx.internal = state.get();

	{
		Globals &g = globals();
		std::lock_guard<std::mutex> lock(g.mutex);
		g.devices[dispatchKey(*pDevice)] = state;
	}
	if (usesSwapchain)
		state->gpu = GpuTimer::create(ctx);
	if (overlayLoaded())
		overlay::onDeviceCreated(ctx);
	PML_LOG(LogLevel::Debug, "device created, present timing %s, GPU timing %s", timingEnabled ? "enabled" : "unavailable",
	        state->gpu ? "on" : "off");
	return VK_SUCCESS;
}

void teardownSwapchain(DeviceState &device, const std::shared_ptr<SwapchainState> &sw, bool waitForReports);

VKAPI_ATTR void VKAPI_CALL hookDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
	void *key = dispatchKey(device);
	std::shared_ptr<DeviceState> state;
	{
		Globals &g = globals();
		std::lock_guard<std::mutex> lock(g.mutex);
		auto it = g.devices.find(key);
		if (it != g.devices.end())
		{
			state = it->second;
			g.devices.erase(it);
		}
	}
	if (!state)
		return;

	std::vector<std::shared_ptr<SwapchainState>> leftovers;
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		for (auto &entry : state->swapchains)
			leftovers.push_back(entry.second);
		state->swapchains.clear();
	}
	for (auto &sw : leftovers)
		teardownSwapchain(*state, sw, false);
	if (overlayLoaded())
		overlay::onDeviceDestroying(state->ctx);
	state->gpu.reset();  // the application has waited for its work, ours included
	state->dt.DestroyDevice(device, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL hookGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *pQueue)
{
	auto state = deviceFor(dispatchKey(device));
	if (!state)
		return;
	state->dt.GetDeviceQueue(device, family, index, pQueue);
	if (pQueue && *pQueue)
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->queueFamilies[*pQueue] = family;
	}
}

VKAPI_ATTR void VKAPI_CALL hookGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *pQueueInfo, VkQueue *pQueue)
{
	auto state = deviceFor(dispatchKey(device));
	if (!state)
		return;
	state->dt.GetDeviceQueue2(device, pQueueInfo, pQueue);
	if (pQueue && *pQueue && pQueueInfo)
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->queueFamilies[*pQueue] = pQueueInfo->queueFamilyIndex;
	}
}

// ---------------------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------------------

std::shared_ptr<SwapchainState> findSwapchain(DeviceState &device, VkSwapchainKHR handle)
{
	std::lock_guard<std::mutex> lock(device.mutex);
	auto it = device.swapchains.find(uint64_t(handle));
	return it == device.swapchains.end() ? nullptr : it->second;
}

// Chooses the time domain the layer requests: CLOCK_MONOTONIC when the swapchain offers it
// (no conversion needed), else a present-stage-local / swapchain-local domain (calibrated).
bool chooseTimeDomain(DeviceState &device, VkSwapchainKHR swapchain, uint32_t *domain, uint64_t *domainId,
                      uint64_t *counter)
{
	if (!device.dt.GetSwapchainTimeDomainPropertiesEXT)
		return false;
	std::vector<VkTimeDomainKHR> domains;
	std::vector<uint64_t> ids;
	VkResult result = VK_INCOMPLETE;
	for (int attempt = 0; attempt < 4 && result == VK_INCOMPLETE; attempt++)
	{
		VkSwapchainTimeDomainPropertiesEXT props = {VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT};
		result = device.dt.GetSwapchainTimeDomainPropertiesEXT(device.ctx.device, swapchain, &props, nullptr);
		if (result != VK_SUCCESS)
			return false;
		domains.assign(props.timeDomainCount, VK_TIME_DOMAIN_DEVICE_KHR);
		ids.assign(props.timeDomainCount, 0);
		props.pTimeDomains = domains.data();
		props.pTimeDomainIds = ids.data();
		result = device.dt.GetSwapchainTimeDomainPropertiesEXT(device.ctx.device, swapchain, &props, counter);
		domains.resize(props.timeDomainCount);
		ids.resize(props.timeDomainCount);
	}
	if (result != VK_SUCCESS || domains.empty())
		return false;
	for (VkTimeDomainKHR wanted : {VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR, VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT,
	                               VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT})
	{
		for (size_t i = 0; i < domains.size(); i++)
		{
			if (domains[i] == wanted)
			{
				*domain = uint32_t(wanted);
				*domainId = ids[i];
				PML_LOG(LogLevel::Debug, "time domain %u (id %llu) of %zu offered", uint32_t(wanted),
				        (unsigned long long)ids[i], domains.size());
				return true;
			}
		}
	}
	return false;
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                   const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
	auto dev = deviceFor(dispatchKey(device));
	if (!dev)
		return VK_ERROR_INITIALIZATION_FAILED;
	InstanceState &inst = *dev->instance;

	VkSwapchainCreateInfoKHR info = *pCreateInfo;
	const bool appTimingBit = (pCreateInfo->flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT) != 0;
	const bool appId2Bit = (pCreateInfo->flags & VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR) != 0;

	// Overlay: it may need extra image usage; it reports whether it will draw on this swapchain.
	bool overlayWanted = false;
	if (overlayLoaded())
		overlayWanted = overlay::onSwapchainCreateInfo(dev->ctx, &info);

	// Surface capabilities decide whether we can ask for timing.
	VkPresentStageFlagsEXT supportedStages = 0;
	bool timingSupported = false, id2Supported = false;
	if (dev->timingEnabled && inst.hasSurfaceCaps2 && inst.dt.GetPhysicalDeviceSurfaceCapabilities2KHR)
	{
		VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
		surfaceInfo.surface = pCreateInfo->surface;
		VkPresentTimingSurfaceCapabilitiesEXT timingCaps = {VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT};
		VkSurfaceCapabilitiesPresentId2KHR id2Caps = {VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR};
		timingCaps.pNext = &id2Caps;
		VkSurfaceCapabilities2KHR caps = {VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, &timingCaps};
		if (inst.dt.GetPhysicalDeviceSurfaceCapabilities2KHR(dev->ctx.physicalDevice, &surfaceInfo, &caps) == VK_SUCCESS)
		{
			supportedStages = timingCaps.presentStageQueries;
			timingSupported = timingCaps.presentTimingSupported &&
			                  (supportedStages & VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT);
			id2Supported = id2Caps.presentId2Supported != VK_FALSE;
		}
	}

	// Which bits to add. The app's own bits stay as they are.
	IdMode idMode = IdMode::None;
	bool layerAddsTiming = false, layerAddsId2 = false;
	if (timingSupported)
	{
		layerAddsTiming = !appTimingBit;
		if (appId2Bit)
			idMode = IdMode::App;
		else if (dev->appEnabledId1)
			idMode = IdMode::App;  // a VkPresentIdKHR (v1) app: the driver reports v1 ids in timing reports too,
			                       // so its ids are used as they are and no PRESENT_ID_2 bit is added
		else if (id2Supported)
		{
			idMode = IdMode::Layer;
			layerAddsId2 = true;
		}
	}
	if (layerAddsTiming)
		info.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
	if (layerAddsId2)
		info.flags |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;

	VkResult result = dev->dt.CreateSwapchainKHR(device, &info, pAllocator, pSwapchain);
	const bool addedUsage = info.imageUsage != pCreateInfo->imageUsage;
	if (result != VK_SUCCESS && (layerAddsTiming || layerAddsId2 || addedUsage) &&
	    (result == VK_ERROR_INITIALIZATION_FAILED || result == VK_ERROR_FEATURE_NOT_PRESENT ||
	     result == VK_ERROR_VALIDATION_FAILED || result == VK_ERROR_UNKNOWN))
	{
		// Never let our additions (timing flags, overlay image usage) make a swapchain fail that would
		// have worked. Only errors that can come from them are retried (not OUT_OF_DATE, SURFACE_LOST,
		// ...). A failed creation has retired oldSwapchain already and passing a retired swapchain is
		// invalid, so the second attempt goes without it.
		PML_LOG(LogLevel::Warn, "swapchain creation with the layer's additions failed (%d); retrying without", int(result));
		info = *pCreateInfo;
		info.oldSwapchain = VK_NULL_HANDLE;
		layerAddsTiming = layerAddsId2 = false;
		timingSupported = false;
		idMode = IdMode::None;
		overlayWanted = false;
		result = dev->dt.CreateSwapchainKHR(device, &info, pAllocator, pSwapchain);
	}
	if (result != VK_SUCCESS)
		return result;

	auto sw = std::make_shared<SwapchainState>();
	sw->device = dev.get();
	sw->handle = *pSwapchain;
	sw->ownerPid = int(::getpid());
	sw->presentMode = pCreateInfo->presentMode;
	sw->appTimingBit = appTimingBit;
	sw->layerTimingBit = layerAddsTiming;
	sw->serial = globals().swapchainSerial.fetch_add(1) + 1;

	SwapchainInfo &si = sw->info;
	si.serial = sw->serial;
	si.handle = uint64_t(*pSwapchain);
	si.createdNs = pml::monotonicNs();
	si.width = pCreateInfo->imageExtent.width;
	si.height = pCreateInfo->imageExtent.height;
	si.format = uint32_t(pCreateInfo->imageFormat);
	si.colorSpace = uint32_t(pCreateInfo->imageColorSpace);
	si.presentMode = uint32_t(pCreateInfo->presentMode);
	si.minImageCount = pCreateInfo->minImageCount;
	si.imageUsage = pCreateInfo->imageUsage;
	si.surfaceKind = uint32_t(surfaceKindOf(pCreateInfo->surface));
	si.timingStagesSupported = timingSupported ? supportedStages : 0;
	std::strncpy(si.engineName, inst.engineName.c_str(), sizeof(si.engineName) - 1);
	{
		uint32_t imageCount = 0;
		if (dev->dt.GetSwapchainImagesKHR(device, *pSwapchain, &imageCount, nullptr) == VK_SUCCESS)
			si.imageCount = imageCount;
	}
	if (appTimingBit) si.timingFlags |= kSwapchainAppTiming;
	if (appId2Bit) si.timingFlags |= kSwapchainAppPresentId2;
	if (layerAddsId2) si.timingFlags |= kSwapchainLayerPresentId2;
	if (dev->appEnabledId1) si.timingFlags |= kSwapchainAppPresentId1;

	pml::Segment *segment = ensureSegment(inst);

	// Timing setup: only when the driver really got the timing bit (ours or the app's).
	if (timingSupported && (appTimingBit || layerAddsTiming))
	{
		uint32_t domain = 0;
		uint64_t domainId = 0, domainsCounter = 0;
		bool ok = chooseTimeDomain(*dev, *pSwapchain, &domain, &domainId, &domainsCounter);
		if (!ok)
			PML_LOG(LogLevel::Warn, "swapchain %llu: no usable time domain, timing disabled for the layer",
			        (unsigned long long)sw->serial);

		if (ok)
		{
			BrokerConfig cfg;
			cfg.swapchainSerial = sw->serial;
			cfg.swapchainHandle = uint64_t(*pSwapchain);
			cfg.defaultPresentMode = uint32_t(pCreateInfo->presentMode);
			cfg.idMode = idMode;
			cfg.ourStages = supportedStages & (VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT | kDisplayStageMask);
			cfg.ourTimeDomainId = domainId;
			cfg.timeDomainsCounter = domainsCounter;
			SwapchainState *raw = sw.get();
			DeviceState *rawDevice = dev.get();
			sw->driver = std::make_unique<VkTimingDriver>(dev.get(), *pSwapchain);
			sw->broker = std::make_unique<TimingBroker>(cfg, *sw->driver, [raw, rawDevice](const FrameRecord &finished) {
				FrameRecord record = finished;
				if (rawDevice->gpu)
					rawDevice->gpu->fill(record);
				Process &p = process();
				if (p.ring)
					p.ring->publish(record);
				if (overlayLoaded() && raw->overlayActive)
					overlay::onRecord(record, raw->info);
			});
			// Largest headroom the driver accepts (it takes far more than we ask for on this machine).
			uint32_t driverSize = sw->broker->initialDriverQueueSize();
			bool queueOk = false;
			for (uint32_t size = driverSize; size >= 8 && !queueOk; size /= 2)
			{
				queueOk = dev->dt.SetSwapchainPresentTimingQueueSizeEXT(device, *pSwapchain, size) == VK_SUCCESS;
				if (queueOk && size != driverSize)
					sw->broker->commitAppQueueSize(0, size);
			}
			if (queueOk)
			{
				sw->managed = true;
				// Without ids (IdMode::None) the broker never asks for the layer's own stages: it still writes
				// the CPU-side records, but the swapchain has no layer timing to advertise.
				if (cfg.idMode != IdMode::None)
				{
					si.timingStagesUsed = cfg.ourStages;
					si.timingFlags |= kSwapchainLayerTiming;
				}
				si.timeDomain = domain;
				si.timeDomainId = domainId;
				VkSwapchainTimingPropertiesEXT props = {VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT};
				uint64_t counter = 0;
				if (dev->dt.GetSwapchainTimingPropertiesEXT &&
				    dev->dt.GetSwapchainTimingPropertiesEXT(device, *pSwapchain, &props, &counter) == VK_SUCCESS)
					si.refreshDurationNs = props.refreshDuration;
			}
			else
			{
				PML_LOG(LogLevel::Warn, "swapchain %llu: setting the timing queue size failed, timing disabled",
				        (unsigned long long)sw->serial);
				sw->broker.reset();
				sw->driver.reset();
			}
		}
	}

	if (segment)
		sw->segmentSlot = pml::publishSwapchain(*segment, si);

	if (overlayWanted && segment)
	{
		sw->overlayCtx.device = &dev->ctx;
		sw->overlayCtx.swapchain = *pSwapchain;
		sw->overlayCtx.createInfo = info;
		sw->overlayCtx.createInfo.pNext = nullptr;
		sw->overlayCtx.info = si;
		sw->overlayCtx.segment = segment;
		sw->overlayActive = true;
		overlay::onSwapchainCreated(sw->overlayCtx);
		// The overlay may have set kSwapchainOverlay / reserved fields in its copy.
		si = sw->overlayCtx.info;
		if (sw->segmentSlot != UINT32_MAX)
			pml::updateSwapchain(*segment, sw->segmentSlot, si);
	}

	// A retired swapchain's remaining reports can be collected now.
	if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE)
	{
		if (auto old = findSwapchain(*dev, pCreateInfo->oldSwapchain))
			if (old->broker)
				old->broker->poll();
	}

	if (sw->managed || sw->overlayActive)
	{
		sw->counted = true;
		dev->managedSwapchains.fetch_add(1);
	}
	{
		std::lock_guard<std::mutex> lock(dev->mutex);
		dev->swapchains[uint64_t(*pSwapchain)] = sw;
	}
	PML_LOG(LogLevel::Info, "swapchain %llu: %ux%u mode %u surface %u timing %s ids %s%s usage 0x%x (app 0x%x)",
	        (unsigned long long)sw->serial, si.width, si.height, si.presentMode, si.surfaceKind,
	        sw->managed ? "on" : "off", idMode == IdMode::Layer ? "layer" : idMode == IdMode::App ? "app" : "none",
	        sw->overlayActive ? " overlay" : "", unsigned(info.imageUsage), unsigned(pCreateInfo->imageUsage));
	return VK_SUCCESS;
}

void teardownSwapchain(DeviceState &device, const std::shared_ptr<SwapchainState> &sw, bool waitForReports)
{
	if (sw->overlayActive && overlayLoaded())
		overlay::onSwapchainDestroying(sw->overlayCtx);
	if (sw->broker)
		sw->broker->flush(waitForReports ? kTeardownWaitMs : 0);
	if (device.gpu)
		device.gpu->forgetSwapchain(sw->serial);  // the next swapchain gets the GPU numbers at once
	sw->overlayActive = false;
	if (sw->counted)
	{
		sw->counted = false;
		device.managedSwapchains.fetch_sub(1);
	}

	// A fork()ed child destroying its parent's swapchain must not write the parent's slot index into its own
	// segment (that slot may hold one of the child's swapchains).
	Process &p = process();
	if (p.segment && sw->segmentSlot != UINT32_MAX && sw->ownerPid == int(::getpid()))
	{
		sw->info.destroyedNs = pml::monotonicNs();
		pml::updateSwapchain(*p.segment, sw->segmentSlot, sw->info);
	}
}

VKAPI_ATTR void VKAPI_CALL hookDestroySwapchain(VkDevice device, VkSwapchainKHR swapchain,
                                                const VkAllocationCallbacks *pAllocator)
{
	auto dev = deviceFor(dispatchKey(device));
	if (!dev)
		return;
	std::shared_ptr<SwapchainState> sw;
	{
		std::lock_guard<std::mutex> lock(dev->mutex);
		auto it = dev->swapchains.find(uint64_t(swapchain));
		if (it != dev->swapchains.end())
		{
			sw = it->second;
			dev->swapchains.erase(it);
		}
	}
	if (sw)
		teardownSwapchain(*dev, sw, true);  // before the driver object goes away
	dev->dt.DestroySwapchainKHR(device, swapchain, pAllocator);
}

// ---------------------------------------------------------------------------------------
// vkQueuePresentKHR
// ---------------------------------------------------------------------------------------

struct PresentScratch
{
	std::vector<std::shared_ptr<SwapchainState>> states;
	std::vector<PresentPlan> plans;
	std::vector<PresentParams> params;
	std::vector<VkPresentTimingInfoEXT> timings;
	std::vector<uint64_t> ids;
	std::vector<VkResult> results;
	std::vector<VkSemaphore> waits;
	std::vector<char> managed;
	std::vector<char> open;     // registered with the broker, endPresent not called yet
	std::vector<char> drawn;
	std::vector<uint64_t> overlayCpu;
};

PresentScratch &presentScratch()
{
	thread_local PresentScratch s;
	return s;
}

uint32_t currentThread()
{
	return pml::currentThreadId();
}

// What presentImpl tells hookQueuePresent, also when it throws.
struct PresentOutcome
{
	bool calledDown = false;
	VkResult downResult = VK_SUCCESS;
	VkResult returned = VK_SUCCESS;
	bool waitsReplaced = false;  // the overlay replaced the wait semaphores (the application's were consumed)
	bool syncConsumed = false;   // consumePresentSync ran: the waits and fences are used up, nothing may go down
};

// Ends every present that was registered with a broker and not finished, also when presentImpl
// unwinds: an entry left "submitting" would block all later records of its swapchain.
class OpenPresents
{
public:
	OpenPresents(PresentScratch &s, const PresentOutcome &outcome) : s_(s), outcome_(outcome) {}
	OpenPresents(const OpenPresents &) = delete;
	OpenPresents &operator=(const OpenPresents &) = delete;
	~OpenPresents()
	{
		try
		{
			const uint64_t now = pml::monotonicNs();
			const VkResult r = outcome_.calledDown ? outcome_.downResult : VK_ERROR_UNKNOWN;
			for (size_t i = 0; i < s_.open.size(); i++)
				if (s_.open[i])
				{
					s_.open[i] = 0;
					s_.states[i]->broker->endPresent(s_.plans[i].token, now, r);
				}
		}
		catch (...)
		{
		}
	}

private:
	PresentScratch &s_;
	const PresentOutcome &outcome_;
};

// A present that is rejected without being sent (the emulated QUEUE_FULL) still executes its
// semaphore waits and signals its present fences, like the driver's rejection would: an empty
// submission on the same queue does both. *consumed becomes true once the application's waits and
// fences have been handed to the driver (nothing allocates after that), so an exception before it
// leaves them untouched for hookQueuePresent's fallback.
bool consumePresentSync(DeviceState &dev, VkQueue queue, const VkPresentInfoKHR &info, bool *consumed)
{
	*consumed = false;
	const auto *fenceInfo = findStruct<VkSwapchainPresentFenceInfoKHR>(info.pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR);
	const uint32_t fenceCount = fenceInfo && fenceInfo->pFences ? fenceInfo->swapchainCount : 0;
	if (info.waitSemaphoreCount == 0 && fenceCount == 0)
	{
		*consumed = true;   // nothing to consume: the rejection stands as it is
		return true;
	}
	if (!dev.dt.QueueSubmit)
		return false;
	std::vector<VkPipelineStageFlags> stages(info.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.waitSemaphoreCount = info.waitSemaphoreCount;
	si.pWaitSemaphores = info.pWaitSemaphores;
	si.pWaitDstStageMask = stages.data();
	bool ok = dev.dt.QueueSubmit(queue, 1, &si, fenceCount ? fenceInfo->pFences[0] : VK_NULL_HANDLE) == VK_SUCCESS;
	*consumed = ok;
	for (uint32_t k = 1; k < fenceCount; k++)
	{
		if (fenceInfo->pFences[k] == VK_NULL_HANDLE)
			continue;
		VkSubmitInfo empty = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		ok = dev.dt.QueueSubmit(queue, 1, &empty, fenceInfo->pFences[k]) == VK_SUCCESS && ok;
	}
	return ok;
}

// ---------------------------------------------------------------------------------------
// Frame cap (frame_limiter.hpp, frame_cap.hpp)
// ---------------------------------------------------------------------------------------

// The app last called vkLatencySleepNV this recently: it runs Reflex (NV_low_latency2) on that swapchain, so the cap
// goes through its interval and the present-side wait stays off (two sleepers fight; under vkd3d-proton with
// Reflex a present-side wait does not even throttle the game thread).
constexpr int64_t kReflexRecentNs = 500'000'000;
// The driver refused the layer's sleep mode: the present-side wait covers, the layer tries again this much later.
constexpr uint64_t kReflexRetryNs = 1'000'000'000;
// A present wait with at least this timeout blocks (a frame thread pacing on it); shorter ones are polls.
constexpr uint64_t kBlockingPresentWaitNs = 100'000'000;
// The hold engages once the app paces on its present waits: this many blocking waits in a row, the last one recent.
// (DXVK with present-timing feedback waits for a couple of presents at start-up only.)
constexpr uint32_t kHoldMinStreak = 8;
// The poll hold sleeps inside the app's call, under whatever lock the app holds around it (DXVK: its timing mutex,
// which a FIFO present on its submit thread also takes): only for caps of 20 fps and more.
constexpr uint64_t kMaxPollHoldNs = 50'000'000;

// Sends the app's own sleep mode with the layer's cap interval merged in, or restores the app's mode once the layer
// has nothing to add. Caller holds sw.lowLatency.mutex. layerApplied changes only when the driver took the mode.
// (No external synchronization is required on the swapchain for the NV_low_latency2 commands, vk.xml: the layer may
// call this on the present thread while the app sleeps on another.)
VkResult applyLatencyModeLocked(DeviceState &dev, SwapchainState &sw)
{
	auto &ll = sw.lowLatency;
	if (!dev.dt.SetLatencySleepModeNV)
		return VK_SUCCESS;
	if (ll.capUs == 0)
	{
		if (!ll.layerApplied)
			return VK_SUCCESS;
		const VkResult r =
		    dev.dt.SetLatencySleepModeNV(dev.ctx.device, sw.handle, ll.appSet && !ll.appNull ? &ll.app : nullptr);
		if (r == VK_SUCCESS)
			ll.layerApplied = false;
		return r;
	}
	VkLatencySleepModeInfoNV mode = {VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
	if (ll.appSet && !ll.appNull)
		mode = ll.app;
	mode.pNext = nullptr;
	mode.minimumIntervalUs = mergedIntervalUs(mode.minimumIntervalUs, ll.capUs);
	const VkResult r = dev.dt.SetLatencySleepModeNV(dev.ctx.device, sw.handle, &mode);
	if (r == VK_SUCCESS)
		ll.layerApplied = true;
	return r;
}

// The driver's refresh cycle for the swapchain's surface (vkGetSwapchainTimingPropertiesEXT): the one thing that
// tells which output the game is on (pml::pickOutput). The VRR readout and the auto cap need that output's VRR
// state, not any output's. NVIDIA (620.70) reports 0 at swapchain creation: the cycle is known only once the
// surface is on a screen, and it changes when the window moves to another one. So it is asked again every 250 ms.
// The timingPropertiesCounter of the past-timing poll is no trigger: it stays 0 on MAILBOX and IMMEDIATE
// swapchains although the properties did change (measured 2026-11-13; on FIFO it counts).
// Called inside the app's vkQueuePresentKHR, which synchronizes the swapchain.
constexpr uint64_t kRefreshDurationEveryNs = 250'000'000ull;

void updateRefreshDuration(DeviceState &dev, SwapchainState &sw, uint64_t nowNs)
{
	if (!sw.managed || !dev.dt.GetSwapchainTimingPropertiesEXT || nowNs - sw.refreshDurationAskedNs < kRefreshDurationEveryNs)
		return;
	sw.refreshDurationAskedNs = nowNs;
	VkSwapchainTimingPropertiesEXT props = {VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT};
	uint64_t counter = 0;
	if (dev.dt.GetSwapchainTimingPropertiesEXT(dev.ctx.device, sw.handle, &props, &counter) != VK_SUCCESS)
		return;   // VK_NOT_READY: not on a screen yet
	if (props.refreshDuration == sw.info.refreshDurationNs)
		return;
	sw.info.refreshDurationNs = props.refreshDuration;
	sw.overlayCtx.info.refreshDurationNs = props.refreshDuration;   // the overlay's VRR readout asks with it
	// A fork()ed child must not write its parent's slot index into its own segment (teardownSwapchain).
	Process &p = process();
	if (p.segment && sw.segmentSlot != UINT32_MAX && sw.ownerPid == int(::getpid()))
		pml::updateSwapchain(*p.segment, sw.segmentSlot, sw.info);
	PML_LOG(LogLevel::Info, "swapchain %llu: refresh cycle %llu ns (%.3f Hz)", (unsigned long long)sw.serial,
	        (unsigned long long)props.refreshDuration, props.refreshDuration ? 1e9 / double(props.refreshDuration) : 0.0);
}

struct CapPlan
{
	std::shared_ptr<SwapchainState> sw;  // whose cap state is used: the largest known swapchain of the present
	FrameLimiter *limiter = nullptr;     // present-side wait (null: none)
	bool early = false;
	bool reflex = false;                 // the cap goes through the app's NV_low_latency2 interval
	uint32_t appIntervalUs = 0;          // ... and the app's own minimumIntervalUs (the driver runs the longer one)
	bool hold = false;                   // the cap holds the app's frame-pacing thread (SwapchainState::hold)
	uint32_t holdKind = 0;               // SwapchainState::FrameHold::Kind
	uint32_t driverIntervalUs = 0;       // Reflex: the minimumIntervalUs the driver holds (app's and the layer's)
	bool active = false;                 // the cap code ran: report the status
	CapDecision decision;
	uint64_t earlyWaitNs = 0;
	uint64_t lateWaitNs = 0;
	bool lateDone = false;
};

CapPlan planCap(DeviceState &dev, const VkPresentInfoKHR *info, uint64_t nowNs)
{
	CapPlan plan;
	CapRuntime &rt = CapRuntime::get();
	pml::Segment *segment = process().segment.get();
	CapDecision d = rt.decide(segment, 0, 0);
	if (d.mode == uint32_t(pml::CapMode::Off) && !rt.everActive())
		return plan;   // never capped in this process: no lookups on the present path
	// One limiter per vkQueuePresentKHR call: the largest known swapchain's (a game's main window), the same one
	// whatever order the app lists its swapchains in.
	uint64_t bestArea = 0;
	for (uint32_t i = 0; i < info->swapchainCount; i++)
	{
		auto s = findSwapchain(dev, info->pSwapchains[i]);
		if (!s)
			continue;
		const uint64_t area = uint64_t(s->info.width) * uint64_t(s->info.height);
		if (!plan.sw || area > bestArea)
		{
			plan.sw = std::move(s);
			bestArea = area;
		}
	}
	if (!plan.sw)
		return plan;
	SwapchainState &sw = *plan.sw;
	if (d.mode == uint32_t(pml::CapMode::Auto))
		d = rt.decide(segment, sw.info.width, sw.info.height, sw.info.refreshDurationNs);
	plan.active = true;
	plan.decision = d;

	auto &ll = sw.lowLatency;
	const uint64_t lastSleep = ll.lastSleepNs.load(std::memory_order_relaxed);
	const bool reflexSeen = lastSleep != 0 && int64_t(nowNs - lastSleep) < kReflexRecentNs;
	// Reflex only for late placement: early asks for evenly spaced presents, and a Reflex interval paces only the
	// frames' starts (a game with uneven CPU work per frame still presents unevenly; docs/FINDINGS.md 6).
	const bool earlyPlacement = d.placement == uint32_t(pml::CapPlacement::Early);
	const bool wantReflex =
	    d.fps > 0.0 && d.allowReflex && !earlyPlacement && dev.dt.SetLatencySleepModeNV && reflexSeen;
	const uint32_t wantUs = wantReflex ? capIntervalUs(d.fps) : 0;
	{
		std::lock_guard<std::mutex> lock(ll.mutex);
		if (wantUs != ll.capUs && nowNs >= ll.retryAfterNs)
		{
			const uint32_t before = ll.capUs;
			ll.capUs = wantUs;
			const VkResult r = applyLatencyModeLocked(dev, sw);
			if (r != VK_SUCCESS)
			{
				// The driver kept its mode: the present-side wait caps until the retry.
				ll.capUs = before;
				ll.retryAfterNs = nowNs + kReflexRetryNs;
				if (!ll.failLogged)
				{
					ll.failLogged = true;
					PML_LOG(LogLevel::Warn,
					        "frame cap: vkSetLatencySleepModeNV failed (%d) on swapchain %llu; capping at the present "
					        "instead, retrying every second", int(r), (unsigned long long)sw.serial);
				}
			}
		}
		// Reflex once the driver holds the cap's interval; also while a failed change left it an earlier cap's
		// interval that alone caps at least as hard (a present-side wait on top would only add a second sleeper).
		plan.reflex = wantReflex && ll.capUs == wantUs;
		if (!plan.reflex && d.fps > 0.0 && ll.capUs != 0 && ll.capUs >= capIntervalUs(d.fps))
			plan.reflex = true;
		plan.appIntervalUs = ll.appSet && !ll.appNull ? ll.app.minimumIntervalUs : 0;
		plan.driverIntervalUs = ll.capUs ? mergedIntervalUs(plan.appIntervalUs, ll.capUs) : plan.appIntervalUs;
	}

	const uint64_t interval = (!plan.reflex && d.fps > 0.0) ? uint64_t(std::llround(1.0e9 / d.fps)) : 0;
	const bool early = d.placement == uint32_t(pml::CapPlacement::Early);
	// Late placement holds the app's frame-pacing thread once it is seen pacing (SwapchainState::FrameHold): its
	// present waits, else (not FIFO) its present-timing polls. In FIFO the poll hold is not used: DXVK holds a lock
	// across that poll which its submit thread needs for FIFO presents (measured: 29 ms instead of 17, uneven rate).
	auto &h = sw.hold;
	h.presentThread.store(currentThread(), std::memory_order_relaxed);
	h.presents.fetch_add(1, std::memory_order_relaxed);
	h.lastPlanNs.store(nowNs, std::memory_order_relaxed);
	uint32_t presentMode = uint32_t(sw.presentMode);
	if (const auto *modes = findStruct<VkSwapchainPresentModeInfoKHR>(info->pNext,
	                                                                  VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR))
		for (uint32_t i = 0; modes->pPresentModes && i < modes->swapchainCount && i < info->swapchainCount; i++)
			if (info->pSwapchains[i] == sw.handle)
				presentMode = uint32_t(modes->pPresentModes[i]);
	const bool fifo = presentMode == VK_PRESENT_MODE_FIFO_KHR || presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
	const int64_t recentNs = int64_t(std::max<uint64_t>(100'000'000, 3 * interval));
	auto steady = [&](const std::atomic<uint32_t> &streak, const std::atomic<uint64_t> &last) {
		return streak.load(std::memory_order_relaxed) >= kHoldMinStreak &&
		       int64_t(nowNs - last.load(std::memory_order_relaxed)) < recentNs;
	};
	uint32_t kind = SwapchainState::FrameHold::None;
	if (interval && !early && nowNs >= h.blockedUntilNs)
	{
		if (steady(h.blockingStreak, h.lastBlockingNs))
			kind = SwapchainState::FrameHold::PresentWait;
		else if (!fifo && interval <= kMaxPollHoldNs && steady(h.pollStreak, h.lastPollNs))
			kind = SwapchainState::FrameHold::TimingPoll;
	}
	// The held thread must pace the presents: over a window of 32 they average at least 0.9 of the interval. If not
	// (a thread that waits or polls without pacing the game: it then runs far faster), the present-side wait caps
	// for the next 2 s. The first window after the hold starts is not judged: frames queued under the present-side
	// wait drain in it. (No backstop wait at the present while holding: on DXVK's submit thread it becomes the pacer
	// itself, measured: presents every 0.95 T, the hold re-anchoring every frame.)
	if (kind != SwapchainState::FrameHold::None)
	{
		if (!h.windowStartNs)
		{
			h.windowStartNs = nowNs;
			h.windowFrames = 0;
		}
		else if (++h.windowFrames >= 32)
		{
			const uint64_t average = (nowNs - h.windowStartNs) / h.windowFrames;
			const bool ineffective = h.warmedUp && average * 10 < interval * 9;
			h.warmedUp = true;
			h.windowStartNs = nowNs;
			h.windowFrames = 0;
			if (ineffective)
			{
				h.blockedUntilNs = nowNs + 2'000'000'000ull;
				h.warmedUp = false;
				h.windowStartNs = 0;
				if (!h.ineffectiveLogged)
				{
					h.ineffectiveLogged = true;
					PML_LOG(LogLevel::Warn, "frame cap: holding the app's frame thread did not pace its presents "
					                        "(%.3f ms for %.3f ms); capping at the present instead (for 2 s, then "
					                        "trying again)", double(average) / 1e6, double(interval) / 1e6);
				}
				kind = SwapchainState::FrameHold::None;
			}
		}
	}
	else
	{
		h.windowStartNs = 0;
		h.warmedUp = false;
	}
	plan.hold = kind != SwapchainState::FrameHold::None;
	plan.holdKind = kind;
	h.kind.store(kind, std::memory_order_relaxed);
	h.intervalNs.store(plan.hold ? interval : 0, std::memory_order_relaxed);
	if (interval && !plan.hold)
	{
		if (!sw.limiter)
			sw.limiter = std::make_unique<FrameLimiter>(systemWaitClock());
		sw.limiter->setInterval(interval);
		plan.limiter = sw.limiter.get();
		plan.early = early;
	}
	else if (sw.limiter)
	{
		sw.limiter->setInterval(0);
	}
	return plan;
}

namespace {

void noteStreak(std::atomic<uint64_t> &last, std::atomic<uint32_t> &streak, uint64_t now)
{
	const uint64_t prev = last.exchange(now, std::memory_order_relaxed);
	if (prev && now - prev < 250'000'000)
		streak.fetch_add(1, std::memory_order_relaxed);
	else
		streak.store(1, std::memory_order_relaxed);
}

// The hold itself: the caller owns h.mutex.
void holdLocked(SwapchainState::FrameHold &h, uint64_t interval)
{
	if (!h.limiter)
		h.limiter = std::make_unique<FrameLimiter>(systemWaitClock());
	h.limiter->setInterval(interval);
	const uint64_t waited = h.limiter->wait();
	const LimiterStats st = h.limiter->stats();
	h.pendingWaitNs.fetch_add(waited, std::memory_order_relaxed);
	h.meanWaitNs.store(uint64_t(st.meanWaitNs), std::memory_order_relaxed);
	h.releaseP99Ns.store(st.releaseP99Ns, std::memory_order_relaxed);
	h.sleepFailures.store(st.sleepFailures, std::memory_order_relaxed);
}

} // namespace

// After the app's present wait returned VK_SUCCESS for presentId: hold the waiting thread until the cap's deadline
// (SwapchainState::FrameHold). The app's timeout (from callStartNs; UINT64_MAX: none) is never stretched.
void holdPresentWait(DeviceState &dev, VkSwapchainKHR handle, uint64_t presentId, uint64_t timeoutNs, uint64_t callStartNs)
{
	if (!CapRuntime::get().everActive())
		return;   // never capped in this process: nothing to count or hold
	auto sw = findSwapchain(dev, handle);
	if (!sw)
		return;
	auto &h = sw->hold;
	const uint64_t now = pml::monotonicNs();
	if (timeoutNs >= kBlockingPresentWaitNs)
		noteStreak(h.lastBlockingNs, h.blockingStreak, now);
	const uint64_t interval = h.intervalNs.load(std::memory_order_relaxed);
	if (!interval || h.kind.load(std::memory_order_relaxed) != SwapchainState::FrameHold::PresentWait ||
	    now - h.lastPlanNs.load(std::memory_order_relaxed) > std::max<uint64_t>(250'000'000, 3 * interval))
		return;   // not holding, or a stale plan (the cap went to another swapchain)
	std::unique_lock<std::mutex> lock(h.mutex, std::try_to_lock);
	if (!lock.owns_lock() || presentId <= h.lastHeldId)
		return;   // another thread holds this swapchain, or a present that was already held (a repeated wait)
	h.lastHeldId = presentId;
	if (timeoutNs != UINT64_MAX && (callStartNs + timeoutNs < callStartNs || callStartNs + timeoutNs < now + interval))
		return;   // the app's timeout leaves no room for a whole interval: never stretch a wait past it
	holdLocked(h, interval);
}

// Before the app's present-timing poll (vkGetPastPresentationTimingEXT) from another thread than the presenting one:
// hold it until the cap's deadline, once per present (SwapchainState::FrameHold).
void holdTimingPoll(SwapchainState &sw)
{
	if (!CapRuntime::get().everActive())
		return;   // never capped in this process: nothing to count or hold
	auto &h = sw.hold;
	const uint32_t presenter = h.presentThread.load(std::memory_order_relaxed);
	if (presenter == 0 || currentThread() == presenter)
		return;   // presenting thread not known yet, or its own polls: a hold there is the late wait's job
	const uint64_t now = pml::monotonicNs();
	noteStreak(h.lastPollNs, h.pollStreak, now);
	const uint64_t interval = h.intervalNs.load(std::memory_order_relaxed);
	if (!interval || h.kind.load(std::memory_order_relaxed) != SwapchainState::FrameHold::TimingPoll ||
	    now - h.lastPlanNs.load(std::memory_order_relaxed) > std::max<uint64_t>(250'000'000, 3 * interval))
		return;   // not holding, or a stale plan (the cap went to another swapchain)
	std::unique_lock<std::mutex> lock(h.mutex, std::try_to_lock);
	if (!lock.owns_lock())
		return;
	const uint64_t presents = h.presents.load(std::memory_order_relaxed);
	if (presents <= h.lastHeldPresent)
		return;   // nothing presented since the last hold: a repeated poll passes
	h.lastHeldPresent = presents;
	holdLocked(h, interval);
}

void lateCapWait(CapPlan &cap)
{
	if (cap.limiter && !cap.early && !cap.lateDone)
	{
		cap.lateDone = true;
		cap.lateWaitNs = cap.limiter->wait();
	}
}

pml::CapMethod capMethod(const CapPlan &cap)
{
	if (cap.decision.fps <= 0.0)
		return pml::CapMethod::None;
	if (cap.reflex)
		return pml::CapMethod::Reflex;
	if (cap.hold)
		return pml::CapMethod::FrameThread;
	if (cap.limiter)
		return cap.early ? pml::CapMethod::PresentEarly : pml::CapMethod::PresentLate;
	return pml::CapMethod::None;
}

void finishCap(CapPlan &cap, uint64_t nowNs)
{
	CapRuntime &rt = CapRuntime::get();
	pml::Segment *segment = process().segment.get();
	if (!cap.active || !cap.sw)
	{
		rt.reportIdle(segment, nowNs);   // acknowledges a settings change while nothing is capped
		return;
	}
	const pml::CapMethod method = capMethod(cap);
	LimiterStats stats;
	if (cap.limiter)
		stats = cap.limiter->stats();
	else if (cap.hold)
	{
		stats.meanWaitNs = double(cap.sw->hold.meanWaitNs.load(std::memory_order_relaxed));
		stats.releaseP99Ns = cap.sw->hold.releaseP99Ns.load(std::memory_order_relaxed);
		stats.sleepFailures = cap.sw->hold.sleepFailures.load(std::memory_order_relaxed);
	}
	// With Reflex the driver runs at the interval it holds (the app's own, or a failed change's earlier cap, when
	// longer than the cap's): report that rate.
	CapDecision shown = cap.decision;
	const bool appSlower = cap.reflex && cap.driverIntervalUs > capIntervalUs(shown.fps);
	if (appSlower)
		shown.fps = 1.0e6 / double(cap.driverIntervalUs);
	// One status line per change (method or cap), the first time it applies; the status follows at once.
	const uint32_t milli = uint32_t(std::lround(shown.fps * 1000.0));
	SwapchainState &sw = *cap.sw;
	bool changed = false;
	if (uint32_t(method) != sw.capLoggedMethod || milli != sw.capLoggedMilli || cap.holdKind != sw.capLoggedKind)
	{
		changed = true;
		sw.capLoggedMethod = uint32_t(method);
		sw.capLoggedMilli = milli;
		sw.capLoggedKind = cap.holdKind;
		const char *const kNames[] = {"none", "present wait (late)", "present wait (early)", "Reflex interval",
		                              cap.holdKind == SwapchainState::FrameHold::PresentWait
		                                  ? "hold on the app's frame thread (its vkWaitForPresent)"
		                                  : "hold on the app's frame thread (its present-timing poll)"};
		if (method == pml::CapMethod::None)
			PML_LOG(LogLevel::Info, "frame cap: off on swapchain %llu", (unsigned long long)sw.serial);
		else
			PML_LOG(LogLevel::Info, "frame cap: %.3f fps on swapchain %llu via %s%s%s", shown.fps,
			        (unsigned long long)sw.serial, kNames[uint32_t(method)],
			        cap.decision.mode == uint32_t(pml::CapMode::Auto) ? " (auto, VRR)" : "",
			        appSlower ? " (the driver's Reflex interval is longer than the cap's)" : "");
	}
	if (stats.sleepFailures && !sw.capSleepFailLogged)
	{
		sw.capSleepFailLogged = true;
		PML_LOG(LogLevel::Warn, "frame cap: the present thread cannot sleep (clock_nanosleep and nanosleep refused); "
		                        "the cap spins instead");
	}
	rt.report(segment, shown, method, cap.limiter || cap.hold ? &stats : nullptr, nowNs, changed);
}

// Returns false (nothing done) when none of the swapchains is known to the layer.
bool presentImpl(DeviceState &dev, VkQueue queue, const VkPresentInfoKHR *pPresentInfo, uint64_t startNs,
                 PresentOutcome &outcome, CapPlan &cap)
{
	PresentScratch &s = presentScratch();
	const uint32_t count = pPresentInfo->swapchainCount;
	s.states.assign(count, nullptr);
	s.plans.assign(count, PresentPlan{});
	s.params.assign(count, PresentParams{});
	s.timings.assign(count, VkPresentTimingInfoEXT{VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT});
	s.ids.assign(count, 0);
	s.results.assign(count, VK_SUCCESS);
	s.managed.assign(count, 0);
	s.open.assign(count, 0);
	s.drawn.assign(count, 0);
	s.overlayCpu.assign(count, 0);
	s.waits.assign(pPresentInfo->pWaitSemaphores, pPresentInfo->pWaitSemaphores + pPresentInfo->waitSemaphoreCount);

	const auto *timingsNode = findStruct<VkPresentTimingsInfoEXT>(pPresentInfo->pNext, VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT);
	const auto *id2Node = findStruct<VkPresentId2KHR>(pPresentInfo->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR);
	const auto *id1Node = findStruct<VkPresentIdKHR>(pPresentInfo->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR);
	const auto *modeNode = findStruct<VkSwapchainPresentModeInfoKHR>(pPresentInfo->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR);

	bool anyKnown = false, allManaged = true;
	for (uint32_t i = 0; i < count; i++)
	{
		s.states[i] = findSwapchain(dev, pPresentInfo->pSwapchains[i]);
		if (s.states[i])
			anyKnown = true;
		s.managed[i] = s.states[i] && s.states[i]->managed;
		if (!s.managed[i])
			allManaged = false;
	}
	if (!anyKnown)
		return false;

	OpenPresents openPresents(s, outcome);

	// The layer may add a VkPresentId2KHR only when every swapchain of the present was created with
	// PRESENT_ID_2 (VUID-VkPresentId2KHR-None-10820), or when the app sends one itself.
	bool canInjectId = id2Node != nullptr;
	if (!canInjectId)
	{
		canInjectId = true;
		for (uint32_t i = 0; i < count; i++)
		{
			const SwapchainState *sw = s.states[i].get();
			if (!sw || !(sw->info.timingFlags & (kSwapchainAppPresentId2 | kSwapchainLayerPresentId2)))
				canInjectId = false;
		}
	}

	// Register every present with its broker. This comes before the overlay: the overlay's queue
	// submission consumes the application's wait semaphores, which must only happen for a present
	// that is really sent.
	for (uint32_t i = 0; i < count; i++)
	{
		SwapchainState *sw = s.states[i].get();

		// App's view of this swapchain's chain entries.
		const VkPresentTimingInfoEXT *appTiming = nullptr;
		if (timingsNode && timingsNode->pTimingInfos && timingsNode->swapchainCount == count)
		{
			appTiming = &timingsNode->pTimingInfos[i];
			s.timings[i] = *appTiming;
		}
		uint64_t appId = 0;
		if (id2Node && id2Node->pPresentIds && id2Node->swapchainCount == count)
			appId = id2Node->pPresentIds[i];
		else if (id1Node && id1Node->pPresentIds && id1Node->swapchainCount == count)
			appId = id1Node->pPresentIds[i];
		s.ids[i] = id2Node && id2Node->pPresentIds && id2Node->swapchainCount == count ? id2Node->pPresentIds[i] : 0;

		if (!sw || !sw->broker)
			continue;

		PresentParams &pp = s.params[i];
		pp.presentStartNs = startNs;
		pp.threadId = currentThread();
		pp.imageIndex = pPresentInfo->pImageIndices[i];
		pp.presentMode = (modeNode && modeNode->pPresentModes && modeNode->swapchainCount == count)
		                     ? uint32_t(modeNode->pPresentModes[i])
		                     : uint32_t(sw->presentMode);
		pp.appPresentId = appId;
		pp.appTiming = appTiming;
		if (s.states[i] == cap.sw)   // the cap's swapchain: other swapchains of the call were not limited by it
		{
			// A hold in the app's present wait since the previous present counts towards this one.
			pp.capWaitNs = cap.earlyWaitNs + sw->hold.pendingWaitNs.exchange(0, std::memory_order_relaxed);
			if (pp.capWaitNs)
				pp.extraFlags |= kFrameCapWait;
			if (cap.reflex)
				pp.extraFlags |= kFrameCapReflex;
		}
		pp.observeOnly = !allManaged;  // a swapchain without the timing bit forbids any timing node
		pp.canInjectId = canInjectId;
		s.plans[i] = sw->broker->beginPresent(pp);
		s.open[i] = 1;
	}

	// Some, not all, swapchains hit the application's emulated timing-queue limit. The driver would reject
	// only those (their images stay acquired), which one call cannot express without rebuilding every
	// per-swapchain array of the chain without them. So their presents are planned again without the limit:
	// the app's requests go down over its queue size (the driver has the headroom) and the app gets the
	// driver's result.
	{
		bool anyForced = false, everyForced = count > 0;
		for (uint32_t i = 0; i < count; i++)
		{
			const bool forced = s.states[i] && s.states[i]->broker && s.plans[i].forcedResult != VK_SUCCESS;
			anyForced = anyForced || forced;
			everyForced = everyForced && forced;
		}
		if (anyForced && !everyForced)
		{
			for (uint32_t i = 0; i < count; i++)
			{
				if (!(s.states[i] && s.states[i]->broker && s.plans[i].forcedResult != VK_SUCCESS))
					continue;
				s.open[i] = 0;
				s.states[i]->broker->cancelPresent(s.plans[i].token);
				s.params[i].noLimit = true;
				s.plans[i] = s.states[i]->broker->beginPresent(s.params[i]);
				s.open[i] = 1;
			}
		}
	}

	bool anyReplace = false, anyInject = false;
	for (uint32_t i = 0; i < count; i++)
	{
		if (!s.open[i] || s.params[i].observeOnly)
			continue;
		s.timings[i] = s.plans[i].timing;
		if (s.plans[i].timingRequested || s.plans[i].forcedResult != VK_SUCCESS)
			anyReplace = true;
		if (s.plans[i].inject)
		{
			anyInject = true;
			s.ids[i] = s.plans[i].injectedId;
		}
	}

	// Every swapchain hit the application's emulated timing-queue limit: the driver would have
	// rejected the present (no image is shown, the images stay acquired) but still executed its
	// semaphore waits and signaled its fences, so those are done here and nothing goes down.
	bool allForced = count > 0;
	for (uint32_t i = 0; i < count; i++)
		allForced = allForced && s.states[i] && s.states[i]->broker && s.plans[i].forcedResult != VK_SUCCESS;
	if (allForced)
	{
		// Once the application's waits are used up and its fences signaled (outcome.syncConsumed), nothing may
		// go down whatever happens next: hookQueuePresent's fallback checks this.
		outcome.returned = s.plans[0].forcedResult;
		if (!consumePresentSync(dev, queue, *pPresentInfo, &outcome.syncConsumed))
			PML_LOG_ONCE(LogLevel::Warn, "cannot consume the semaphores of a rejected present (vkQueueSubmit failed)");
		const uint64_t nowNs = pml::monotonicNs();
		for (uint32_t i = 0; i < count; i++)
		{
			s.open[i] = 0;
			s.states[i]->broker->endPresent(s.plans[i].token, nowNs, s.plans[i].forcedResult);
			if (pPresentInfo->pResults)
				pPresentInfo->pResults[i] = s.plans[i].forcedResult;
		}
		outcome.returned = s.plans[0].forcedResult;
		return true;
	}

	// Overlay draw, before anything is sent down. It may replace the wait semaphores. A swapchain
	// whose present was rejected by the emulated queue limit gets no overlay (its image is shown).
	for (uint32_t i = 0; i < count; i++)
	{
		SwapchainState *sw = s.states[i].get();
		if (!sw || !sw->overlayActive)
			continue;
		if (sw->broker && s.plans[i].forcedResult != VK_SUCCESS)
			continue;
		PresentContext ctx;
		ctx.swapchain = &sw->overlayCtx;
		ctx.queue = queue;
		ctx.queueFamily = queueFamilyIndex(dev.ctx, queue);
		ctx.imageIndex = pPresentInfo->pImageIndices[i];
		ctx.waits = &s.waits;
		const size_t before = s.waits.size();
		const VkSemaphore first = s.waits.empty() ? VK_NULL_HANDLE : s.waits[0];
		try
		{
			overlay::onPresent(ctx);
		}
		catch (...)
		{
			PML_LOG_ONCE(LogLevel::Error, "internal error in the overlay; no overlay on this present");
			ctx.drawn = false;
		}
		// The overlay submission consumed the application's semaphores whenever it replaced them.
		if (s.waits.size() != before || (!s.waits.empty() && s.waits[0] != first))
			outcome.waitsReplaced = true;
		s.drawn[i] = ctx.drawn;
		s.overlayCpu[i] = ctx.cpuNs;
		if (sw->broker)
			sw->broker->noteOverlay(s.plans[i].token, ctx.drawn, ctx.cpuNs);
	}

	// Build what goes down.
	VkPresentInfoKHR out = *pPresentInfo;
	PresentChain chain;
	bool useChain = anyReplace || anyInject;
	if (useChain)
	{
		VkStructureType unknownType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		if (!chain.rebuild(*pPresentInfo, anyReplace ? s.timings.data() : nullptr, anyInject ? s.ids.data() : nullptr,
		                   pPresentInfo->pResults ? nullptr : s.results.data(), &out, &unknownType))
		{
			PML_LOG_ONCE(LogLevel::Warn, "present chain has a structure the layer cannot copy (sType %d); presenting untouched",
			             int(unknownType));
			for (uint32_t i = 0; i < count; i++)
				if (s.states[i] && s.states[i]->broker && s.plans[i].token)
					s.states[i]->broker->dropLayerTiming(s.plans[i].token);
			out = *pPresentInfo;
			useChain = false;
		}
	}
	if (!useChain && !pPresentInfo->pResults && count > 1)
		out.pResults = s.results.data();
	if (outcome.waitsReplaced)
	{
		out.waitSemaphoreCount = uint32_t(s.waits.size());
		out.pWaitSemaphores = s.waits.data();
	}
	VkResult *resultsOut = out.pResults;

	outcome.calledDown = true;
	VkResult result = dev.dt.QueuePresentKHR(queue, &out);
	outcome.downResult = result;
	uint64_t endNs = pml::monotonicNs();

	// The driver refused the layer's additional timing slot: repeat without it, once. The failed
	// attempt already executed the semaphore waits, so the repeat waits for nothing. It does not signal
	// its present fences (measured on this driver: the fence of a QUEUE_FULL present never signals), so
	// the repeat carries them. The layer's timing request is dropped; the application's own request is
	// only ever sent when the driver accepted its slot, so what remains is the application's chain
	// without timing stages.
	// Only for a single swapchain: with several, the driver's per-swapchain results say which images went down, and
	// repeating the whole call would present those again. There each swapchain keeps the driver's result, and the
	// layer stops asking for its own timing on the refused one for a while. (Never seen on this driver: the broker
	// keeps the driver's queue from filling.)
	if (result == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT && useChain && count > 1)
	{
		for (uint32_t i = 0; i < count; i++)
			if (resultsOut && resultsOut[i] == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT && s.states[i] &&
			    s.states[i]->broker && s.plans[i].token && !s.plans[i].appStagesSent)
				s.states[i]->broker->dropLayerTiming(s.plans[i].token);
	}
	if (result == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT && useChain && count == 1)
	{
		bool appAsked = false;
		for (uint32_t i = 0; i < count; i++)
			appAsked = appAsked || s.plans[i].appStagesSent;
		if (!appAsked)
		{
			for (uint32_t i = 0; i < count; i++)
				if (s.states[i] && s.states[i]->broker && s.plans[i].token)
					s.states[i]->broker->dropLayerTiming(s.plans[i].token);
			// The application's timing entries without any stage request (also those the emulated limit
			// zeroed): the chain nodes keep their shape, nothing is asked of the driver.
			for (uint32_t i = 0; i < count; i++)
				s.timings[i].presentStageQueries = 0;
			VkPresentInfoKHR retry = *pPresentInfo;
			PresentChain retryChain;
			bool timingNode = findStruct<VkPresentTimingsInfoEXT>(pPresentInfo->pNext, VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT);
			if (retryChain.rebuild(*pPresentInfo, timingNode ? s.timings.data() : nullptr, nullptr,
			                       pPresentInfo->pResults ? nullptr : s.results.data(), &retry, nullptr))
			{
				retry.waitSemaphoreCount = 0;
				retry.pWaitSemaphores = nullptr;
				if (!retry.pResults && count > 1)
					retry.pResults = s.results.data();
				resultsOut = retry.pResults;
				result = dev.dt.QueuePresentKHR(queue, &retry);
				outcome.downResult = result;
				endNs = pml::monotonicNs();
				PML_LOG_ONCE(LogLevel::Warn, "driver timing queue full: layer timing request dropped for a present");
			}
		}
	}
	outcome.downResult = result;

	// Frame cap, late placement: after the driver's present, inside the app's call (so the app's next frame starts
	// later). The record's present end includes the wait, as the app experiences it.
	lateCapWait(cap);
	if (cap.lateDone)
		endNs = pml::monotonicNs();

	// Per swapchain results, emulated errors, records.
	VkResult returned = result;
	for (uint32_t i = 0; i < count; i++)
	{
		VkResult r = resultsOut ? resultsOut[i] : result;
		SwapchainState *sw = s.states[i].get();
		if (sw && sw->broker)
		{
			s.open[i] = 0;
			if (cap.lateWaitNs && s.states[i] == cap.sw)
				sw->broker->noteCapWait(s.plans[i].token, cap.lateWaitNs);
			sw->broker->endPresent(s.plans[i].token, endNs, r);
			updateRefreshDuration(dev, *sw, endNs);
			if (s.plans[i].forcedResult != VK_SUCCESS && r >= 0)
			{
				if (pPresentInfo->pResults)
					pPresentInfo->pResults[i] = s.plans[i].forcedResult;
				if (returned >= 0)
					returned = s.plans[i].forcedResult;
			}
		}
		else if (sw && sw->overlayActive && overlayLoaded())
		{
			// No present timing on this swapchain: the overlay still gets a CPU-side record (present
			// start/end), so it shows frame rate and frame times instead of a permanent NA.
			FrameRecord rec = {};
			rec.swapchainSerial = sw->serial;
			rec.swapchainHandle = uint64_t(sw->handle);
			rec.frameIndex = ++sw->cpuFrames;
			rec.presentStartNs = startNs;
			rec.presentEndNs = endNs;
			rec.threadId = currentThread();
			rec.presentResult = r;
			rec.presentMode = (modeNode && modeNode->pPresentModes && modeNode->swapchainCount == count)
			                      ? uint32_t(modeNode->pPresentModes[i])
			                      : uint32_t(sw->presentMode);
			rec.imageIndex = pPresentInfo->pImageIndices[i];
			rec.overlayCpuNs = s.overlayCpu[i];
			if (s.states[i] == cap.sw)
			{
				rec.capWaitNs = cap.earlyWaitNs + cap.lateWaitNs + sw->hold.pendingWaitNs.exchange(0, std::memory_order_relaxed);
				if (rec.capWaitNs)
					rec.flags |= kFrameCapWait;
				if (cap.reflex)
					rec.flags |= kFrameCapReflex;
			}
			if (s.drawn[i])
				rec.flags |= kFrameOverlayDrawn;
			if (r < 0)
				rec.flags |= kFramePresentFailed;
			else if (r == VK_SUBOPTIMAL_KHR)
				rec.flags |= kFrameSuboptimal;
			if (dev.gpu)
				dev.gpu->fill(rec);  // no ready time: drops the frame's GPU intervals, no GPU numbers
			overlay::onRecord(rec, sw->info);
		}
	}

	Process &p = process();
	if (p.segment)
		pml::touchHeartbeat(*p.segment, endNs);
	outcome.returned = returned;
	return true;
}

VKAPI_ATTR VkResult VKAPI_CALL hookQueuePresent(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
	const uint64_t startNs = pml::monotonicNs();
	auto dev = deviceFor(dispatchKey(queue));
	if (!dev)
		return VK_ERROR_DEVICE_LOST;  // cannot happen: the loader only routes known devices here

	// Frame cap: decided per present; early placement waits here, before anything of the present happens.
	CapPlan cap;
	try
	{
		cap = planCap(*dev, pPresentInfo, startNs);
		if (cap.limiter && cap.early)
			cap.earlyWaitNs = cap.limiter->wait();
	}
	catch (...)
	{
		PML_LOG_ONCE(LogLevel::Error, "internal error in the frame cap; no cap on this present");
		cap = CapPlan{};
	}

	if (dev->managedSwapchains.load(std::memory_order_relaxed) == 0)
	{
		const VkResult r = dev->dt.QueuePresentKHR(queue, pPresentInfo);
		lateCapWait(cap);
		finishCap(cap, pml::monotonicNs());
		return r;
	}

	PresentOutcome outcome;
	try
	{
		if (!presentImpl(*dev, queue, pPresentInfo, startNs, outcome, cap))
		{
			const VkResult r = dev->dt.QueuePresentKHR(queue, pPresentInfo);
			lateCapWait(cap);
			finishCap(cap, pml::monotonicNs());
			return r;
		}
		finishCap(cap, pml::monotonicNs());
		return outcome.returned;
	}
	catch (...)
	{
		PML_LOG_ONCE(LogLevel::Error, "internal error in vkQueuePresentKHR; falling back to pass-through");
		if (outcome.syncConsumed)
			return outcome.returned;   // the rejected present's waits and fences are used up: nothing goes down
		if (outcome.calledDown)
			return outcome.downResult;
		if (outcome.waitsReplaced)
		{
			// The overlay already waited on the application's semaphores; its own semaphore replaces them.
			const PresentScratch &s = presentScratch();
			VkPresentInfoKHR fallback = *pPresentInfo;
			fallback.waitSemaphoreCount = uint32_t(s.waits.size());
			fallback.pWaitSemaphores = s.waits.data();
			return dev->dt.QueuePresentKHR(queue, &fallback);
		}
		return dev->dt.QueuePresentKHR(queue, pPresentInfo);
	}
}

// ---------------------------------------------------------------------------------------
// App-visible present timing calls
// ---------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL hookSetQueueSize(VkDevice device, VkSwapchainKHR swapchain, uint32_t size)
{
	auto dev = deviceFor(dispatchKey(device));
	if (!dev)
		return VK_ERROR_INITIALIZATION_FAILED;
	auto sw = findSwapchain(*dev, swapchain);
	if (!sw || !sw->broker || !sw->managed)
		return dev->dt.SetSwapchainPresentTimingQueueSizeEXT(device, swapchain, size);

	// Like the driver: a queue smaller than the number of results the application has not collected yet
	// is refused with VK_NOT_READY (a success code) and nothing changes.
	const BrokerStats stats = sw->broker->stats();
	if (size < stats.appOutstanding)
		return VK_NOT_READY;

	// Ask the driver for the app's size plus headroom, never less than it holds now: the layer's own
	// pending feedback slots would make a smaller queue come back VK_NOT_READY although the
	// application's results all fit. Fall back to less headroom if the driver refuses.
	const uint32_t wanted = std::max(sw->broker->driverQueueSizeFor(size), stats.driverQueueSize);
	for (uint32_t total : {wanted, size + 16u, size + 4u, size + 1u, size})
	{
		if (total > wanted)
			continue;
		VkResult r = dev->dt.SetSwapchainPresentTimingQueueSizeEXT(device, swapchain, total);
		if (r == VK_SUCCESS)
		{
			sw->broker->commitAppQueueSize(size, total);
			return VK_SUCCESS;
		}
		if (total == size)
			return r;  // the app's own size is invalid: report the driver's verdict
	}
	return VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL hookGetPastPresentationTiming(VkDevice device,
                                                             const VkPastPresentationTimingInfoEXT *pInfo,
                                                             VkPastPresentationTimingPropertiesEXT *pProps)
{
	auto dev = deviceFor(dispatchKey(device));
	if (!dev)
		return VK_ERROR_INITIALIZATION_FAILED;
	auto sw = pInfo ? findSwapchain(*dev, pInfo->swapchain) : nullptr;
	if (sw)
	{
		try
		{
			holdTimingPoll(*sw);   // frame cap: before the query, so the app gets the newest results
		}
		catch (...)
		{
			PML_LOG_ONCE(LogLevel::Error, "internal error in the frame cap's timing-poll hold; not held");
		}
	}
	if (!sw || !sw->broker || !sw->managed || !pProps)
		return dev->dt.GetPastPresentationTimingEXT(device, pInfo, pProps);
	try
	{
		// The broker also notices changed time domains in its own polls (see TimingBroker::pollLocked).
		return sw->broker->appGetPast(pInfo, pProps);
	}
	catch (...)
	{
		PML_LOG_ONCE(LogLevel::Error, "internal error in vkGetPastPresentationTimingEXT");
		return VK_ERROR_UNKNOWN;
	}
}

// ---------------------------------------------------------------------------------------
// Submissions (GPU timestamps)
// ---------------------------------------------------------------------------------------

// The device and queue family of a queue the layer handed out, or nullptr (then the call passes through).
std::shared_ptr<DeviceState> gpuTimedDevice(VkQueue queue, uint32_t *family)
{
	auto dev = deviceFor(dispatchKey(queue));
	if (!dev || !dev->gpu)
		return dev;
	std::lock_guard<std::mutex> lock(dev->mutex);
	auto it = dev->queueFamilies.find(queue);
	if (it == dev->queueFamilies.end())
		return dev;
	*family = it->second;
	return dev;
}

VKAPI_ATTR VkResult VKAPI_CALL hookQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence)
{
	uint32_t family = UINT32_MAX;
	auto dev = gpuTimedDevice(queue, &family);
	if (!dev)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (!dev->gpu || family == UINT32_MAX)
		return dev->dt.QueueSubmit(queue, count, submits, fence);
	return dev->gpu->submit(dev->dt.QueueSubmit, queue, family, count, submits, fence);
}

template <PFN_vkQueueSubmit2 DeviceDispatch::*Next>
VKAPI_ATTR VkResult VKAPI_CALL hookQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence)
{
	uint32_t family = UINT32_MAX;
	auto dev = gpuTimedDevice(queue, &family);
	if (!dev)
		return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkQueueSubmit2 next = (dev->dt).*Next;
	if (!next)
		next = dev->dt.QueueSubmit2 ? dev->dt.QueueSubmit2 : dev->dt.QueueSubmit2KHR;
	if (!dev->gpu || family == UINT32_MAX)
		return next(queue, count, submits, fence);
	return dev->gpu->submit2(next, queue, family, count, submits, fence);
}

// ---------------------------------------------------------------------------------------
// VK_NV_low_latency2 (Reflex): the frame cap's interval goes into the app's sleep mode
// ---------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL hookSetLatencySleepMode(VkDevice device, VkSwapchainKHR swapchain,
                                                       const VkLatencySleepModeInfoNV *pSleepModeInfo)
{
	auto dev = deviceFor(dispatchKey(device));
	if (!dev || !dev->dt.SetLatencySleepModeNV)
		return VK_ERROR_INITIALIZATION_FAILED;
	auto sw = findSwapchain(*dev, swapchain);
	if (!sw)
		return dev->dt.SetLatencySleepModeNV(device, swapchain, pSleepModeInfo);
	auto &ll = sw->lowLatency;
	std::lock_guard<std::mutex> lock(ll.mutex);
	VkResult r;
	if (ll.capUs == 0)
	{
		r = dev->dt.SetLatencySleepModeNV(device, swapchain, pSleepModeInfo);
	}
	else
	{
		// A cap is in force: the app's mode with the interval raised to the cap's (the app's own lower cap stays).
		VkLatencySleepModeInfoNV mode = {VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
		if (pSleepModeInfo)
			mode = *pSleepModeInfo;
		mode.minimumIntervalUs = mergedIntervalUs(mode.minimumIntervalUs, ll.capUs);
		r = dev->dt.SetLatencySleepModeNV(device, swapchain, &mode);
	}
	// The driver's mode changed only on success: only then is it the app's mode (plus the layer's interval).
	if (r == VK_SUCCESS)
	{
		ll.appSet = true;
		ll.appNull = pSleepModeInfo == nullptr;
		if (pSleepModeInfo)
		{
			ll.app = *pSleepModeInfo;
			ll.app.pNext = nullptr;
		}
		ll.layerApplied = ll.capUs != 0;
	}
	return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hookLatencySleep(VkDevice device, VkSwapchainKHR swapchain,
                                                const VkLatencySleepInfoNV *pSleepInfo)
{
	auto dev = deviceFor(dispatchKey(device));
	if (!dev || !dev->dt.LatencySleepNV)
		return VK_ERROR_INITIALIZATION_FAILED;
	const VkResult r = dev->dt.LatencySleepNV(device, swapchain, pSleepInfo);
	// Reflex counts as in use only while the driver accepts the app's sleeps on this swapchain.
	if (r == VK_SUCCESS)
		if (auto sw = findSwapchain(*dev, swapchain))
		{
			sw->lowLatency.lastSleepNs.store(pml::monotonicNs(), std::memory_order_relaxed);
			sw->lowLatency.sleeps.fetch_add(1, std::memory_order_relaxed);
		}
	return r;
}

// The app's present waits: the frame cap's hold placement (holdPresentWait). A failed or timed-out wait passes.
VKAPI_ATTR VkResult VKAPI_CALL hookWaitForPresent(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId,
                                                  uint64_t timeout)
{
	auto dev = deviceFor(dispatchKey(device));
	if (!dev || !dev->dt.WaitForPresentKHR)
		return VK_ERROR_INITIALIZATION_FAILED;
	const uint64_t start = pml::monotonicNs();
	const VkResult r = dev->dt.WaitForPresentKHR(device, swapchain, presentId, timeout);
	if (r == VK_SUCCESS)
	{
		try
		{
			holdPresentWait(*dev, swapchain, presentId, timeout, start);
		}
		catch (...)
		{
			PML_LOG_ONCE(LogLevel::Error, "internal error in the frame cap's present-wait hold; not held");
		}
	}
	return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hookWaitForPresent2(VkDevice device, VkSwapchainKHR swapchain,
                                                   const VkPresentWait2InfoKHR *pPresentWait2Info)
{
	auto dev = deviceFor(dispatchKey(device));
	if (!dev || !dev->dt.WaitForPresent2KHR)
		return VK_ERROR_INITIALIZATION_FAILED;
	const uint64_t start = pml::monotonicNs();
	const VkResult r = dev->dt.WaitForPresent2KHR(device, swapchain, pPresentWait2Info);
	if (r == VK_SUCCESS && pPresentWait2Info)
	{
		try
		{
			holdPresentWait(*dev, swapchain, pPresentWait2Info->presentId, pPresentWait2Info->timeout, start);
		}
		catch (...)
		{
			PML_LOG_ONCE(LogLevel::Error, "internal error in the frame cap's present-wait hold; not held");
		}
	}
	return r;
}

// ---------------------------------------------------------------------------------------
// Proc addr lookup
// ---------------------------------------------------------------------------------------

struct Hook
{
	const char *name;
	PFN_vkVoidFunction function;
};

#define HOOK(name, fn) {name, reinterpret_cast<PFN_vkVoidFunction>(fn)}

const Hook kDeviceHooks[] = {
	HOOK("vkDestroyDevice", hookDestroyDevice),
	HOOK("vkGetDeviceQueue", hookGetDeviceQueue),
	HOOK("vkGetDeviceQueue2", hookGetDeviceQueue2),
	HOOK("vkCreateSwapchainKHR", hookCreateSwapchain),
	HOOK("vkDestroySwapchainKHR", hookDestroySwapchain),
	HOOK("vkQueuePresentKHR", hookQueuePresent),
	HOOK("vkQueueSubmit", hookQueueSubmit),
	HOOK("vkQueueSubmit2", hookQueueSubmit2<&DeviceDispatch::QueueSubmit2>),
	HOOK("vkQueueSubmit2KHR", hookQueueSubmit2<&DeviceDispatch::QueueSubmit2KHR>),
	HOOK("vkSetSwapchainPresentTimingQueueSizeEXT", hookSetQueueSize),
	HOOK("vkSetLatencySleepModeNV", hookSetLatencySleepMode),
	HOOK("vkLatencySleepNV", hookLatencySleep),
	HOOK("vkWaitForPresentKHR", hookWaitForPresent),
	HOOK("vkWaitForPresent2KHR", hookWaitForPresent2),
	HOOK("vkGetPastPresentationTimingEXT", hookGetPastPresentationTiming),
};

PFN_vkVoidFunction findDeviceHook(const char *name)
{
	for (const Hook &h : kDeviceHooks)
		if (!std::strcmp(h.name, name))
			return h.function;
	return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookGetDeviceProcAddr(VkDevice device, const char *pName)
{
	if (!pName)
		return nullptr;
	if (!std::strcmp(pName, "vkGetDeviceProcAddr"))
		return reinterpret_cast<PFN_vkVoidFunction>(hookGetDeviceProcAddr);
	auto dev = deviceFor(dispatchKey(device));
	if (!dev || !dev->dt.GetDeviceProcAddr)
		return nullptr;
	PFN_vkVoidFunction next = dev->dt.GetDeviceProcAddr(device, pName);
	if (!next)
		return nullptr;  // only hook what the next layer provides
	if (PFN_vkVoidFunction hook = findDeviceHook(pName))
		return hook;
	return next;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookGetInstanceProcAddr(VkInstance instance, const char *pName)
{
	if (!pName)
		return nullptr;
	if (!std::strcmp(pName, "vkGetInstanceProcAddr"))
		return reinterpret_cast<PFN_vkVoidFunction>(hookGetInstanceProcAddr);
	if (!std::strcmp(pName, "vkCreateInstance"))
		return reinterpret_cast<PFN_vkVoidFunction>(hookCreateInstance);
	if (!instance)
		return nullptr;

	auto state = instanceFor(dispatchKey(instance));
	if (!state || !state->dt.GetInstanceProcAddr)
		return nullptr;
	PFN_vkVoidFunction next = state->dt.GetInstanceProcAddr(instance, pName);
	if (!next)
		return nullptr;

	if (!std::strcmp(pName, "vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(hookDestroyInstance);
	if (!std::strcmp(pName, "vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(hookCreateDevice);
	if (!std::strcmp(pName, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(hookGetDeviceProcAddr);
	if (!std::strcmp(pName, "vkDestroySurfaceKHR")) return reinterpret_cast<PFN_vkVoidFunction>(hookDestroySurface);
	if (!std::strcmp(pName, "vkCreateWaylandSurfaceKHR"))
		return reinterpret_cast<PFN_vkVoidFunction>(&hookCreateSurface<&InstanceDispatch::CreateWaylandSurfaceKHR, SurfaceKind::Wayland>);
	if (!std::strcmp(pName, "vkCreateXcbSurfaceKHR"))
		return reinterpret_cast<PFN_vkVoidFunction>(&hookCreateSurface<&InstanceDispatch::CreateXcbSurfaceKHR, SurfaceKind::Xcb>);
	if (!std::strcmp(pName, "vkCreateXlibSurfaceKHR"))
		return reinterpret_cast<PFN_vkVoidFunction>(&hookCreateSurface<&InstanceDispatch::CreateXlibSurfaceKHR, SurfaceKind::Xlib>);
	if (!std::strcmp(pName, "vkCreateDisplayPlaneSurfaceKHR"))
		return reinterpret_cast<PFN_vkVoidFunction>(&hookCreateSurface<&InstanceDispatch::CreateDisplayPlaneSurfaceKHR, SurfaceKind::Display>);
	if (!std::strcmp(pName, "vkCreateHeadlessSurfaceEXT"))
		return reinterpret_cast<PFN_vkVoidFunction>(&hookCreateSurface<&InstanceDispatch::CreateHeadlessSurfaceEXT, SurfaceKind::Headless>);
	if (PFN_vkVoidFunction hook = findDeviceHook(pName))
		return hook;
	return next;
}

} // namespace

// ---------------------------------------------------------------------------------------
// Services for the overlay
// ---------------------------------------------------------------------------------------

pml::Segment *processSegment()
{
	Process &p = process();
	std::lock_guard<std::mutex> lock(p.mutex);
	return p.segment.get();
}

uint32_t queueFamilyIndex(const DeviceContext &device, VkQueue queue)
{
	auto *state = static_cast<DeviceState *>(device.internal);
	if (!state)
		return 0;
	std::lock_guard<std::mutex> lock(state->mutex);
	auto it = state->queueFamilies.find(queue);
	return it == state->queueFamilies.end() ? 0 : it->second;
}

} // namespace pml::layer

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
	if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (pVersionStruct->loaderLayerInterfaceVersion < 2)
		return VK_ERROR_INITIALIZATION_FAILED;
	pVersionStruct->loaderLayerInterfaceVersion = 2;
	pVersionStruct->pfnGetInstanceProcAddr = pml::layer::hookGetInstanceProcAddr;
	pVersionStruct->pfnGetDeviceProcAddr = pml::layer::hookGetDeviceProcAddr;
	pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
	return VK_SUCCESS;
}
