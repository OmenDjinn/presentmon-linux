// SPDX-License-Identifier: MIT
//
// vk_truth: a native Vulkan test application whose frame timing is known.
//
// End-to-end tests compare what the layer, CLI and overlay report against what this app
// knows about itself: when it slept, spun the CPU, ran its GPU work and presented. Optional
// "DXVK-like" modes (present_id2 + present_wait2 like DXVK 3.x, present_id + present_wait like
// DXVK 2.x and vkd3d-proton, VK_EXT_present_timing polled every frame) check that the layer
// shares present ids, present waits and present timing with the application without disturbing them.

#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <pml/clock.hpp>

#include "spv_fullscreen_vert.h"
#include "spv_load_comp.h"
#include "spv_pattern_frag.h"

using pml::monotonicNs;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitVulkan = 2;
constexpr int kExitAnomalies = 3;

// Thrown for any unrecoverable setup/runtime error; caught in main, never crosses an API boundary.
struct Fatal
{
	std::string msg;
	int code;
};

[[noreturn]] void fatal(int code, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	throw Fatal{ buf, code };
}

#define VK_CHECK(expr) \
	do { \
		VkResult vk_check_r_ = (expr); \
		if (vk_check_r_ < 0) fatal(kExitVulkan, "%s failed: VkResult %d (%s:%d)", #expr, int(vk_check_r_), __FILE__, __LINE__); \
	} while (0)

std::atomic<int> g_signal{ 0 };
void onSignal(int sig) { g_signal.store(sig); }

// A driver call that never returns would hang a test run forever. Every potentially blocking
// call records where it is; a watchdog thread reports a stall and exits with the Vulkan-error code.
std::atomic<const char *> g_where{ "startup" };
std::atomic<uint64_t> g_whereSince{ 0 };
inline void at(const char *where)
{
	g_where.store(where, std::memory_order_relaxed);
	g_whereSince.store(monotonicNs(), std::memory_order_relaxed);
}

void watchdogMain(double limitSeconds)
{
	for (;;) {
		struct timespec ts{ 0, 250'000'000 };
		nanosleep(&ts, nullptr);
		uint64_t since = g_whereSince.load(std::memory_order_relaxed);
		if (since && double(monotonicNs() - since) > limitSeconds * 1e9) {
			fprintf(stderr, "[vk_truth] WATCHDOG: no progress for %.0f s, stuck in \"%s\"; aborting (exit %d)\n", limitSeconds,
			        g_where.load(), kExitVulkan);
			fflush(stdout);
			_exit(kExitVulkan);
		}
	}
}

// ---------------------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------------------

struct Options
{
	double seconds = 0;
	uint64_t frames = 0;
	std::string presentMode = "fifo";
	double fps = 0;
	double cpuMs = 0;
	double gpuMs = 0;
	uint32_t width = 1280, height = 720;
	bool fullscreen = false;
	std::string display;
	bool listDisplays = false;
	std::string format = "srgb";
	std::string engineName;
	uint32_t images = 3;
	bool appPresentId = false;
	bool presentIdV1 = false;    // --app-present-id v1: VK_KHR_present_id + VK_KHR_present_wait
	bool appTiming = false;
	bool inputReactive = false;
	std::string logPath;
	std::string wsi = "wayland";
	uint32_t frameLatency = 2;
	uint32_t timingQueue = 64;
	std::string timingStage = "auto";
	std::string timeDomain = "auto";
	double watchdog = 10;
	bool resizeTest = false;
	bool help = false;
};

void printHelp()
{
	puts(
		"vk_truth: Vulkan test app with known frame timing (presentmon-linux ground truth)\n"
		"\n"
		"usage: vk_truth [options]\n"
		"\n"
		"run length\n"
		"  --seconds N          exit after N seconds of rendering (default: run until closed)\n"
		"  --frames N           exit after N frames\n"
		"pacing and load\n"
		"  --present-mode M     fifo | fifo_relaxed | mailbox | immediate | fifo_latest_ready (default fifo)\n"
		"  --fps N              frame cap against an absolute schedule; 0 = uncapped (default 0)\n"
		"  --cpu-ms X           busy-spin X ms per frame on the render thread before submitting\n"
		"  --gpu-ms X           GPU compute load of X ms per frame, calibrated at startup\n"
		"window and swapchain\n"
		"  --size WxH           window size (default 1280x720)\n"
		"  --fullscreen         fullscreen on the chosen display\n"
		"  --display D          display index or name (substring); see --list-displays\n"
		"  --list-displays      print SDL displays and exit\n"
		"  --wsi wayland|x11    window system backend (default wayland; x11 = Xwayland)\n"
		"  --format F           srgb | unorm | hdr10 | scrgb (default srgb); fails if unavailable\n"
		"                         srgb  B8G8R8A8_SRGB / SRGB_NONLINEAR\n"
		"                         unorm B8G8R8A8_UNORM / SRGB_NONLINEAR\n"
		"                         hdr10 A2B10G10R10_UNORM_PACK32 / HDR10_ST2084 (white at 200 nits)\n"
		"                         scrgb R16G16B16A16_SFLOAT / EXTENDED_SRGB_LINEAR (1.0 = 80 nits)\n"
		"  --images N           requested swapchain image count (default 3)\n"
		"  --engine-name NAME   VkApplicationInfo.pEngineName (e.g. DXVK)\n"
		"  --frame-latency N    CPU frames in flight, and the present-wait distance (default 2)\n"
		"DXVK-like modes\n"
		"  --app-present-id [v1|v2]\n"
		"                       chain increasing present IDs and keep at most --frame-latency presents\n"
		"                       outstanding by waiting for them (FIFO modes only, as DXVK does)\n"
		"                         v2 (default) present_id2 + present_wait2: VkPresentId2KHR and\n"
		"                            vkWaitForPresent2KHR, PRESENT_ID_2/PRESENT_WAIT_2 swapchain (DXVK 3.x)\n"
		"                         v1 present_id + present_wait: VkPresentIdKHR and vkWaitForPresentKHR,\n"
		"                            no swapchain flags (DXVK 2.x, vkd3d-proton)\n"
		"  --app-timing         VK_EXT_present_timing: one-stage timing request on every present,\n"
		"                       polled every frame like DXVK, every report validated. Implies\n"
		"                       present_id2 IDs (without waiting unless --app-present-id); not with\n"
		"                       --app-present-id v1 (DXVK only times presents that carry present_id2 IDs).\n"
		"  --timing-queue N     present-timing queue size set on the swapchain (1..1024, default 64).\n"
		"                       On VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT the present is not retried\n"
		"                       (its waits already ran): the size doubles and the swapchain is recreated,\n"
		"                       as DXVK does\n"
		"  --timing-stage S     auto | queue | pixel-out (default auto: FIRST_PIXEL_OUT if the\n"
		"                       surface reports it, else QUEUE_OPERATIONS_END)\n"
		"  --time-domain D      auto | monotonic | stage | swapchain (time domain requested for\n"
		"                       --app-timing; auto prefers CLOCK_MONOTONIC)\n"
		"input\n"
		"  --input-reactive     shift the pattern by relative mouse motion, with no smoothing\n"
		"                       (also captures the pointer while the window is focused)\n"
		"  --resize-test        resize the window every 0.5 s (exercises swapchain recreation; windowed only)\n"
		"output and safety\n"
		"  --watchdog S         abort with exit 2 if a driver call blocks for S seconds (default 10; 0 = off)\n"
		"  --log FILE           per-frame ground-truth CSV (CLOCK_MONOTONIC ns)\n"
		"  --help               this text\n"
		"\n"
		"Esc or q quits. SIGINT/SIGTERM exit cleanly.\n"
		"exit codes: 0 ok, 1 usage error, 2 Vulkan/setup error, 3 --app-timing saw anomalies");
}

bool parseArgs(int argc, char **argv, Options &o)
{
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		std::string val;
		bool hasVal = false;
		auto eq = a.find('=');
		if (a.rfind("--", 0) == 0 && eq != std::string::npos) {
			val = a.substr(eq + 1);
			a = a.substr(0, eq);
			hasVal = true;
		}
		auto need = [&]() -> const char * {
			if (hasVal) return val.c_str();
			if (i + 1 >= argc) {
				fprintf(stderr, "vk_truth: %s needs a value\n", a.c_str());
				return nullptr;
			}
			return argv[++i];
		};
		const char *v = nullptr;
		auto num = [&](double &out) {
			v = need();
			if (!v) return false;
			char *end = nullptr;
			out = strtod(v, &end);
			if (end == v || *end || out < 0) {
				fprintf(stderr, "vk_truth: bad number for %s: %s\n", a.c_str(), v);
				return false;
			}
			return true;
		};
		double d = 0;
		if (a == "--help" || a == "-h") o.help = true;
		else if (a == "--seconds") { if (!num(o.seconds)) return false; }
		else if (a == "--frames") { if (!num(d)) return false; o.frames = uint64_t(d); }
		else if (a == "--fps") { if (!num(o.fps)) return false; }
		else if (a == "--cpu-ms") { if (!num(o.cpuMs)) return false; }
		else if (a == "--gpu-ms") { if (!num(o.gpuMs)) return false; }
		else if (a == "--images") { if (!num(d)) return false; o.images = uint32_t(d); }
		else if (a == "--watchdog") { if (!num(o.watchdog)) return false; }
		else if (a == "--timing-queue") { if (!num(d) || d < 1 || d > 1024) return false; o.timingQueue = uint32_t(d); }
		else if (a == "--frame-latency") { if (!num(d) || d < 1) return false; o.frameLatency = uint32_t(d); }
		else if (a == "--present-mode") { if (!(v = need())) return false; o.presentMode = v; }
		else if (a == "--format") { if (!(v = need())) return false; o.format = v; }
		else if (a == "--engine-name") { if (!(v = need())) return false; o.engineName = v; }
		else if (a == "--display") { if (!(v = need())) return false; o.display = v; }
		else if (a == "--wsi") { if (!(v = need())) return false; o.wsi = v; }
		else if (a == "--log") { if (!(v = need())) return false; o.logPath = v; }
		else if (a == "--timing-stage") { if (!(v = need())) return false; o.timingStage = v; }
		else if (a == "--time-domain") { if (!(v = need())) return false; o.timeDomain = v; }
		else if (a == "--size") {
			if (!(v = need())) return false;
			unsigned w = 0, h = 0;
			if (sscanf(v, "%ux%u", &w, &h) != 2 || !w || !h) {
				fprintf(stderr, "vk_truth: --size expects WxH\n");
				return false;
			}
			o.width = w;
			o.height = h;
		}
		else if (a == "--fullscreen") o.fullscreen = true;
		else if (a == "--resize-test") o.resizeTest = true;
		else if (a == "--list-displays") o.listDisplays = true;
		else if (a == "--app-present-id") {
			// Optional version: "--app-present-id=v1" or "--app-present-id v1"; bare = v2 (as before).
			o.appPresentId = true;
			if (!hasVal && i + 1 < argc && (!strcmp(argv[i + 1], "v1") || !strcmp(argv[i + 1], "v2"))) {
				val = argv[++i];
				hasVal = true;
			}
			if (hasVal && val != "v1" && val != "v2") {
				fprintf(stderr, "vk_truth: --app-present-id takes v1 or v2, not %s\n", val.c_str());
				return false;
			}
			o.presentIdV1 = hasVal && val == "v1";
		}
		else if (a == "--app-timing") o.appTiming = true;
		else if (a == "--input-reactive") o.inputReactive = true;
		else {
			fprintf(stderr, "vk_truth: unknown option %s (see --help)\n", a.c_str());
			return false;
		}
	}
	auto oneOf = [](const std::string &s, std::initializer_list<const char *> l) {
		for (auto *e : l) if (s == e) return true;
		return false;
	};
	if (!oneOf(o.presentMode, { "fifo", "fifo_relaxed", "mailbox", "immediate", "fifo_latest_ready" }) ||
	    !oneOf(o.format, { "srgb", "unorm", "hdr10", "scrgb" }) ||
	    !oneOf(o.wsi, { "wayland", "x11" }) ||
	    !oneOf(o.timingStage, { "auto", "queue", "pixel-out" }) ||
	    !oneOf(o.timeDomain, { "auto", "monotonic", "stage", "swapchain" })) {
		fprintf(stderr, "vk_truth: invalid option value (see --help)\n");
		return false;
	}
	if (o.appTiming && o.presentIdV1) {
		fprintf(stderr, "vk_truth: --app-timing uses present_id2 IDs; it cannot be combined with --app-present-id v1\n");
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------------------
// Per-frame ground truth
// ---------------------------------------------------------------------------------------

struct Row
{
	uint64_t frame = 0;
	uint64_t presentId = 0;      // 0 if the app does not chain present ids
	uint64_t sleepStart = 0, sleepEnd = 0;
	uint64_t cpuStart = 0, cpuEnd = 0;
	uint64_t presentPre = 0, presentPost = 0;
	uint64_t gpuBegin = 0, gpuEnd = 0;
	uint64_t stageTime = 0;      // CLOCK_MONOTONIC ns of the requested stage, 0 if none
	uint32_t stage = 0;          // VkPresentStageFlagBitsEXT of that time
	int32_t presentResult = 0;
	enum Timing : uint8_t { None, Pending, Reported, Abandoned } timing = None;
};

// ---------------------------------------------------------------------------------------
// The application
// ---------------------------------------------------------------------------------------

struct GfxPush
{
	float res[2];
	float offset[2];
	uint32_t frame;
	uint32_t mode;
	float time;
};

struct Slot
{
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	VkSemaphore acquire = VK_NULL_HANDLE;
	VkQueryPool queries = VK_NULL_HANDLE;
	int64_t frame = -1; // frame whose GPU timestamps are still to be harvested
};

class App
{
public:
	explicit App(const Options &o) : opt(o) {}
	int run();

private:
	Options opt;

	// SDL
	SDL_Window *window = nullptr;
	bool quit = false;
	bool minimized = false;
	bool needRecreate = false;
	float mouseDx = 0, mouseDy = 0;

	// Vulkan core
	VkInstance instance = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;
	VkPhysicalDeviceProperties physProps{};
	uint32_t timestampValidBits = 64;
	double timestampPeriodNs = 1.0;

	// Extension entry points
	PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR pfnGetSurfaceCaps2 = nullptr;
	PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR pfnGetCalibrateableDomains = nullptr;
	PFN_vkGetCalibratedTimestampsKHR pfnGetCalibrated = nullptr;
	PFN_vkWaitForPresent2KHR pfnWaitForPresent2 = nullptr;
	PFN_vkWaitForPresentKHR pfnWaitForPresent = nullptr;
	PFN_vkSetSwapchainPresentTimingQueueSizeEXT pfnSetTimingQueueSize = nullptr;
	PFN_vkGetPastPresentationTimingEXT pfnGetPastTiming = nullptr;
	PFN_vkGetSwapchainTimeDomainPropertiesEXT pfnGetTimeDomains = nullptr;

	// Capabilities that were actually enabled
	bool hasPresentId2 = false, hasPresentWait2 = false, hasPresentTiming = false;
	bool hasPresentId1 = false, hasPresentWait1 = false;  // --app-present-id v1
	bool surfPresentId2 = false, surfPresentWait2 = false, surfPresentTiming = false;
	VkPresentStageFlagsEXT surfStages = 0;

	// Swapchain
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat swapFormat = VK_FORMAT_UNDEFINED;
	VkColorSpaceKHR swapColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
	VkExtent2D extent{};
	std::vector<VkImage> images;
	std::vector<VkImageView> views;
	std::vector<VkSemaphore> renderDone;
	uint64_t swapchainFirstId = 0; // first present id used on the current swapchain
	bool swapUsesPresentId = false, swapUsesWait2 = false, swapUsesTiming = false;
	bool swapUsesWait1 = false;

	// Timing state for --app-timing
	VkPresentStageFlagBitsEXT timingStage = VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT;
	uint64_t timingDomainId = 0;
	VkTimeDomainKHR timingDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
	uint64_t timeDomainsCounter = 0;
	static constexpr uint32_t kMaxTimingQueue = 1024;
	uint64_t lastReportedId = 0;
	uint64_t lastReportedMono = 0;
	uint64_t timingReports = 0;
	uint64_t anomalies = 0;
	std::map<std::string, uint64_t> anomalyKinds;
	uint32_t timingQueueSize = 0;   // current queue size: --timing-queue, doubled on every QUEUE_FULL (as DXVK does)
	uint64_t queueFull = 0;         // presents rejected with VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT
	uint64_t queueFullSpurious = 0; // ... while fewer than timingQueueSize of our requests were outstanding
	uint64_t notDisplayed = 0;
	uint32_t recreations = 0;
	uint64_t presentWaitTimeouts = 0;
	uint64_t presentWaits = 0, presentWaitNsSum = 0, presentWaitNsMax = 0;

	// Rendering objects
	VkCommandPool cmdPool = VK_NULL_HANDLE;
	std::vector<Slot> slots;
	VkPipelineLayout gfxLayout = VK_NULL_HANDLE;
	VkPipeline gfxPipeline = VK_NULL_HANDLE;
	VkDescriptorSetLayout loadSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool descPool = VK_NULL_HANDLE;
	VkDescriptorSet loadSet = VK_NULL_HANDLE;
	VkPipelineLayout loadLayout = VK_NULL_HANDLE;
	VkPipeline loadPipeline = VK_NULL_HANDLE;
	VkBuffer loadBuf = VK_NULL_HANDLE;
	VkDeviceMemory loadMem = VK_NULL_HANDLE;
	static constexpr uint32_t kLoadGroups = 2048;
	uint32_t loadIters = 0;
	uint32_t shaderMode = 0;

	// GPU timestamp -> CLOCK_MONOTONIC
	uint64_t calDev = 0, calMono = 0, calAt = 0;

	std::vector<Row> rows;

	// ---- setup
	void initSdl();
	void listDisplays();
	void createInstance();
	void pickDevice();
	void createDevice();
	void queryCapabilities();
	void createFrameObjects();
	void createPipelines();
	void calibrateLoad();
	double measureLoadMs(uint32_t iters, int runs, double *minMs = nullptr, double *maxMs = nullptr);
	bool createSwapchain();
	void destroySwapchainViews();
	void pickTimingDomain();
	uint32_t findMemoryType(uint32_t bits, VkMemoryPropertyFlags want);
	VkShaderModule makeShader(const uint32_t *code, size_t bytes);

	// ---- per frame
	void pumpEvents();
	void sleepUntil(uint64_t target);
	void recordFrame(Slot &s, uint32_t imageIndex, uint64_t frame);
	void harvest(Slot &s);
	void recalibrateGpuClock(bool force);
	uint64_t gpuToMono(uint64_t ticks);
	void pollTiming(bool drain);
	uint64_t toMono(VkTimeDomainKHR domain, uint64_t domainId, VkPresentStageFlagsEXT stage, uint64_t t);
	void anomaly(const char *kind, const char *fmt, ...);
	void abandonPending();
	void waitForPresentDistance(uint64_t presentId);
	void recreateSwapchain();

	void writeLog();
	void summary(double wallSeconds);

public:
	void cleanup();
	int exitCode() const { return anomalies ? kExitAnomalies : kExitOk; }
	bool timingEnabled() const { return opt.appTiming; }
};

void App::anomaly(const char *kind, const char *fmt, ...)
{
	anomalies++;
	anomalyKinds[kind]++;
	if (anomalies <= 10) {
		char buf[512];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		fprintf(stderr, "[vk_truth] ANOMALY (%s): %s\n", kind, buf);
	}
}

// ---------------------------------------------------------------------------------------
// SDL
// ---------------------------------------------------------------------------------------

void App::initSdl()
{
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1"); // our SIGINT/SIGTERM handlers must win
	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, opt.wsi.c_str());
	if (!SDL_Init(SDL_INIT_VIDEO))
		fatal(kExitVulkan, "SDL_Init(video, driver=%s) failed: %s", opt.wsi.c_str(), SDL_GetError());
	if (!SDL_Vulkan_LoadLibrary(nullptr))
		fatal(kExitVulkan, "SDL_Vulkan_LoadLibrary failed: %s", SDL_GetError());
}

void App::listDisplays()
{
	int n = 0;
	SDL_DisplayID *ids = SDL_GetDisplays(&n);
	printf("SDL video driver: %s, %d display(s)\n", SDL_GetCurrentVideoDriver(), n);
	for (int i = 0; i < n; i++) {
		SDL_Rect r{};
		SDL_GetDisplayBounds(ids[i], &r);
		const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(ids[i]);
		bool hdr = SDL_GetBooleanProperty(SDL_GetDisplayProperties(ids[i]), SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
		printf("  [%d] id=%u name=\"%s\" bounds=%dx%d+%d+%d mode=%dx%d@%.2fHz hdr=%s\n", i, ids[i],
		       SDL_GetDisplayName(ids[i]), r.w, r.h, r.x, r.y, m ? m->w : 0, m ? m->h : 0,
		       m ? double(m->refresh_rate) : 0.0, hdr ? "on" : "off");
	}
	SDL_free(ids);
}

void App::pumpEvents()
{
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_EVENT_QUIT:
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			quit = true;
			break;
		case SDL_EVENT_KEY_DOWN:
			if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_Q)
				quit = true;
			break;
		case SDL_EVENT_WINDOW_MINIMIZED:
		case SDL_EVENT_WINDOW_HIDDEN:
			minimized = true;
			break;
		case SDL_EVENT_WINDOW_RESTORED:
		case SDL_EVENT_WINDOW_SHOWN:
			minimized = false;
			break;
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
			needRecreate = true;
			break;
		case SDL_EVENT_MOUSE_MOTION:
			mouseDx += e.motion.xrel;
			mouseDy += e.motion.yrel;
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			if (opt.inputReactive)
				SDL_SetWindowRelativeMouseMode(window, true);
			break;
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			if (opt.inputReactive)
				SDL_SetWindowRelativeMouseMode(window, false);
			break;
		default:
			break;
		}
	}
	if (g_signal.load())
		quit = true;
}

