// SPDX-License-Identifier: MIT
// timing_probe: a small SDL3/Vulkan application that uses VK_EXT_present_timing and
// VK_KHR_present_id2 the way DXVK does, and prints exactly what the driver (or the layer) returns.
//
// Used to learn how the driver behaves (time domains, stages per surface type, what a replaced MAILBOX
// image reports, partial results...) and to compare "what the app sees" with and without
// VkLayer_presentmon: run it twice and diff the report lines.
//
//   timing_probe --mode 2 --frames 120 [--partial] [--ooo] [--ids v2|v1|both|none] [--queue 8]
//                [--poll-every 1] [--stages all|last|<mask>] [--x11] [--quiet] [--no-timing]
//
// Output: "CAPS", "DOMAIN" and "REPORT"/"POLL" lines on stdout, one summary line at the end.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

#define CHECK(x) do { VkResult r_ = (x); if (r_ < 0) { std::fprintf(stderr, "%s failed: %d (line %d)\n", #x, int(r_), __LINE__); std::exit(1); } } while (0)

uint64_t nowNs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

const char *stageName(uint32_t s)
{
	switch (s)
	{
	case 1: return "QOE";
	case 2: return "DEQ";
	case 4: return "FPO";
	case 8: return "FPV";
	}
	return "?";
}

struct Options
{
	int mode = 2;
	int frames = 120;
	bool partial = false, ooo = false, quiet = false, noTiming = false, x11 = false;
	std::string ids = "v2";
	uint32_t queue = 8;
	int pollEvery = 1;
	std::string stages = "all";
	int width = 320, height = 240;
	int sleepUs = 0;
	int windows = 1;
	bool noCleanup = false;
	bool fullscreen = false;
};

} // namespace