// Sleeps against an absolute CLOCK_MONOTONIC target: clock_nanosleep up to ~0.5 ms before it,
// then spins. Long waits are chopped so the window keeps answering the compositor.
void App::sleepUntil(uint64_t target)
{
	constexpr uint64_t kSpinNs = 500'000;
	for (;;) {
		uint64_t now = monotonicNs();
		if (now >= target || quit)
			return;
		uint64_t rem = target - now;
		if (rem > kSpinNs) {
			uint64_t until = rem > 25'000'000 ? now + 20'000'000 : target - kSpinNs;
			timespec ts{};
			ts.tv_sec = time_t(until / 1'000'000'000ull);
			ts.tv_nsec = long(until % 1'000'000'000ull);
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
			if (rem > 25'000'000)
				pumpEvents();
		}
		// else: spin
	}
}

// ---------------------------------------------------------------------------------------
// Vulkan setup
// ---------------------------------------------------------------------------------------

static bool hasExt(const std::vector<VkExtensionProperties> &v, const char *name)
{
	for (auto &e : v)
		if (!strcmp(e.extensionName, name))
			return true;
	return false;
}

void App::createInstance()
{
	uint32_t n = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> avail(n);
	vkEnumerateInstanceExtensionProperties(nullptr, &n, avail.data());

	uint32_t sdlCount = 0;
	const char *const *sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlCount);
	if (!sdlExts)
		fatal(kExitVulkan, "SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
	std::vector<const char *> exts(sdlExts, sdlExts + sdlCount);
	auto addIfAvail = [&](const char *name) {
		if (hasExt(avail, name) && std::find_if(exts.begin(), exts.end(), [&](const char *e) { return !strcmp(e, name); }) == exts.end())
			exts.push_back(name);
	};
	addIfAvail(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
	addIfAvail(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);

	VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = "vk_truth";
	app.applicationVersion = 1;
	app.pEngineName = opt.engineName.empty() ? "vk_truth" : opt.engineName.c_str();
	app.engineVersion = 1;
	app.apiVersion = VK_API_VERSION_1_3;

	VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	ci.pApplicationInfo = &app;
	ci.enabledExtensionCount = uint32_t(exts.size());
	ci.ppEnabledExtensionNames = exts.data();
	VK_CHECK(vkCreateInstance(&ci, nullptr, &instance));

	pfnGetSurfaceCaps2 = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
	pfnGetCalibrateableDomains = (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR");
	if (!pfnGetCalibrateableDomains)
		pfnGetCalibrateableDomains = (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
}

void App::pickDevice()
{
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(instance, &n, nullptr);
	std::vector<VkPhysicalDevice> devs(n);
	vkEnumeratePhysicalDevices(instance, &n, devs.data());

	int bestScore = -1;
	for (auto d : devs) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(d, &p);
		if (p.apiVersion < VK_API_VERSION_1_3)
			continue;
		uint32_t qn = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
		std::vector<VkQueueFamilyProperties> qf(qn);
		vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qf.data());
		for (uint32_t i = 0; i < qn; i++) {
			VkBool32 present = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface, &present);
			if (!present || !(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !(qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
				continue;
			int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 10;
			if (score > bestScore) {
				bestScore = score;
				phys = d;
				queueFamily = i;
				physProps = p;
				timestampValidBits = qf[i].timestampValidBits;
			}
			break;
		}
	}
	if (!phys)
		fatal(kExitVulkan, "no Vulkan 1.3 device with a graphics+compute+present queue found");
	if (!timestampValidBits)
		fatal(kExitVulkan, "queue family does not support timestamps");
	timestampPeriodNs = double(physProps.limits.timestampPeriod);
}

void App::createDevice()
{
	uint32_t n = 0;
	vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> avail(n);
	vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, avail.data());

	std::vector<const char *> exts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	if (!hasExt(avail, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME))
		fatal(kExitVulkan, "VK_KHR_calibrated_timestamps is required but not supported");
	exts.push_back(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);

	if (opt.appPresentId && opt.presentIdV1) {
		// DXVK 2.x / vkd3d-proton: VkPresentIdKHR ids and vkWaitForPresentKHR, nothing of present_id2.
		if (!hasExt(avail, VK_KHR_PRESENT_ID_EXTENSION_NAME))
			fatal(kExitVulkan, "--app-present-id v1: VK_KHR_present_id not supported");
		if (!hasExt(avail, VK_KHR_PRESENT_WAIT_EXTENSION_NAME))
			fatal(kExitVulkan, "--app-present-id v1: VK_KHR_present_wait not supported");
		exts.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
		exts.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
		hasPresentId1 = hasPresentWait1 = true;
	}
	const bool wantId2 = (opt.appPresentId && !opt.presentIdV1) || opt.appTiming;
	if (wantId2) {
		if (!hasExt(avail, VK_KHR_PRESENT_ID_2_EXTENSION_NAME))
			fatal(kExitVulkan, "--app-present-id/--app-timing: VK_KHR_present_id2 not supported");
		exts.push_back(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
		hasPresentId2 = true;
	}
	if (opt.appPresentId && !opt.presentIdV1) {
		if (!hasExt(avail, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME))
			fatal(kExitVulkan, "--app-present-id: VK_KHR_present_wait2 not supported");
		exts.push_back(VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME);
		hasPresentWait2 = true;
	}
	if (opt.appTiming) {
		if (!hasExt(avail, VK_EXT_PRESENT_TIMING_EXTENSION_NAME))
			fatal(kExitVulkan, "--app-timing: VK_EXT_present_timing not supported");
		exts.push_back(VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
		hasPresentTiming = true;
	}
	bool latestReady = false;
	bool latestReadyKhr = hasExt(avail, VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
	bool latestReadyExt = hasExt(avail, VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
	if (opt.presentMode == "fifo_latest_ready") {
		if (latestReadyKhr) exts.push_back(VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
		else if (latestReadyExt) exts.push_back(VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
		else fatal(kExitVulkan, "--present-mode fifo_latest_ready: extension not supported by this device");
		latestReady = true;
	}

	VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	f13.dynamicRendering = VK_TRUE;
	f13.synchronization2 = VK_TRUE;
	void *chain = &f13;
	VkPhysicalDevicePresentId2FeaturesKHR fId2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR };
	VkPhysicalDevicePresentWait2FeaturesKHR fWait2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR };
	VkPhysicalDevicePresentTimingFeaturesEXT fTiming{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT };
	VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR fLatest{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR };
	VkPhysicalDevicePresentIdFeaturesKHR fId1{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR };
	VkPhysicalDevicePresentWaitFeaturesKHR fWait1{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR };
	auto push = [&](auto &s) { s.pNext = chain; chain = &s; };
	if (hasPresentId1) { fId1.presentId = VK_TRUE; push(fId1); }
	if (hasPresentWait1) { fWait1.presentWait = VK_TRUE; push(fWait1); }
	if (hasPresentId2) { fId2.presentId2 = VK_TRUE; push(fId2); }
	if (hasPresentWait2) { fWait2.presentWait2 = VK_TRUE; push(fWait2); }
	if (hasPresentTiming) { fTiming.presentTiming = VK_TRUE; push(fTiming); }
	if (latestReady) { fLatest.presentModeFifoLatestReady = VK_TRUE; push(fLatest); }

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	qci.queueFamilyIndex = queueFamily;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;

	VkDeviceCreateInfo ci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	ci.pNext = chain;
	ci.queueCreateInfoCount = 1;
	ci.pQueueCreateInfos = &qci;
	ci.enabledExtensionCount = uint32_t(exts.size());
	ci.ppEnabledExtensionNames = exts.data();
	VK_CHECK(vkCreateDevice(phys, &ci, nullptr, &device));
	vkGetDeviceQueue(device, queueFamily, 0, &queue);

	auto dp = [&](const char *name) { return vkGetDeviceProcAddr(device, name); };
	pfnGetCalibrated = (PFN_vkGetCalibratedTimestampsKHR)dp("vkGetCalibratedTimestampsKHR");
	if (!pfnGetCalibrated)
		pfnGetCalibrated = (PFN_vkGetCalibratedTimestampsKHR)dp("vkGetCalibratedTimestampsEXT");
	if (!pfnGetCalibrated)
		fatal(kExitVulkan, "vkGetCalibratedTimestampsKHR not available");
	pfnWaitForPresent2 = (PFN_vkWaitForPresent2KHR)dp("vkWaitForPresent2KHR");
	pfnWaitForPresent = (PFN_vkWaitForPresentKHR)dp("vkWaitForPresentKHR");
	if (hasPresentWait1 && !pfnWaitForPresent)
		fatal(kExitVulkan, "vkWaitForPresentKHR not available");
	pfnSetTimingQueueSize = (PFN_vkSetSwapchainPresentTimingQueueSizeEXT)dp("vkSetSwapchainPresentTimingQueueSizeEXT");
	pfnGetPastTiming = (PFN_vkGetPastPresentationTimingEXT)dp("vkGetPastPresentationTimingEXT");
	pfnGetTimeDomains = (PFN_vkGetSwapchainTimeDomainPropertiesEXT)dp("vkGetSwapchainTimeDomainPropertiesEXT");

	// The calibration domains we need must exist.
	uint32_t dn = 0;
	pfnGetCalibrateableDomains(phys, &dn, nullptr);
	std::vector<VkTimeDomainKHR> doms(dn);
	pfnGetCalibrateableDomains(phys, &dn, doms.data());
	bool hasDev = false, hasMono = false;
	for (auto d : doms) {
		hasDev |= d == VK_TIME_DOMAIN_DEVICE_KHR;
		hasMono |= d == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
	}
	if (!hasDev || !hasMono)
		fatal(kExitVulkan, "device cannot calibrate DEVICE <-> CLOCK_MONOTONIC timestamps");
}

void App::queryCapabilities()
{
	if (!pfnGetSurfaceCaps2)
		return;
	VkPhysicalDeviceSurfaceInfo2KHR si{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR };
	si.surface = surface;
	VkSurfaceCapabilities2KHR caps{ VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR };
	VkSurfaceCapabilitiesPresentId2KHR id2{ VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR };
	VkSurfaceCapabilitiesPresentWait2KHR w2{ VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR };
	VkPresentTimingSurfaceCapabilitiesEXT tc{ VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT };
	void *chain = nullptr;
	auto push = [&](auto &s) { s.pNext = chain; chain = &s; };
	if (hasPresentId2) push(id2);
	if (hasPresentWait2) push(w2);
	if (hasPresentTiming) push(tc);
	caps.pNext = chain;
	VK_CHECK(pfnGetSurfaceCaps2(phys, &si, &caps));
	surfPresentId2 = id2.presentId2Supported;
	surfPresentWait2 = w2.presentWait2Supported;
	surfPresentTiming = tc.presentTimingSupported;
	surfStages = tc.presentStageQueries;

	if (hasPresentId2 && !surfPresentId2)
		fatal(kExitVulkan, "this surface does not support present_id2");
	if (hasPresentWait2 && !surfPresentWait2)
		fatal(kExitVulkan, "this surface does not support present_wait2");
	if (hasPresentTiming) {
		if (!surfPresentTiming)
			fatal(kExitVulkan, "this surface does not support present_timing");
		bool fpo = surfStages & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
		bool qoe = surfStages & VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT;
		if (opt.timingStage == "queue") {
			if (!qoe) fatal(kExitVulkan, "surface does not report QUEUE_OPERATIONS_END");
			timingStage = VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT;
		} else if (opt.timingStage == "pixel-out") {
			if (!fpo) fatal(kExitVulkan, "surface does not report IMAGE_FIRST_PIXEL_OUT (stages=0x%x)", surfStages);
			timingStage = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
		} else {
			if (fpo) timingStage = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
			else if (qoe) timingStage = VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT;
			else fatal(kExitVulkan, "surface reports neither FIRST_PIXEL_OUT nor QUEUE_OPERATIONS_END (stages=0x%x)", surfStages);
		}
	}
}

uint32_t App::findMemoryType(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	fatal(kExitVulkan, "no suitable memory type");
}

VkShaderModule App::makeShader(const uint32_t *code, size_t bytes)
{
	VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	ci.codeSize = bytes;
	ci.pCode = code;
	VkShaderModule m;
	VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &m));
	return m;
}

void App::createPipelines()
{
	// Graphics: fullscreen triangle + pattern, dynamic rendering into the swapchain format.
	VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GfxPush) };
	VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &gfxLayout));

	VkShaderModule vs = makeShader(spv_fullscreen_vert, sizeof(spv_fullscreen_vert));
	VkShaderModule fs = makeShader(spv_pattern_frag, sizeof(spv_pattern_frag));
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr },
	};
	VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;
	VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachmentFormats = &swapFormat;

	VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	gp.pNext = &rendering;
	gp.stageCount = 2;
	gp.pStages = stages;
	gp.pVertexInputState = &vi;
	gp.pInputAssemblyState = &ia;
	gp.pViewportState = &vp;
	gp.pRasterizationState = &rs;
	gp.pMultisampleState = &ms;
	gp.pColorBlendState = &cb;
	gp.pDynamicState = &ds;
	gp.layout = gfxLayout;
	VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &gfxPipeline));
	vkDestroyShaderModule(device, vs, nullptr);
	vkDestroyShaderModule(device, fs, nullptr);

	// Compute load: one storage buffer, one push constant (iteration count).
	VkDescriptorSetLayoutBinding b{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	VkDescriptorSetLayoutCreateInfo dsl{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dsl.bindingCount = 1;
	dsl.pBindings = &b;
	VK_CHECK(vkCreateDescriptorSetLayout(device, &dsl, nullptr, &loadSetLayout));
	VkPushConstantRange cpcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) };
	VkPipelineLayoutCreateInfo cplci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	cplci.setLayoutCount = 1;
	cplci.pSetLayouts = &loadSetLayout;
	cplci.pushConstantRangeCount = 1;
	cplci.pPushConstantRanges = &cpcr;
	VK_CHECK(vkCreatePipelineLayout(device, &cplci, nullptr, &loadLayout));
	VkShaderModule cs = makeShader(spv_load_comp, sizeof(spv_load_comp));
	VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	cpci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", nullptr };
	cpci.layout = loadLayout;
	VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &loadPipeline));
	vkDestroyShaderModule(device, cs, nullptr);

	VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bci.size = VkDeviceSize(kLoadGroups) * 256 * sizeof(float);
	bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &loadBuf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(device, loadBuf, &mr);
	VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &loadMem));
	VK_CHECK(vkBindBufferMemory(device, loadBuf, loadMem, 0));

	VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &ps;
	VK_CHECK(vkCreateDescriptorPool(device, &dpci, nullptr, &descPool));
	VkDescriptorSetAllocateInfo dai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	dai.descriptorPool = descPool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &loadSetLayout;
	VK_CHECK(vkAllocateDescriptorSets(device, &dai, &loadSet));
	VkDescriptorBufferInfo dbi{ loadBuf, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	w.dstSet = loadSet;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	w.pBufferInfo = &dbi;
	vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

void App::createFrameObjects()
{
	VkCommandPoolCreateInfo cpci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpci.queueFamilyIndex = queueFamily;
	VK_CHECK(vkCreateCommandPool(device, &cpci, nullptr, &cmdPool));

	slots.resize(std::max(1u, opt.frameLatency));
	for (auto &s : slots) {
		VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		ai.commandPool = cmdPool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		VK_CHECK(vkAllocateCommandBuffers(device, &ai, &s.cmd));
		VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		VK_CHECK(vkCreateFence(device, &fci, nullptr, &s.fence));
		VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		VK_CHECK(vkCreateSemaphore(device, &sci, nullptr, &s.acquire));
		VkQueryPoolCreateInfo qci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
		qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
		qci.queryCount = 2;
		VK_CHECK(vkCreateQueryPool(device, &qci, nullptr, &s.queries));
	}
}

// ---------------------------------------------------------------------------------------
// GPU clock calibration and load calibration
// ---------------------------------------------------------------------------------------

void App::recalibrateGpuClock(bool force)
{
	uint64_t now = monotonicNs();
	if (!force && now - calAt < 250'000'000ull)
		return;
	VkCalibratedTimestampInfoKHR info[2] = {
		{ VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_DEVICE_KHR },
		{ VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR },
	};
	uint64_t ts[2], dev = 0;
	VK_CHECK(pfnGetCalibrated(device, 2, info, ts, &dev));
	calDev = ts[0];
	calMono = ts[1];
	calAt = now;
}

uint64_t App::gpuToMono(uint64_t ticks)
{
	if (timestampValidBits < 64) {
		uint64_t mask = (1ull << timestampValidBits) - 1;
		ticks &= mask;
		calDev &= mask;
	}
	double deltaNs = (double(int64_t(ticks - calDev))) * timestampPeriodNs;
	return uint64_t(int64_t(calMono) + int64_t(deltaNs));
}

// Runs `runs` dispatches of the load shader back to back, each bracketed by timestamps, and
// returns the median GPU time in ms.
double App::measureLoadMs(uint32_t iters, int runs, double *minMs, double *maxMs)
{
	VkQueryPoolCreateInfo qci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
	qci.queryCount = uint32_t(runs) * 2;
	VkQueryPool qp;
	VK_CHECK(vkCreateQueryPool(device, &qci, nullptr, &qp));

	VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	ai.commandPool = cmdPool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cb;
	VK_CHECK(vkAllocateCommandBuffers(device, &ai, &cb));
	VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK(vkBeginCommandBuffer(cb, &bi));
	vkCmdResetQueryPool(cb, qp, 0, qci.queryCount);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, loadPipeline);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, loadLayout, 0, 1, &loadSet, 0, nullptr);
	vkCmdPushConstants(cb, loadLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(iters), &iters);
	VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
	mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
	VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &mb;
	for (int i = 0; i < runs; i++) {
		vkCmdPipelineBarrier2(cb, &dep);
		vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, uint32_t(i) * 2);
		vkCmdDispatch(cb, kLoadGroups, 1, 1);
		vkCmdPipelineBarrier2(cb, &dep);
		vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, uint32_t(i) * 2 + 1);
	}
	VK_CHECK(vkEndCommandBuffer(cb));

	VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	VK_CHECK(vkCreateFence(device, &fci, nullptr, &fence));
	VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cb;
	VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));
	VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 30'000'000'000ull));

	std::vector<uint64_t> ts(qci.queryCount);
	VK_CHECK(vkGetQueryPoolResults(device, qp, 0, qci.queryCount, ts.size() * sizeof(uint64_t), ts.data(),
	                               sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
	std::vector<double> ms;
	for (int i = 0; i < runs; i++)
		ms.push_back(double(ts[i * 2 + 1] - ts[i * 2]) * timestampPeriodNs / 1e6);
	std::sort(ms.begin(), ms.end());

	vkDestroyFence(device, fence, nullptr);
	vkFreeCommandBuffers(device, cmdPool, 1, &cb);
	vkDestroyQueryPool(device, qp, nullptr);
	if (minMs) *minMs = ms.front();
	if (maxMs) *maxMs = ms.back();
	return ms[ms.size() / 2];
}

void App::calibrateLoad()
{
	if (opt.gpuMs <= 0)
		return;
	const double target = opt.gpuMs;
	uint32_t iters = 2048;
	double ms = 0;
	int round = 0;
	for (; round < 24; round++) {
		ms = measureLoadMs(iters, 7);
		double ratio = target / std::max(ms, 1e-3);
		// Let the GPU clocks ramp for a few rounds before trusting the numbers.
		if (round >= 4 && std::abs(ratio - 1.0) < 0.01)
			break;
		double next = double(iters) * ratio;
		iters = uint32_t(std::clamp(next, 1.0, 100'000'000.0));
	}
	double mn = 0, mx = 0;
	double med = measureLoadMs(iters, 31, &mn, &mx);
	loadIters = iters;
	printf("[vk_truth] gpu load calibration: target %.3f ms -> %u iterations/thread x %u threads, "
	       "measured median %.3f ms (min %.3f, max %.3f) after %d rounds\n",
	       target, iters, kLoadGroups * 256, med, mn, mx, round + 1);
	if (std::abs(med / target - 1.0) > 0.10)
		printf("[vk_truth] WARNING: calibration is more than 10%% off the target (fixed per-dispatch overhead?)\n");
}

// ---------------------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------------------

static VkPresentModeKHR parsePresentMode(const std::string &s)
{
	if (s == "fifo") return VK_PRESENT_MODE_FIFO_KHR;
	if (s == "fifo_relaxed") return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
	if (s == "mailbox") return VK_PRESENT_MODE_MAILBOX_KHR;
	if (s == "immediate") return VK_PRESENT_MODE_IMMEDIATE_KHR;
	return VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
}

void App::destroySwapchainViews()
{
	for (auto v : views) vkDestroyImageView(device, v, nullptr);
	for (auto s : renderDone) vkDestroySemaphore(device, s, nullptr);
	views.clear();
	renderDone.clear();
	images.clear();
}

void App::pickTimingDomain()
{
	if (!swapUsesTiming)
		return;
	VkTimeDomainKHR doms[16];
	uint64_t ids[16];
	VkSwapchainTimeDomainPropertiesEXT p{ VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT };
	p.timeDomainCount = 16;
	p.pTimeDomains = doms;
	p.pTimeDomainIds = ids;
	uint64_t counter = 0;
	VK_CHECK(pfnGetTimeDomains(device, swapchain, &p, &counter));
	timeDomainsCounter = counter;

	auto pick = [&](VkTimeDomainKHR want) {
		for (uint32_t i = 0; i < p.timeDomainCount; i++)
			if (doms[i] == want) {
				timingDomain = doms[i];
				timingDomainId = ids[i];
				return true;
			}
		return false;
	};
	bool ok = false;
	if (opt.timeDomain == "monotonic") ok = pick(VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR);
	else if (opt.timeDomain == "stage") ok = pick(VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT);
	else if (opt.timeDomain == "swapchain") ok = pick(VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT);
	else ok = pick(VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR) || pick(VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT) ||
	          pick(VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT);
	if (!ok && p.timeDomainCount) {
		timingDomain = doms[0];
		timingDomainId = ids[0];
		ok = opt.timeDomain == "auto";
	}
	if (!ok)
		fatal(kExitVulkan, "--time-domain %s is not offered by the swapchain", opt.timeDomain.c_str());
	static bool printed = false;
	if (!printed) {
		printed = true;
		printf("[vk_truth] time domains offered:");
		for (uint32_t i = 0; i < p.timeDomainCount; i++)
			printf(" %d(id %llu)", int(doms[i]), (unsigned long long)ids[i]);
		printf("; using domain %d id %llu\n", int(timingDomain), (unsigned long long)timingDomainId);
	}
}

// Returns false when the window currently has no drawable area (minimized).
bool App::createSwapchain()
{
	VkSurfaceCapabilitiesKHR caps;
	VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));

	VkExtent2D ext = caps.currentExtent;
	if (ext.width == 0xFFFFFFFFu) {
		int w = 0, h = 0;
		SDL_GetWindowSizeInPixels(window, &w, &h);
		ext.width = std::clamp(uint32_t(std::max(w, 0)), caps.minImageExtent.width, caps.maxImageExtent.width);
		ext.height = std::clamp(uint32_t(std::max(h, 0)), caps.minImageExtent.height, caps.maxImageExtent.height);
	}
	if (!ext.width || !ext.height)
		return false;

	// Format and color space
	uint32_t fn = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(fn);
	vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, formats.data());
	VkFormat wantFmt;
	VkColorSpaceKHR wantCs;
	if (opt.format == "srgb") { wantFmt = VK_FORMAT_B8G8R8A8_SRGB; wantCs = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; shaderMode = 0; }
	else if (opt.format == "unorm") { wantFmt = VK_FORMAT_B8G8R8A8_UNORM; wantCs = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; shaderMode = 1; }
	else if (opt.format == "hdr10") { wantFmt = VK_FORMAT_A2B10G10R10_UNORM_PACK32; wantCs = VK_COLOR_SPACE_HDR10_ST2084_EXT; shaderMode = 2; }
	else { wantFmt = VK_FORMAT_R16G16B16A16_SFLOAT; wantCs = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT; shaderMode = 3; }
	bool found = false;
	for (auto &f : formats)
		if (f.format == wantFmt && f.colorSpace == wantCs)
			found = true;
	if (!found) {
		std::string have;
		for (auto &f : formats)
			have += " (" + std::to_string(int(f.format)) + "," + std::to_string(int(f.colorSpace)) + ")";
		fatal(kExitVulkan, "--format %s unavailable on this surface (needs format %d colorspace %d); surface offers:%s",
		      opt.format.c_str(), int(wantFmt), int(wantCs), have.c_str());
	}
	swapFormat = wantFmt;
	swapColorSpace = wantCs;

	// Present mode
	presentMode = parsePresentMode(opt.presentMode);
	uint32_t pn = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pn, nullptr);
	std::vector<VkPresentModeKHR> modes(pn);
	vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pn, modes.data());
	if (std::find(modes.begin(), modes.end(), presentMode) == modes.end()) {
		std::string have;
		for (auto m : modes) {
			switch (m) {
			case VK_PRESENT_MODE_IMMEDIATE_KHR: have += " immediate"; break;
			case VK_PRESENT_MODE_MAILBOX_KHR: have += " mailbox"; break;
			case VK_PRESENT_MODE_FIFO_KHR: have += " fifo"; break;
			case VK_PRESENT_MODE_FIFO_RELAXED_KHR: have += " fifo_relaxed"; break;
			case VK_PRESENT_MODE_FIFO_LATEST_READY_KHR: have += " fifo_latest_ready"; break;
			default: have += " (" + std::to_string(int(m)) + ")"; break;
			}
		}
		fatal(kExitVulkan, "--present-mode %s is not offered by this surface; it offers:%s", opt.presentMode.c_str(), have.c_str());
	}

	uint32_t count = std::max(opt.images, caps.minImageCount);
	if (caps.maxImageCount)
		count = std::min(count, caps.maxImageCount);

	VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	ci.surface = surface;
	ci.minImageCount = count;
	ci.imageFormat = swapFormat;
	ci.imageColorSpace = swapColorSpace;
	ci.imageExtent = ext;
	ci.imageArrayLayers = 1;
	ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ci.preTransform = caps.currentTransform;
	ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
		? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	ci.presentMode = presentMode;
	ci.clipped = VK_TRUE;
	ci.oldSwapchain = swapchain;
	if (hasPresentId2) ci.flags |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
	if (hasPresentWait2) ci.flags |= VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
	if (hasPresentTiming) ci.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;

	VkSwapchainKHR fresh = VK_NULL_HANDLE;
	VkResult r = vkCreateSwapchainKHR(device, &ci, nullptr, &fresh);
	if (swapchain) {
		vkDestroySwapchainKHR(device, swapchain, nullptr);
		swapchain = VK_NULL_HANDLE;
	}
	destroySwapchainViews();
	if (r < 0)
		fatal(kExitVulkan, "vkCreateSwapchainKHR failed: VkResult %d", int(r));
	swapchain = fresh;
	extent = ext;
	swapUsesPresentId = hasPresentId2;
	swapUsesWait2 = hasPresentWait2;
	swapUsesWait1 = hasPresentWait1;  // v1 needs no swapchain flag
	swapUsesTiming = hasPresentTiming;
	swapchainFirstId = 0;

	uint32_t in = 0;
	vkGetSwapchainImagesKHR(device, swapchain, &in, nullptr);
	images.resize(in);
	vkGetSwapchainImagesKHR(device, swapchain, &in, images.data());
	for (auto img : images) {
		VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vci.image = img;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = swapFormat;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkImageView v;
		VK_CHECK(vkCreateImageView(device, &vci, nullptr, &v));
		views.push_back(v);
		VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		VkSemaphore s;
		VK_CHECK(vkCreateSemaphore(device, &sci, nullptr, &s));
		renderDone.push_back(s);
	}

	if (swapUsesTiming) {
		if (!timingQueueSize)
			timingQueueSize = opt.timingQueue;
		VK_CHECK(pfnSetTimingQueueSize(device, swapchain, timingQueueSize));
		pickTimingDomain();
	}
	return true;
}

// Drains what the old swapchain still owes us, then replaces it. Timing requests that never
// complete on a destroyed swapchain are marked abandoned (not an anomaly).
void App::recreateSwapchain()
{
	at("vkDeviceWaitIdle(recreate)");
	VK_CHECK(vkDeviceWaitIdle(device));
	if (swapUsesTiming && swapchain) {
		uint64_t until = monotonicNs() + 150'000'000ull;
		do {
			pollTiming(true);
			bool pending = false;
			for (size_t i = rows.size(); i-- > 0 && i + 4 * timingQueueSize > rows.size();)
				if (rows[i].timing == Row::Pending) { pending = true; break; }
			if (!pending) break;
			struct timespec ts{ 0, 2'000'000 };
			nanosleep(&ts, nullptr);
		} while (monotonicNs() < until);
		abandonPending();
	}
	needRecreate = false;
	recreations++;
	if (!createSwapchain())
		return; // minimized; the main loop retries
	printf("[vk_truth] swapchain recreated: %ux%u, %u images\n", extent.width, extent.height, unsigned(images.size()));
}

void App::abandonPending()
{
	for (auto &r : rows)
		if (r.timing == Row::Pending)
			r.timing = Row::Abandoned;
}

// ---------------------------------------------------------------------------------------
// Present timing (--app-timing): mirrors DXVK's Presenter::updatePresentTiming
// ---------------------------------------------------------------------------------------