int main(int argc, char **argv)
{
	Options o;
	setvbuf(stdout, nullptr, _IOLBF, 0);
	for (int i = 1; i < argc; i++)
	{
		std::string a = argv[i];
		auto v = [&]() { return i + 1 < argc ? argv[++i] : ""; };
		if (a == "--mode") o.mode = std::atoi(v());
		else if (a == "--frames") o.frames = std::atoi(v());
		else if (a == "--partial") o.partial = true;
		else if (a == "--ooo") o.ooo = true;
		else if (a == "--quiet") o.quiet = true;
		else if (a == "--no-timing") o.noTiming = true;
		else if (a == "--x11") o.x11 = true;
		else if (a == "--ids") o.ids = v();
		else if (a == "--queue") o.queue = uint32_t(std::atoi(v()));
		else if (a == "--poll-every") o.pollEvery = std::atoi(v());
		else if (a == "--stages") o.stages = v();
		else if (a == "--sleep-us") o.sleepUs = std::atoi(v());
		else if (a == "--windows") o.windows = std::atoi(v());
		else if (a == "--no-cleanup") o.noCleanup = true;
		else if (a == "--fullscreen") o.fullscreen = true;
		else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
	}
	if (o.x11)
		SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
	if (!SDL_Init(SDL_INIT_VIDEO) || !SDL_Vulkan_LoadLibrary(nullptr))
	{
		std::fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
		return 1;
	}
	struct Chain
	{
		SDL_Window *window = nullptr;
		VkSurfaceKHR surface = VK_NULL_HANDLE;
		VkSwapchainKHR swapchain = VK_NULL_HANDLE;
		std::vector<VkImage> images;
		std::vector<VkSemaphore> doneSem;
		VkSemaphore acquireSem = VK_NULL_HANDLE;
		uint64_t domainId = 0;
		uint32_t stageMask = 0;
		uint32_t swapchainFlags = 0;
		bool useTiming = false;
		VkPresentTimingSurfaceCapabilitiesEXT tcaps = {};
		VkSurfaceCapabilitiesPresentId2KHR id2caps = {};
		uint32_t imageIndex = 0;
		int reports = 0, complete = 0, withFpo = 0, noFpoComplete = 0, partialSeen = 0;
		uint64_t idCounter = 0;
		uint64_t timingCounterSeen = 0;
	};
	std::vector<Chain> chains(size_t(std::max(1, o.windows)));
	for (size_t i = 0; i < chains.size(); i++)
	{
		chains[i].window = SDL_CreateWindow("timing_probe", o.width, o.height,
		                                    SDL_WINDOW_VULKAN | (o.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
		if (!chains[i].window)
		{
			std::fprintf(stderr, "window failed: %s\n", SDL_GetError());
			return 1;
		}
	}
	std::printf("VIDEO_DRIVER %s\n", SDL_GetCurrentVideoDriver());

	uint32_t sdlExtCount = 0;
	const char *const *sdlExt = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
	std::vector<const char *> instExt(sdlExt, sdlExt + sdlExtCount);
	instExt.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
	instExt.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "timing_probe";
	app.pEngineName = "timing_probe_engine";
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = uint32_t(instExt.size());
	ici.ppEnabledExtensionNames = instExt.data();
	VkInstance instance;
	CHECK(vkCreateInstance(&ici, nullptr, &instance));

	for (Chain &c : chains)
		if (!SDL_Vulkan_CreateSurface(c.window, instance, nullptr, &c.surface))
		{
			std::fprintf(stderr, "surface failed: %s\n", SDL_GetError());
			return 1;
		}
	VkSurfaceKHR surface = chains[0].surface;
	uint32_t n = 1;
	VkPhysicalDevice phys;
	vkEnumeratePhysicalDevices(instance, &n, &phys);

	const bool wantV1 = o.ids == "v1" || o.ids == "both";
	const bool wantV2 = o.ids == "v2" || o.ids == "both";
	std::vector<const char *> devExt = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	VkPhysicalDevicePresentTimingFeaturesEXT ftTiming = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
	VkPhysicalDevicePresentId2FeaturesKHR ftId2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
	VkPhysicalDevicePresentIdFeaturesKHR ftId1 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
	void *chain = nullptr;
	if (!o.noTiming)
	{
		devExt.push_back(VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
		devExt.push_back(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
		devExt.push_back(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
		ftTiming.presentTiming = VK_TRUE;
		ftTiming.pNext = chain; chain = &ftTiming;
	}
	if (wantV2 || !o.noTiming)
	{
		if (std::find(devExt.begin(), devExt.end(), VK_KHR_PRESENT_ID_2_EXTENSION_NAME) == devExt.end())
			devExt.push_back(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
		ftId2.presentId2 = VK_TRUE;
		ftId2.pNext = chain; chain = &ftId2;
	}
	if (wantV1)
	{
		devExt.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
		ftId1.presentId = VK_TRUE;
		ftId1.pNext = chain; chain = &ftId1;
	}
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	uint32_t qcount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, nullptr);
	std::vector<VkQueueFamilyProperties> qprops(qcount);
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, qprops.data());
	uint32_t family = 0;
	for (uint32_t i = 0; i < qcount; i++)
	{
		VkBool32 sup = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &sup);
		if (sup && (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
	}
	qci.queueFamilyIndex = family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, chain};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = uint32_t(devExt.size());
	dci.ppEnabledExtensionNames = devExt.data();
	VkDevice device;
	CHECK(vkCreateDevice(phys, &dci, nullptr, &device));
	VkQueue queue;
	vkGetDeviceQueue(device, family, 0, &queue);

#define DEVFN(name) auto name##Fn = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name))
	DEVFN(vkSetSwapchainPresentTimingQueueSizeEXT);
	DEVFN(vkGetSwapchainTimingPropertiesEXT);
	DEVFN(vkGetSwapchainTimeDomainPropertiesEXT);
	DEVFN(vkGetPastPresentationTimingEXT);
	DEVFN(vkGetCalibratedTimestampsKHR);

	// One swapchain per window.
	VkFormat chosenFormat = VK_FORMAT_UNDEFINED;
	for (size_t ci = 0; ci < chains.size(); ci++)
	{
		Chain &c = chains[ci];
		VkPhysicalDeviceSurfaceInfo2KHR sinfo = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
		sinfo.surface = c.surface;
		c.tcaps = {VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT};
		c.id2caps = {VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR};
		VkSurfaceCapabilities2KHR caps2 = {VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
		if (!o.noTiming)
		{
			caps2.pNext = &c.tcaps;
			c.tcaps.pNext = &c.id2caps;
		}
		CHECK(vkGetPhysicalDeviceSurfaceCapabilities2KHR(phys, &sinfo, &caps2));
		std::printf("CAPS chain=%zu timing=%u abs=%u rel=%u stages=0x%x id2=%u minImages=%u\n", ci, c.tcaps.presentTimingSupported,
		            c.tcaps.presentAtAbsoluteTimeSupported, c.tcaps.presentAtRelativeTimeSupported, c.tcaps.presentStageQueries,
		            c.id2caps.presentId2Supported, caps2.surfaceCapabilities.minImageCount);

		VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
		sci.surface = c.surface;
		sci.minImageCount = std::max(caps2.surfaceCapabilities.minImageCount, 3u);
		uint32_t fmtCount = 0;
		vkGetPhysicalDeviceSurfaceFormatsKHR(phys, c.surface, &fmtCount, nullptr);
		std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(phys, c.surface, &fmtCount, fmts.data());
		VkSurfaceFormatKHR fmt = fmts[0];
		for (auto &f : fmts)
			if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) fmt = f;
		sci.imageFormat = fmt.format;
		sci.imageColorSpace = fmt.colorSpace;
		int pw, ph;
		SDL_GetWindowSizeInPixels(c.window, &pw, &ph);
		sci.imageExtent = {uint32_t(pw), uint32_t(ph)};
		sci.imageArrayLayers = 1;
		sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		sci.preTransform = caps2.surfaceCapabilities.currentTransform;
		sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		sci.presentMode = VkPresentModeKHR(o.mode);
		sci.clipped = VK_TRUE;
		c.useTiming = !o.noTiming && c.tcaps.presentTimingSupported;
		if (c.useTiming)
			sci.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
		if (wantV2 && c.id2caps.presentId2Supported)
			sci.flags |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
		c.swapchainFlags = sci.flags;
		CHECK(vkCreateSwapchainKHR(device, &sci, nullptr, &c.swapchain));
		chosenFormat = fmt.format;
		uint32_t imageCount = 0;
		vkGetSwapchainImagesKHR(device, c.swapchain, &imageCount, nullptr);
		c.images.resize(imageCount);
		vkGetSwapchainImagesKHR(device, c.swapchain, &imageCount, c.images.data());

		if (c.useTiming)
		{
			CHECK(vkSetSwapchainPresentTimingQueueSizeEXTFn(device, c.swapchain, o.queue));
			VkSwapchainTimeDomainPropertiesEXT dp = {VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT};
			CHECK(vkGetSwapchainTimeDomainPropertiesEXTFn(device, c.swapchain, &dp, nullptr));
			std::vector<VkTimeDomainKHR> d(dp.timeDomainCount);
			std::vector<uint64_t> ids(dp.timeDomainCount);
			dp.pTimeDomains = d.data();
			dp.pTimeDomainIds = ids.data();
			uint64_t counter = 0;
			CHECK(vkGetSwapchainTimeDomainPropertiesEXTFn(device, c.swapchain, &dp, &counter));
			for (uint32_t i = 0; i < dp.timeDomainCount; i++)
			{
				std::printf("DOMAIN chain=%zu %u id=%llu counter=%llu\n", ci, unsigned(d[i]), (unsigned long long)ids[i], (unsigned long long)counter);
				if (d[i] == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT) c.domainId = ids[i];
			}
			if (!c.domainId && dp.timeDomainCount) c.domainId = ids[0];
			VkSwapchainTimingPropertiesEXT tp = {VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT};
			uint64_t tc = 0;
			CHECK(vkGetSwapchainTimingPropertiesEXTFn(device, c.swapchain, &tp, &tc));
			std::printf("TIMING chain=%zu refreshDuration=%llu refreshInterval=%llu counter=%llu\n", ci,
			            (unsigned long long)tp.refreshDuration, (unsigned long long)tp.refreshInterval, (unsigned long long)tc);

			for (uint32_t stage : {1u, 4u})
			{
				if (!(c.tcaps.presentStageQueries & stage) || ci) continue;
				VkSwapchainCalibratedTimestampInfoEXT local = {VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT};
				local.swapchain = c.swapchain;
				local.presentStage = stage;
				local.timeDomainId = c.domainId;
				VkCalibratedTimestampInfoKHR cal[2] = {{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR},
				                                       {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, &local, VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT}};
				uint64_t ts[2] = {}, dev = 0;
				VkResult cr = vkGetCalibratedTimestampsKHRFn(device, 2, cal, ts, &dev);
				std::printf("CALIB stage=%s rc=%d monotonic=%llu local=%llu diff=%lld deviation=%llu\n", stageName(stage), int(cr),
				            (unsigned long long)ts[0], (unsigned long long)ts[1], (long long)(int64_t(ts[0]) - int64_t(ts[1])),
				            (unsigned long long)dev);
			}
		}

		c.stageMask = c.tcaps.presentStageQueries;
		if (o.stages == "last")
			c.stageMask = c.tcaps.presentStageQueries ? (1u << (31 - __builtin_clz(c.tcaps.presentStageQueries))) : 0;
		else if (o.stages != "all")
			c.stageMask = uint32_t(std::strtoul(o.stages.c_str(), nullptr, 0)) & c.tcaps.presentStageQueries;
	}
	(void)chosenFormat;

	VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpci.queueFamilyIndex = family;
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(device, &cpci, nullptr, &pool));
	VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cbai.commandPool = pool;
	cbai.commandBufferCount = 1;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(device, &cbai, &cb));
	VkSemaphoreCreateInfo semi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	for (Chain &c : chains)
	{
		CHECK(vkCreateSemaphore(device, &semi, nullptr, &c.acquireSem));
		c.doneSem.resize(c.images.size());
		for (auto &s : c.doneSem) CHECK(vkCreateSemaphore(device, &semi, nullptr, &s));
	}
	VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence;
	CHECK(vkCreateFence(device, &fi, nullptr, &fence));

	auto poll = [&](Chain &c, size_t ci, int frame) {
		if (!c.useTiming) return;
		std::vector<VkPastPresentationTimingEXT> reports(o.queue);
		std::vector<VkPresentStageTimeEXT> stagesStore(o.queue * 4);
		for (uint32_t i = 0; i < o.queue; i++)
		{
			reports[i] = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT};
			reports[i].pPresentStages = &stagesStore[i * 4];
			reports[i].presentStageCount = 4;
		}
		VkPastPresentationTimingInfoEXT pi = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT};
		pi.swapchain = c.swapchain;
		if (o.partial) pi.flags |= VK_PAST_PRESENTATION_TIMING_ALLOW_PARTIAL_RESULTS_BIT_EXT;
		if (o.ooo) pi.flags |= VK_PAST_PRESENTATION_TIMING_ALLOW_OUT_OF_ORDER_RESULTS_BIT_EXT;
		VkPastPresentationTimingPropertiesEXT pp = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT};
		pp.presentationTimingCount = o.queue;
		pp.pPresentationTimings = reports.data();
		VkResult r = vkGetPastPresentationTimingEXTFn(device, &pi, &pp);
		const uint64_t now = nowNs();
		if (!o.quiet)
			std::printf("POLL chain=%zu frame=%d rc=%d n=%u counters=%llu/%llu\n", ci, frame, int(r), pp.presentationTimingCount,
			            (unsigned long long)pp.timingPropertiesCounter, (unsigned long long)pp.timeDomainsCounter);
		if (pp.timingPropertiesCounter != c.timingCounterSeen)
		{
			// The driver's refresh cycle changed (the surface entered an output, VRR switched, a mode change): the
			// properties are only valid once the surface is on a screen, so read them again.
			c.timingCounterSeen = pp.timingPropertiesCounter;
			VkSwapchainTimingPropertiesEXT tp = {VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT};
			uint64_t tc = 0;
			const VkResult tr = vkGetSwapchainTimingPropertiesEXTFn(device, c.swapchain, &tp, &tc);
			std::printf("TIMING chain=%zu frame=%d rc=%d refreshDuration=%llu refreshInterval=%llu counter=%llu\n", ci, frame, int(tr),
			            (unsigned long long)tp.refreshDuration, (unsigned long long)tp.refreshInterval, (unsigned long long)tc);
		}
		for (uint32_t i = 0; i < pp.presentationTimingCount; i++)
		{
			const auto &rep = reports[i];
			c.reports++;
			bool fpo = false;
			std::string st;
			for (uint32_t k = 0; k < rep.presentStageCount; k++)
			{
				const auto &s = rep.pPresentStages[k];
				char buf[96];
				std::snprintf(buf, sizeof buf, " %s=%lld", stageName(s.stage), s.time ? (long long)(int64_t(s.time) - int64_t(now)) : 0ll);
				st += buf;
				if (s.stage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT && s.time) fpo = true;
			}
			if (rep.reportComplete) { c.complete++; if (fpo) c.withFpo++; else c.noFpoComplete++; }
			else c.partialSeen++;
			if (!o.quiet)
				std::printf("REPORT chain=%zu id=%llu complete=%u domain=%u/%llu nstages=%u%s (ns relative to poll time)\n", ci,
				            (unsigned long long)rep.presentId, rep.reportComplete, unsigned(rep.timeDomain),
				            (unsigned long long)rep.timeDomainId, rep.presentStageCount, st.c_str());
		}
	};

	std::vector<double> frameMs;
	uint64_t lastPresent = 0;
	const size_t n_chains = chains.size();
	for (int frame = 0; frame < o.frames; frame++)
	{
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {}
		// Render into every window, then present all swapchains with ONE vkQueuePresentKHR.
		std::vector<VkSwapchainKHR> scs;
		std::vector<uint32_t> idx;
		std::vector<VkSemaphore> waits;
		std::vector<uint64_t> ids(n_chains);
		std::vector<VkPresentTimingInfoEXT> timingInfos(n_chains, VkPresentTimingInfoEXT{VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT});
		bool anyTiming = false;
		for (size_t ci = 0; ci < n_chains; ci++)
		{
			Chain &c = chains[ci];
			VkResult ar = vkAcquireNextImageKHR(device, c.swapchain, UINT64_MAX, c.acquireSem, VK_NULL_HANDLE, &c.imageIndex);
			if (ar < 0) { std::fprintf(stderr, "acquire failed %d\n", int(ar)); frame = o.frames; break; }
			vkResetCommandBuffer(cb, 0);
			VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			vkBeginCommandBuffer(cb, &bi);
			VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = c.images[c.imageIndex];
			b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
			float t = float(frame % 120) / 120.0f;
			VkClearColorValue clear = {{t, 0.2f + 0.3f * float(ci), 1.0f - t, 1.0f}};
			vkCmdClearColorImage(cb, c.images[c.imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &b.subresourceRange);
			b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b.dstAccessMask = 0;
			b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
			vkEndCommandBuffer(cb);
			VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
			si.waitSemaphoreCount = 1;
			si.pWaitSemaphores = &c.acquireSem;
			si.pWaitDstStageMask = &waitStage;
			si.commandBufferCount = 1;
			si.pCommandBuffers = &cb;
			si.signalSemaphoreCount = 1;
			si.pSignalSemaphores = &c.doneSem[c.imageIndex];
			vkResetFences(device, 1, &fence);
			CHECK(vkQueueSubmit(queue, 1, &si, fence));
			vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
			scs.push_back(c.swapchain);
			idx.push_back(c.imageIndex);
			waits.push_back(c.doneSem[c.imageIndex]);
			ids[ci] = ++c.idCounter;
			timingInfos[ci].presentStageQueries = c.useTiming ? c.stageMask : 0;
			timingInfos[ci].timeDomainId = c.domainId;
			anyTiming = anyTiming || (c.useTiming && c.stageMask);
		}
		if (scs.size() != n_chains)
			break;

		VkPresentInfoKHR pinfo = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		pinfo.waitSemaphoreCount = uint32_t(waits.size());
		pinfo.pWaitSemaphores = waits.data();
		pinfo.swapchainCount = uint32_t(scs.size());
		pinfo.pSwapchains = scs.data();
		pinfo.pImageIndices = idx.data();
		std::vector<VkResult> results(n_chains);
		pinfo.pResults = results.data();
		VkPresentIdKHR pid1 = {VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
		pid1.swapchainCount = uint32_t(n_chains);
		pid1.pPresentIds = ids.data();
		VkPresentId2KHR pid2 = {VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR};
		pid2.swapchainCount = uint32_t(n_chains);
		pid2.pPresentIds = ids.data();
		VkPresentTimingsInfoEXT timings = {VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT};
		timings.swapchainCount = uint32_t(n_chains);
		timings.pTimingInfos = timingInfos.data();
		if (wantV1) { pid1.pNext = pinfo.pNext; pinfo.pNext = &pid1; }
		if (wantV2 && (chains[0].swapchainFlags & VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR)) { pid2.pNext = pinfo.pNext; pinfo.pNext = &pid2; }
		if (anyTiming) { timings.pNext = pinfo.pNext; pinfo.pNext = &timings; }
		const uint64_t t0 = nowNs();
		VkResult pr = vkQueuePresentKHR(queue, &pinfo);
		const uint64_t t1 = nowNs();
		for (size_t ci = 0; ci < n_chains; ci++)
			if (results[ci] < 0 && results[ci] != VK_ERROR_OUT_OF_DATE_KHR)
				std::printf("PRESENT chain=%zu frame=%d id=%llu result=%d\n", ci, frame, (unsigned long long)ids[ci], int(results[ci]));
		if (lastPresent) frameMs.push_back(double(t0 - lastPresent) / 1e6);
		lastPresent = t0;
		if (!o.quiet && frame < 4)
			std::printf("PRESENT frame=%d ret=%d in_present_us=%.1f\n", frame, int(pr), double(t1 - t0) / 1e3);
		if (o.pollEvery > 0 && frame % o.pollEvery == 0)
			for (size_t ci = 0; ci < n_chains; ci++)
				poll(chains[ci], ci, frame);
		if (o.sleepUs)
			std::this_thread::sleep_for(std::chrono::microseconds(o.sleepUs));
	}
	for (int i = 0; i < 20; i++)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		for (size_t ci = 0; ci < n_chains; ci++)
			poll(chains[ci], ci, -1);
	}
	vkDeviceWaitIdle(device);
	double sum = 0;
	for (double m : frameMs) sum += m;
	for (size_t ci = 0; ci < n_chains; ci++)
	{
		const Chain &c = chains[ci];
		std::printf("SUMMARY chain=%zu mode=%d frames=%d reports=%d complete=%d withFPO=%d completeNoFPO=%d incomplete=%d avgPresentIntervalMs=%.3f\n",
		            ci, o.mode, o.frames, c.reports, c.complete, c.withFpo, c.noFpoComplete, c.partialSeen,
		            frameMs.empty() ? 0.0 : sum / double(frameMs.size()));
	}
	if (o.noCleanup)
	{
		// Exit with every Vulkan object alive: the layer's atexit handler must still unlink its segment.
		std::fflush(stdout);
		std::exit(0);
	}
	for (Chain &c : chains)
	{
		vkDestroySwapchainKHR(device, c.swapchain, nullptr);
		vkDestroySurfaceKHR(instance, c.surface, nullptr);
		SDL_DestroyWindow(c.window);
	}
	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(instance, nullptr);
	SDL_Quit();
	return 0;
}