// Converts a present-stage time in the report's domain to CLOCK_MONOTONIC ns. 0 = cannot.
uint64_t App::toMono(VkTimeDomainKHR domain, uint64_t domainId, VkPresentStageFlagsEXT stage, uint64_t t)
{
	if (domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR)
		return t;
	if (domain == VK_TIME_DOMAIN_DEVICE_KHR) {
		recalibrateGpuClock(true);
		return gpuToMono(t);
	}
	if (domain == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT || domain == VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT) {
		VkSwapchainCalibratedTimestampInfoEXT sc{ VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT };
		sc.swapchain = swapchain;
		sc.presentStage = domain == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT ? stage : 0;
		sc.timeDomainId = domainId;
		VkCalibratedTimestampInfoKHR info[2] = {
			{ VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR },
			{ VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, &sc, domain },
		};
		uint64_t ts[2], dev = 0;
		if (pfnGetCalibrated(device, 2, info, ts, &dev) != VK_SUCCESS)
			return 0;
		// The local domain is in ns; shift the report onto the monotonic axis.
		return ts[0] - (ts[1] - t);
	}
	return 0;
}

void App::pollTiming(bool drain)
{
	(void)drain;
	if (!swapUsesTiming || !swapchain)
		return;

	const uint32_t qn = timingQueueSize;
	VkPresentStageTimeEXT stageTimes[kMaxTimingQueue];
	VkPastPresentationTimingEXT reports[kMaxTimingQueue];
	for (uint32_t i = 0; i < qn; i++) {
		stageTimes[i] = {};
		reports[i] = { VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT };
		reports[i].presentStageCount = 1;
		reports[i].pPresentStages = &stageTimes[i];
	}
	VkPastPresentationTimingInfoEXT info{ VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT };
	info.swapchain = swapchain;
	VkPastPresentationTimingPropertiesEXT props{ VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT };
	props.presentationTimingCount = qn;
	props.pPresentationTimings = reports;

	at("vkGetPastPresentationTimingEXT");
	VkResult r = pfnGetPastTiming(device, &info, &props);
	uint64_t pollTime = monotonicNs();
	if (r < 0) {
		anomaly("query-failed", "vkGetPastPresentationTimingEXT returned %d", int(r));
		return;
	}
	if (props.timeDomainsCounter != timeDomainsCounter) {
		pickTimingDomain(); // domain ids changed; later presents must use the new ones
	}

	for (uint32_t i = 0; i < props.presentationTimingCount; i++) {
		const auto &rep = reports[i];
		const auto &st = stageTimes[i];
		timingReports++;
		if (!rep.presentId || rep.presentId > rows.size()) {
			anomaly("unknown-id", "report for present id %llu, only %zu presents made", (unsigned long long)rep.presentId, rows.size());
			continue;
		}
		Row &row = rows[rep.presentId - 1];
		if (row.timing == Row::Reported) {
			anomaly("duplicate", "present id %llu reported twice", (unsigned long long)rep.presentId);
			continue;
		}
		if (row.timing != Row::Pending && row.timing != Row::Abandoned) {
			anomaly("unrequested", "present id %llu reported but never requested", (unsigned long long)rep.presentId);
			continue;
		}
		row.timing = Row::Reported;
		if (rep.presentId <= lastReportedId)
			anomaly("order", "present id %llu reported after %llu", (unsigned long long)rep.presentId, (unsigned long long)lastReportedId);
		lastReportedId = std::max(lastReportedId, rep.presentId);
		if (!rep.reportComplete) {
			anomaly("incomplete", "present id %llu reported with reportComplete=0 (no partial flag was set)", (unsigned long long)rep.presentId);
			continue;
		}
		if (rep.presentStageCount != 1 || st.stage != uint32_t(timingStage)) {
			anomaly("wrong-stage", "present id %llu: stage 0x%x (count %u), requested 0x%x", (unsigned long long)rep.presentId,
			        st.stage, rep.presentStageCount, uint32_t(timingStage));
			continue;
		}
		row.stage = st.stage;
		if (!st.time) {
			// IMAGE_FIRST_PIXEL_OUT of a present that was never shown (replaced by a newer one):
			// DXVK skips these too. QUEUE_OPERATIONS_END always happens, so there 0 is a bug.
			if (timingStage == VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT)
				anomaly("zero-time", "present id %llu: QUEUE_OPERATIONS_END time is 0", (unsigned long long)rep.presentId);
			else
				notDisplayed++;
			continue;
		}
		uint64_t mono = toMono(rep.timeDomain, rep.timeDomainId, st.stage, st.time);
		if (!mono) {
			anomaly("convert", "present id %llu: cannot convert time domain %d to CLOCK_MONOTONIC", (unsigned long long)rep.presentId, int(rep.timeDomain));
			continue;
		}
		row.stageTime = mono;
		// Plausible = after the frame began and not in the future.
		if (mono + 0 < row.cpuStart)
			anomaly("early", "present id %llu: stage time %.3f ms before the frame's CPU work started", (unsigned long long)rep.presentId, double(row.cpuStart - mono) / 1e6);
		else if (mono > pollTime + 1'000'000ull)
			anomaly("future", "present id %llu: stage time %.3f ms in the future", (unsigned long long)rep.presentId, double(mono - pollTime) / 1e6);
		if (lastReportedMono && mono < lastReportedMono)
			anomaly("non-monotonic", "present id %llu: stage time goes back by %.3f ms", (unsigned long long)rep.presentId, double(lastReportedMono - mono) / 1e6);
		lastReportedMono = std::max(lastReportedMono, mono);
	}
}

// ---------------------------------------------------------------------------------------
// Frame recording / GPU timestamps
// ---------------------------------------------------------------------------------------

void App::recordFrame(Slot &s, uint32_t imageIndex, uint64_t frame)
{
	VkCommandBuffer cb = s.cmd;
	VK_CHECK(vkResetCommandBuffer(cb, 0));
	VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK(vkBeginCommandBuffer(cb, &bi));
	vkCmdResetQueryPool(cb, s.queries, 0, 2);

	// Serialize with the previous frame's GPU work so "begin" means "the GPU got to this
	// frame", not "it was queued". Only compute/fragment stages are in the first scope, so
	// the swapchain acquire semaphore (waited at COLOR_ATTACHMENT_OUTPUT) cannot delay it.
	VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
	mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
	VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &mb;
	vkCmdPipelineBarrier2(cb, &dep);
	vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, s.queries, 0);

	if (loadIters) {
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, loadPipeline);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, loadLayout, 0, 1, &loadSet, 0, nullptr);
		vkCmdPushConstants(cb, loadLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(loadIters), &loadIters);
		vkCmdDispatch(cb, kLoadGroups, 1, 1);
	}

	// Compute -> the rest of the frame, so the load and the pattern do not overlap.
	VkMemoryBarrier2 mb2{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	mb2.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	mb2.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkImageMemoryBarrier2 toColor{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	toColor.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	toColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
	toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	toColor.srcQueueFamilyIndex = toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toColor.image = images[imageIndex];
	toColor.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkDependencyInfo dep2{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dep2.memoryBarrierCount = 1;
	dep2.pMemoryBarriers = &mb2;
	dep2.imageMemoryBarrierCount = 1;
	dep2.pImageMemoryBarriers = &toColor;
	vkCmdPipelineBarrier2(cb, &dep2);

	VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	att.imageView = views[imageIndex];
	att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
	ri.renderArea = { { 0, 0 }, extent };
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &att;
	vkCmdBeginRendering(cb, &ri);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfxPipeline);
	VkViewport vp{ 0, 0, float(extent.width), float(extent.height), 0, 1 };
	VkRect2D sc{ { 0, 0 }, extent };
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &sc);
	GfxPush pc{};
	pc.res[0] = float(extent.width);
	pc.res[1] = float(extent.height);
	pc.offset[0] = mouseDx;
	pc.offset[1] = mouseDy;
	pc.frame = uint32_t(frame);
	pc.mode = shaderMode;
	pc.time = float(double(frame) * 0.016);
	vkCmdPushConstants(cb, gfxLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRendering(cb);

	VkImageMemoryBarrier2 toPresent = toColor;
	toPresent.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
	toPresent.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
	toPresent.dstAccessMask = 0;
	toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	VkDependencyInfo dep3{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dep3.imageMemoryBarrierCount = 1;
	dep3.pImageMemoryBarriers = &toPresent;
	vkCmdPipelineBarrier2(cb, &dep3);
	vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries, 1);
	VK_CHECK(vkEndCommandBuffer(cb));
}

void App::harvest(Slot &s)
{
	if (s.frame < 0)
		return;
	uint64_t ts[2] = {};
	VkResult r = vkGetQueryPoolResults(device, s.queries, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
	                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
	if (r == VK_SUCCESS) {
		recalibrateGpuClock(false);
		rows[size_t(s.frame)].gpuBegin = gpuToMono(ts[0]);
		rows[size_t(s.frame)].gpuEnd = gpuToMono(ts[1]);
	}
	s.frame = -1;
}

void App::waitForPresentDistance(uint64_t presentId)
{
	// DXVK only waits in the FIFO modes: with IMMEDIATE/MAILBOX a replaced present may never complete.
	const bool v1 = swapUsesWait1 && pfnWaitForPresent;
	if (!v1 && (!swapUsesWait2 || !pfnWaitForPresent2))
		return;
	if (presentMode != VK_PRESENT_MODE_FIFO_KHR && presentMode != VK_PRESENT_MODE_FIFO_RELAXED_KHR)
		return;
	if (presentId < swapchainFirstId || !swapchainFirstId)
		return;
	const uint64_t t0 = monotonicNs();
	VkResult r;
	if (v1) {
		// VK_KHR_present_wait: returns once the swapchain's present id is >= presentId (DXVK 2.x).
		at("vkWaitForPresentKHR");
		r = pfnWaitForPresent(device, swapchain, presentId, 200'000'000ull);
	} else {
		VkPresentWait2InfoKHR wi{ VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR };
		wi.presentId = presentId;
		wi.timeout = 200'000'000ull;
		at("vkWaitForPresent2KHR");
		r = pfnWaitForPresent2(device, swapchain, &wi);
	}
	const uint64_t waited = monotonicNs() - t0;
	presentWaits++;
	presentWaitNsSum += waited;
	presentWaitNsMax = std::max(presentWaitNsMax, waited);
	if (r == VK_TIMEOUT)
		presentWaitTimeouts++;
	else if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR)
		needRecreate = true;
	else if (r < 0)
		fatal(kExitVulkan, "%s failed: VkResult %d", v1 ? "vkWaitForPresentKHR" : "vkWaitForPresent2KHR", int(r));
}

// ---------------------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------------------

void App::writeLog()
{
	if (opt.logPath.empty())
		return;
	FILE *f = fopen(opt.logPath.c_str(), "w");
	if (!f) {
		fprintf(stderr, "[vk_truth] cannot write %s: %s\n", opt.logPath.c_str(), strerror(errno));
		return;
	}
	fprintf(f, "frame,present_id,sleep_start_ns,sleep_end_ns,cpu_start_ns,cpu_end_ns,present_pre_ns,present_post_ns,"
	           "gpu_begin_ns,gpu_end_ns,timing_stage,timing_ns,present_result\n");
	for (auto &r : rows)
		fprintf(f, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%llu,%d\n", (unsigned long long)r.frame,
		        (unsigned long long)r.presentId, (unsigned long long)r.sleepStart, (unsigned long long)r.sleepEnd,
		        (unsigned long long)r.cpuStart, (unsigned long long)r.cpuEnd, (unsigned long long)r.presentPre,
		        (unsigned long long)r.presentPost, (unsigned long long)r.gpuBegin, (unsigned long long)r.gpuEnd,
		        r.stage, (unsigned long long)r.stageTime, int(r.presentResult));
	fclose(f);
	printf("[vk_truth] wrote %zu rows to %s\n", rows.size(), opt.logPath.c_str());
}

void App::summary(double wallSeconds)
{
	size_t n = rows.size();
	printf("[vk_truth] summary: frames=%zu wall=%.3fs swapchain_recreations=%u\n", n, wallSeconds, recreations);
	if (n >= 2) {
		std::vector<double> iv;
		for (size_t i = 1; i < n; i++)
			iv.push_back(double(rows[i].presentPre - rows[i - 1].presentPre) / 1e6);
		double sum = 0;
		for (double v : iv) sum += v;
		std::sort(iv.begin(), iv.end());
		double avg = sum / double(iv.size());
		printf("[vk_truth]   present interval: avg %.3f ms (%.2f fps), median %.3f, min %.3f, max %.3f\n", avg,
		       1000.0 / avg, iv[iv.size() / 2], iv.front(), iv.back());
		double cpu = 0, gpu = 0;
		size_t gn = 0;
		for (auto &r : rows) {
			cpu += double(r.cpuEnd - r.cpuStart) / 1e6;
			if (r.gpuEnd > r.gpuBegin) { gpu += double(r.gpuEnd - r.gpuBegin) / 1e6; gn++; }
		}
		printf("[vk_truth]   avg cpu work %.3f ms, avg gpu work %.3f ms\n", cpu / double(n), gn ? gpu / double(gn) : 0.0);
	}
	if (opt.appTiming) {
		uint64_t requested = 0, reported = 0, abandoned = 0;
		for (auto &r : rows) {
			requested += r.timing != Row::None;
			reported += r.timing == Row::Reported;
			abandoned += r.timing == Row::Abandoned;
		}
		printf("[vk_truth]   present timing: requested=%llu reported=%llu abandoned(swapchain recreated)=%llu "
		       "reports_seen=%llu queue_full=%llu queue_full_spurious=%llu final_queue_size=%u "
		       "not_displayed(time 0)=%llu\n",
		       (unsigned long long)requested, (unsigned long long)reported, (unsigned long long)abandoned,
		       (unsigned long long)timingReports, (unsigned long long)queueFull, (unsigned long long)queueFullSpurious,
		       timingQueueSize, (unsigned long long)notDisplayed);
		printf("[vk_truth]   present-timing anomalies: %llu\n", (unsigned long long)anomalies);
		for (auto &k : anomalyKinds)
			printf("[vk_truth]     %s: %llu\n", k.first.c_str(), (unsigned long long)k.second);
	}
	if (opt.appPresentId) {
		printf("[vk_truth]   %s timeouts: %llu\n", opt.presentIdV1 ? "present_wait (v1)" : "present_wait2",
		       (unsigned long long)presentWaitTimeouts);
		printf("[vk_truth]   present waits: %llu, avg %.3f ms, max %.3f ms\n", (unsigned long long)presentWaits,
		       presentWaits ? double(presentWaitNsSum) / double(presentWaits) / 1e6 : 0.0, double(presentWaitNsMax) / 1e6);
	}
}

void App::cleanup()
{
	if (device) {
		at("cleanup");
		vkDeviceWaitIdle(device);
		for (auto &s : slots) {
			vkDestroyQueryPool(device, s.queries, nullptr);
			vkDestroySemaphore(device, s.acquire, nullptr);
			vkDestroyFence(device, s.fence, nullptr);
		}
		destroySwapchainViews();
		if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
		if (loadPipeline) vkDestroyPipeline(device, loadPipeline, nullptr);
		if (loadLayout) vkDestroyPipelineLayout(device, loadLayout, nullptr);
		if (loadSetLayout) vkDestroyDescriptorSetLayout(device, loadSetLayout, nullptr);
		if (descPool) vkDestroyDescriptorPool(device, descPool, nullptr);
		if (loadBuf) vkDestroyBuffer(device, loadBuf, nullptr);
		if (loadMem) vkFreeMemory(device, loadMem, nullptr);
		if (gfxPipeline) vkDestroyPipeline(device, gfxPipeline, nullptr);
		if (gfxLayout) vkDestroyPipelineLayout(device, gfxLayout, nullptr);
		if (cmdPool) vkDestroyCommandPool(device, cmdPool, nullptr);
		vkDestroyDevice(device, nullptr);
	}
	if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
	if (instance) vkDestroyInstance(instance, nullptr);
	if (window) SDL_DestroyWindow(window);
	SDL_Quit();
}

// ---------------------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------------------

int App::run()
{
	initSdl();
	if (opt.listDisplays) {
		listDisplays();
		SDL_Quit();
		return kExitOk;
	}

	// Window
	SDL_DisplayID displayId = 0;
	if (!opt.display.empty()) {
		int n = 0;
		SDL_DisplayID *ids = SDL_GetDisplays(&n);
		char *end = nullptr;
		long idx = strtol(opt.display.c_str(), &end, 10);
		if (*end == 0 && idx >= 0 && idx < n) {
			displayId = ids[idx];
		} else {
			for (int i = 0; i < n; i++) {
				const char *name = SDL_GetDisplayName(ids[i]);
				if (name && strstr(name, opt.display.c_str())) { displayId = ids[i]; break; }
			}
		}
		SDL_free(ids);
		if (!displayId)
			fatal(kExitUsage, "--display %s: no such display (see --list-displays)", opt.display.c_str());
	}
	SDL_PropertiesID wp = SDL_CreateProperties();
	SDL_SetStringProperty(wp, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "vk_truth");
	SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, opt.width);
	SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, opt.height);
	SDL_SetBooleanProperty(wp, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
	SDL_SetBooleanProperty(wp, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
	if (displayId) {
		SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(displayId));
		SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(displayId));
	}
	window = SDL_CreateWindowWithProperties(wp);
	SDL_DestroyProperties(wp);
	if (!window)
		fatal(kExitVulkan, "SDL_CreateWindow failed: %s", SDL_GetError());
	if (opt.fullscreen) {
		if (displayId) {
			if (const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(displayId))
				SDL_SetWindowFullscreenMode(window, m);
		}
		SDL_SetWindowFullscreen(window, true);
		SDL_SyncWindow(window);
	}
	SDL_ShowWindow(window);

	createInstance();
	if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
		fatal(kExitVulkan, "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
	pickDevice();
	createDevice();
	queryCapabilities();

	printf("[vk_truth] video driver %s, device \"%s\" (Vulkan %u.%u.%u), engine name \"%s\"\n", SDL_GetCurrentVideoDriver(),
	       physProps.deviceName, VK_API_VERSION_MAJOR(physProps.apiVersion), VK_API_VERSION_MINOR(physProps.apiVersion),
	       VK_API_VERSION_PATCH(physProps.apiVersion), opt.engineName.empty() ? "vk_truth" : opt.engineName.c_str());
	if (hasPresentTiming)
		printf("[vk_truth] present timing: surface stages 0x%x, requesting stage 0x%x on every present\n", surfStages, uint32_t(timingStage));

	if (!createSwapchain())
		fatal(kExitVulkan, "window has no drawable area at startup");
	printf("[vk_truth] swapchain: %ux%u, %u images, format %d, present mode %s%s%s%s%s\n", extent.width, extent.height,
	       unsigned(images.size()), int(swapFormat), opt.presentMode.c_str(), swapUsesPresentId ? ", present_id2" : "",
	       swapUsesWait2 ? ", present_wait2" : "", swapUsesTiming ? ", present_timing" : "",
	       hasPresentId1 ? ", present_id + present_wait (v1)" : "");
	createFrameObjects();
	createPipelines();
	recalibrateGpuClock(true);
	calibrateLoad();

	struct sigaction sa{};
	sa.sa_handler = onSignal;
	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);

	if (opt.inputReactive)
		SDL_SetWindowRelativeMouseMode(window, true);

	const uint64_t periodNs = opt.fps > 0 ? uint64_t(1e9 / opt.fps) : 0;
	const uint64_t cpuSpinNs = uint64_t(opt.cpuMs * 1e6);
	const bool chainIds = hasPresentId2 || hasPresentId1;
	uint64_t nextTarget = 0;
	rows.reserve(1 << 16);

	if (opt.watchdog > 0)
		std::thread(watchdogMain, opt.watchdog).detach();
	at("loop");
	const uint64_t tStart = monotonicNs();
	uint64_t frame = 0;
	while (!quit) {
		pumpEvents();
		if (quit)
			break;
		uint64_t now = monotonicNs();
		if (opt.frames && frame >= opt.frames)
			break;
		if (opt.seconds > 0 && double(now - tStart) >= opt.seconds * 1e9)
			break;

		if (opt.resizeTest && !opt.fullscreen) {
			static uint64_t lastResize = 0;
			static int step = 0;
			if (now - lastResize > 500'000'000ull) {
				lastResize = now;
				step = (step + 1) % 4;
				SDL_SetWindowSize(window, int(opt.width) + step * 120, int(opt.height) + step * 70);
			}
		}

		if (minimized || needRecreate || !swapchain || images.empty()) {
			if (minimized) {
				// Nothing to draw into; keep the compositor happy and re-arm the schedule.
				SDL_WaitEventTimeout(nullptr, 50);
				nextTarget = 0;
				continue;
			}
			recreateSwapchain();
			if (images.empty()) {
				SDL_WaitEventTimeout(nullptr, 20);
				continue;
			}
		}

		Row row;
		row.frame = frame;
		row.presentId = chainIds ? frame + 1 : 0;

		// 1. pacing sleep against an absolute schedule
		row.sleepStart = monotonicNs();
		if (periodNs) {
			if (!nextTarget || row.sleepStart > nextTarget + 2 * periodNs)
				nextTarget = row.sleepStart; // first frame, or stalled: resync instead of bursting
			sleepUntil(nextTarget);
			nextTarget += periodNs;
		}
		row.sleepEnd = monotonicNs();
		if (quit)
			break;

		// 2. keep at most frameLatency presents outstanding (DXVK-style)
		if ((swapUsesWait2 || swapUsesWait1) && row.presentId > opt.frameLatency)
			waitForPresentDistance(row.presentId - opt.frameLatency);

		// 3. CPU work
		pumpEvents();
		row.cpuStart = monotonicNs();
		if (cpuSpinNs)
			while (monotonicNs() - row.cpuStart < cpuSpinNs) {}
		row.cpuEnd = monotonicNs();

		// 4. acquire + submit
		Slot &s = slots[frame % slots.size()];
		at("vkWaitForFences(frame slot)");
		VK_CHECK(vkWaitForFences(device, 1, &s.fence, VK_TRUE, UINT64_MAX));
		harvest(s);
		uint32_t imageIndex = 0;
		at("vkAcquireNextImageKHR");
		VkResult ar = vkAcquireNextImageKHR(device, swapchain, 1'000'000'000ull, s.acquire, VK_NULL_HANDLE, &imageIndex);
		if (ar == VK_TIMEOUT || ar == VK_NOT_READY)
			continue; // no image yet (window hidden or the driver is throttling); the watchdog catches real stalls
		if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
			needRecreate = true;
			continue;
		}
		if (ar < 0)
			fatal(kExitVulkan, "vkAcquireNextImageKHR failed: VkResult %d", int(ar));
		if (ar == VK_SUBOPTIMAL_KHR)
			needRecreate = true; // still usable this frame; recreate afterwards

		recordFrame(s, imageIndex, frame);
		VK_CHECK(vkResetFences(device, 1, &s.fence));
		VkSemaphoreSubmitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
		wait.semaphore = s.acquire;
		wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSemaphoreSubmitInfo signal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
		signal.semaphore = renderDone[imageIndex];
		signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		VkCommandBufferSubmitInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
		cbi.commandBuffer = s.cmd;
		VkSubmitInfo2 si{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
		si.waitSemaphoreInfoCount = 1;
		si.pWaitSemaphoreInfos = &wait;
		si.commandBufferInfoCount = 1;
		si.pCommandBufferInfos = &cbi;
		si.signalSemaphoreInfoCount = 1;
		si.pSignalSemaphoreInfos = &signal;
		VK_CHECK(vkQueueSubmit2(queue, 1, &si, s.fence));
		s.frame = int64_t(frame);

		// 5. present
		uint64_t presentIdValue = row.presentId;
		VkPresentId2KHR pid{ VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR };
		pid.swapchainCount = 1;
		pid.pPresentIds = &presentIdValue;
		VkPresentIdKHR pid1{ VK_STRUCTURE_TYPE_PRESENT_ID_KHR };
		pid1.swapchainCount = 1;
		pid1.pPresentIds = &presentIdValue;
		VkPresentTimingInfoEXT pti{ VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT };
		pti.timeDomainId = timingDomainId;
		pti.presentStageQueries = timingStage;
		VkPresentTimingsInfoEXT ptis{ VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT };
		ptis.swapchainCount = 1;
		ptis.pTimingInfos = &pti;
		VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &renderDone[imageIndex];
		pi.swapchainCount = 1;
		pi.pSwapchains = &swapchain;
		pi.pImageIndices = &imageIndex;
		const void *pn = nullptr;
		if (swapUsesTiming) { ptis.pNext = pn; pn = &ptis; }
		if (hasPresentId2) { pid.pNext = pn; pn = &pid; }
		else if (hasPresentId1) { pid1.pNext = pn; pn = &pid1; }
		pi.pNext = pn;

		if (!swapchainFirstId)
			swapchainFirstId = row.presentId;
		at("vkQueuePresentKHR");
		row.presentPre = monotonicNs();
		VkResult pr = vkQueuePresentKHR(queue, &pi);
		row.presentPost = monotonicNs();
		row.presentResult = int32_t(pr);
		bool presented = pr >= 0;
		row.timing = (presented && swapUsesTiming) ? Row::Pending : Row::None;
		if (pr == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT) {
			// Handled like DXVK 3.1 (dxvk_presenter.cpp): no retry, because the rejected present has already
			// executed its semaphore waits; double the queue size and recreate the swapchain. The frame is lost.
			queueFull++;
			uint64_t outstanding = 0;
			for (const auto &r : rows)
				outstanding += r.timing == Row::Pending;
			if (outstanding < timingQueueSize)
				queueFullSpurious++;
			if (queueFull == 1 || outstanding < timingQueueSize)
				printf("[vk_truth] present id %llu rejected (timing queue full, %llu of %u requests outstanding): "
				       "doubling the queue and recreating the swapchain\n",
				       (unsigned long long)row.presentId, (unsigned long long)outstanding, timingQueueSize);
			timingQueueSize = std::min(timingQueueSize * 2, kMaxTimingQueue);
			needRecreate = true;
		}
		rows.push_back(row);
		frame++;

		if (pr == VK_SUBOPTIMAL_KHR || pr == VK_ERROR_OUT_OF_DATE_KHR)
			needRecreate = true;
		else if (pr < 0 && pr != VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT)
			fatal(kExitVulkan, "vkQueuePresentKHR failed: VkResult %d", int(pr));

		// 6. poll present timing every frame, as DXVK does
		at("loop");
		pollTiming(false);
	}

	const uint64_t tEnd = monotonicNs();

	// Finish: GPU idle, last timestamps, and give the driver time to report the final presents.
	at("vkDeviceWaitIdle(exit)");
	if (device)
		vkDeviceWaitIdle(device);
	for (auto &s : slots)
		harvest(s);
	at("final timing drain");
	if (swapUsesTiming && swapchain) {
		uint64_t until = monotonicNs() + 1'000'000'000ull;
		for (;;) {
			pollTiming(true);
			bool pending = false;
			for (auto &r : rows)
				if (r.timing == Row::Pending) { pending = true; break; }
			if (!pending || monotonicNs() > until)
				break;
			struct timespec ts{ 0, 5'000'000 };
			nanosleep(&ts, nullptr);
		}
		for (auto &r : rows)
			if (r.timing == Row::Pending) {
				anomaly("missing", "present id %llu never reported", (unsigned long long)r.presentId);
				r.timing = Row::Abandoned;
			}
	}

	writeLog();
	summary(double(tEnd - tStart) / 1e9);
	if (g_signal.load())
		printf("[vk_truth] stopped by signal %d\n", g_signal.load());
	return exitCode();
}

} // namespace

int main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0); // keep progress visible when piped or killed
	Options opt;
	if (!parseArgs(argc, argv, opt))
		return kExitUsage;
	if (opt.help) {
		printHelp();
		return kExitOk;
	}
	App app(opt);
	int code;
	try {
		code = app.run();
	} catch (const Fatal &f) {
		fprintf(stderr, "[vk_truth] error: %s\n", f.msg.c_str());
		code = f.code;
	}
	app.cleanup();
	return code;
}
