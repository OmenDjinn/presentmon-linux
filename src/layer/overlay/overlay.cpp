// SPDX-License-Identifier: MIT
// In-game overlay: implements the seam declared in ../overlay_seam.hpp.
//
// What happens per present (onPresent, on the application's presenting thread):
//   1. read OverlayControl (one atomic load; the fields only when `generation` changed)
//   2. feed the queued finished records to the metrics engine, refresh the numbers
//   3. build the ImGui frame, record "font atlas upload (when ImGui asks for one) + render pass
//      (LOAD) + ImGui draw" into the command buffer of this swapchain image, submit it on the
//      present queue, waiting on the application's present semaphores and signalling our own, and
//      make the present wait on ours
// Nothing waits for the GPU except the per-image fence before a command buffer is reused (which
// has already signalled by the time the application re-acquires the image). Atlas uploads (a new
// backend, a new scale) go into that same command buffer; Dear ImGui's own upload path, which
// submits and then waits for the whole queue to go idle, is never used (see serveTextures).
//
// Colour: see shaders/overlay.frag. The render pass draws straight into the swapchain image with
// a fragment shader that encodes ImGui's sRGB colours for the swapchain's format and colour space.
// That is cheaper than an offscreen RGBA8 target plus a composite pass (no extra image, no second
// pass, no extra memory traffic on a 5k-pixel overlay) and it keeps blending in the target's own
// encoding. The cost is that blending of the translucent panel happens in PQ code values on HDR10
// (a mild approximation: it darkens the panel edge slightly differently than a linear blend would)
// and in linear light on scRGB and sRGB formats (exact).
#include "../overlay_seam.hpp"

#include "dump.hpp"
#include "stats.hpp"
#include "ui.hpp"

#include "../log.hpp"
#include "../present_chain.hpp"

#include <pml/clock.hpp>
#include <pml/metrics/adapter.hpp>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "ov_font.h"
#include "ov_frag_mode0.h"
#include "ov_frag_mode1.h"
#include "ov_frag_mode2.h"
#include "ov_frag_mode3.h"

namespace pml::layer::overlay {

namespace {

// ---------------------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------------------

bool envEquals(const char *name, const char *value)
{
	const char *v = std::getenv(name);
	return v && strcasecmp(v, value) == 0;
}

struct DumpConfig
{
	std::string path;
	uint64_t frame = 120;
	std::atomic<bool> done{false};
};

DumpConfig &dumpConfig()
{
	static DumpConfig *cfg = [] {
		auto *c = new DumpConfig();
		if (const char *p = std::getenv("PRESENTMON_OVERLAY_DUMP"))
			c->path = p;
		if (const char *f = std::getenv("PRESENTMON_OVERLAY_DUMP_FRAME"))
			if (long n = std::atol(f); n > 0)
				c->frame = uint64_t(n);
		return c;
	}();
	return *cfg;
}

// ---------------------------------------------------------------------------------------
// Format / colour space
// ---------------------------------------------------------------------------------------

enum class ColorMode : int { Unorm = 0, Srgb = 1, Pq = 2, ScRgb = 3 };

bool colorModeFor(VkFormat format, VkColorSpaceKHR cs, ColorMode *mode)
{
	switch (cs)
	{
	case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
		switch (format)
		{
		case VK_FORMAT_B8G8R8A8_SRGB:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
			*mode = ColorMode::Srgb;
			return true;
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
		case VK_FORMAT_R16G16B16A16_SFLOAT:
			*mode = ColorMode::Unorm;
			return true;
		default:
			return false;
		}
	case VK_COLOR_SPACE_HDR10_ST2084_EXT:
		if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)
		{
			*mode = ColorMode::Pq;
			return true;
		}
		return false;
	case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
		if (format == VK_FORMAT_R16G16B16A16_SFLOAT)
		{
			*mode = ColorMode::ScRgb;
			return true;
		}
		return false;
	default:
		return false;
	}
}

VkShaderModuleCreateInfo fragmentShaderFor(ColorMode mode)
{
	VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	switch (mode)
	{
	case ColorMode::Unorm: ci.codeSize = sizeof(spv_overlay_frag_mode0); ci.pCode = spv_overlay_frag_mode0; break;
	case ColorMode::Srgb:  ci.codeSize = sizeof(spv_overlay_frag_mode1); ci.pCode = spv_overlay_frag_mode1; break;
	case ColorMode::Pq:    ci.codeSize = sizeof(spv_overlay_frag_mode2); ci.pCode = spv_overlay_frag_mode2; break;
	case ColorMode::ScRgb: ci.codeSize = sizeof(spv_overlay_frag_mode3); ci.pCode = spv_overlay_frag_mode3; break;
	}
	return ci;
}

const char *formatName(uint32_t f)
{
	switch (VkFormat(f))
	{
	case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
	case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
	case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
	case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
	case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return "A8B8G8R8_UNORM";
	case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return "A8B8G8R8_SRGB";
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10";
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10";
	case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
	default: return "format?";
	}
}

const char *colorSpaceName(uint32_t cs)
{
	switch (VkColorSpaceKHR(cs))
	{
	case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR: return "sRGB";
	case VK_COLOR_SPACE_HDR10_ST2084_EXT: return "HDR10 PQ";
	case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: return "scRGB";
	default: return "colorspace?";
	}
}

// ---------------------------------------------------------------------------------------
// Vulkan function table (next layer / driver; never the loader's exported symbols)
// ---------------------------------------------------------------------------------------

#define OV_DEVICE_FUNCTIONS(X) \
	X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
	X(CreateImageView) X(DestroyImageView) X(CreateCommandPool) X(DestroyCommandPool) \
	X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) \
	X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdPipelineBarrier) X(CmdCopyImageToBuffer) \
	X(QueueSubmit) X(CreateFence) X(DestroyFence) X(ResetFences) X(WaitForFences) X(GetFenceStatus) \
	X(CreateSemaphore) X(DestroySemaphore) X(CreateBuffer) X(DestroyBuffer) \
	X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) \
	X(MapMemory) X(UnmapMemory) X(InvalidateMappedMemoryRanges) X(FlushMappedMemoryRanges) X(ResetCommandBuffer) \
	X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) X(CmdCopyBufferToImage)

struct Fn
{
#define X(name) PFN_vk##name name = nullptr;
	OV_DEVICE_FUNCTIONS(X)
#undef X
};

struct OvDevice
{
	DeviceContext *ctx = nullptr;
	Fn fn;
	PFN_vkGetPhysicalDeviceFormatProperties getFormatProperties = nullptr;
	std::vector<VkQueueFamilyProperties> families;
	VkPhysicalDeviceMemoryProperties memory = {};
	bool ok = false;
};

// Resolves through the next layer; instance level functions (not known to the device proc addr)
// fall back to the next layer's instance proc addr.
PFN_vkVoidFunction resolve(const DeviceContext &d, const char *name)
{
	PFN_vkVoidFunction f = d.getDeviceProcAddr ? d.getDeviceProcAddr(d.device, name) : nullptr;
	if (!f && d.getInstanceProcAddr)
		f = d.getInstanceProcAddr(d.instance, name);
	return f;
}

OvDevice *deviceOf(DeviceContext &d)
{
	return static_cast<OvDevice *>(d.userData);
}

// Dear ImGui keeps its Vulkan function table and its current context global, so every use of it is
// under gImguiMutex; this is the device the locked section works for (see imguiAllocateCommandBuffers).
DeviceContext *gImguiDevice = nullptr;

// Dear ImGui's texture upload allocates a command buffer behind the loader's back; like our own
// buffers it needs the loader's dispatch pointer.
VKAPI_ATTR VkResult VKAPI_CALL imguiAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *info,
                                                           VkCommandBuffer *buffers)
{
	DeviceContext *dc = gImguiDevice;
	OvDevice *d = dc ? deviceOf(*dc) : nullptr;
	if (!d || !d->fn.AllocateCommandBuffers)
		return VK_ERROR_INITIALIZATION_FAILED;
	const VkResult r = d->fn.AllocateCommandBuffers(device, info, buffers);
	if (r == VK_SUCCESS && dc->setDeviceLoaderData)
		for (uint32_t i = 0; i < info->commandBufferCount; i++)
			dc->setDeviceLoaderData(device, buffers[i]);
	return r;
}

PFN_vkVoidFunction imguiLoader(const char *name, void *user)
{
	if (!std::strcmp(name, "vkAllocateCommandBuffers"))
		return reinterpret_cast<PFN_vkVoidFunction>(imguiAllocateCommandBuffers);
	return resolve(*static_cast<const DeviceContext *>(user), name);
}

// ---------------------------------------------------------------------------------------
// Per swapchain state
// ---------------------------------------------------------------------------------------

struct ImageSlot
{
	VkImage image = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	uint32_t cmdFamily = UINT32_MAX;
	VkSemaphore semaphore = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	bool submitted = false;   // fence is armed
	uint64_t serial = 0;      // OvSwapchain::submitSerial of that submission
};

// A font atlas image (and its descriptor set from the backend's pool), or a staging buffer, or both.
struct OvTexture
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
};

// Freed once every overlay submission up to `serial` has finished on the GPU.
struct Garbage
{
	uint64_t serial;
	OvTexture tex;
};

struct Pool
{
	uint32_t family;
	VkCommandPool pool;
};

struct OvControl
{
	uint32_t generation = ~0u;
	uint32_t visible = 1;
	pml::OverlayPosition position = pml::OverlayPosition::TopLeft;
	uint32_t scalePermille = 0;
	uint32_t panels = 0;
};

struct OvSwapchain
{
	OvDevice *dev = nullptr;
	SwapchainContext *sc = nullptr;
	ColorMode mode = ColorMode::Unorm;
	uint32_t width = 0, height = 0;
	VkFormat format = VK_FORMAT_UNDEFINED;
	bool canDump = false;

	VkRenderPass renderPass = VK_NULL_HANDLE;
	std::vector<ImageSlot> slots;
	std::vector<Pool> pools;

	ImGuiContext *imgui = nullptr;
	ImFont *font = nullptr;
	bool imguiReady = false;
	bool counted = false;                // included in gLiveSwapchains
	bool failed = false;          // disabled for the rest of this swapchain's life
	float bakedScale = 0;
	uint32_t forkGeneration = 0;  // gForkGeneration when created; differs in a fork()ed child

	std::unordered_map<int, OvTexture> textures;   // ImTextureData::UniqueID -> its current image
	std::vector<Garbage> garbage;
	uint64_t submitSerial = 0;    // overlay submissions so far
	uint64_t uploadSerial = 0;    // submission that did the latest atlas upload, until it is known to be done
	VkQueue uploadQueue = VK_NULL_HANDLE;   // ... and the queue it went to

	std::shared_ptr<OverlayStats> stats;
	OvControl control;
	bool wasVisible = true;
	uint64_t presents = 0;
	uint64_t lastFrameNs = 0;
	bool warnedNoGraphics = false;
};

std::mutex gImguiMutex;   // Dear ImGui has one global current context; also guards the backend's static function table

std::mutex gRegistryMutex;
std::unordered_map<uint64_t, std::shared_ptr<OverlayStats>> gStats;   // swapchain serial -> stats
int gLiveSwapchains = 0;   // swapchains that can draw (gRegistryMutex); the segment's "overlay available" flag follows it

// fork(): the child inherits the parent's swapchains, their cached segment pointers (the layer frees
// that segment in the child) and possibly locked mutexes. The child handler gives it fresh locks and
// an empty registry, and swapchains created before the fork stay inert there: no drawing, no
// segment access, and their state is leaked rather than torn down (its locks may be held).
std::atomic<uint32_t> gForkGeneration{0};
std::once_flag gForkOnce;

void childAfterFork()
{
	new (&gImguiMutex) std::mutex;
	new (&gRegistryMutex) std::mutex;
	new (&gStats) std::unordered_map<uint64_t, std::shared_ptr<OverlayStats>>();   // the old one is leaked
	gLiveSwapchains = 0;
	gImguiDevice = nullptr;
	gForkGeneration.fetch_add(1, std::memory_order_relaxed);
}

bool inherited(const OvSwapchain &s)
{
	return s.forkGeneration != gForkGeneration.load(std::memory_order_relaxed);
}

// An overlay swapchain stops drawing (failed or destroyed): the flag `presentmon overlay` reports
// goes off with the last one.
void retireSwapchain(OvSwapchain &s)
{
	std::lock_guard<std::mutex> lock(gRegistryMutex);
	if (!s.counted)
		return;
	s.counted = false;
	if (--gLiveSwapchains == 0 && s.sc && s.sc->segment)
		pml::setOverlayAvailable(*s.sc->segment, false);
}

void markFailed(OvSwapchain &s)
{
	s.failed = true;
	retireSwapchain(s);
}

// ---------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------

float effectiveScale(const OvControl &c, uint32_t width, uint32_t height)
{
	float s = c.scalePermille ? float(c.scalePermille) / 1000.0f : autoOverlayScale(width, height);
	s = std::clamp(s, 0.5f, 4.0f);
	return std::round(s * 20.0f) / 20.0f;   // few distinct sizes -> few font bakes
}

void pollControl(OvSwapchain &s)
{
	pml::Segment *seg = s.sc->segment;
	if (!seg)
		return;
	const uint32_t gen = pml::atomicLoad(seg->header().overlay.generation);
	if (gen == s.control.generation)
		return;
	const pml::OverlayControl c = pml::readOverlayControl(*seg);
	s.control.generation = c.generation;
	s.control.visible = c.visible;
	s.control.position = pml::OverlayPosition(std::min<uint32_t>(c.position, 3));
	s.control.scalePermille = c.scalePermille;
	s.control.panels = c.panels;
}

bool isGraphicsFamily(const OvDevice &d, uint32_t family)
{
	return family < d.families.size() && (d.families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT);
}

bool ensurePool(OvSwapchain &s, uint32_t family, VkCommandPool *out)
{
	for (const Pool &p : s.pools)
		if (p.family == family)
		{
			*out = p.pool;
			return true;
		}
	VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	ci.queueFamilyIndex = family;
	VkCommandPool pool = VK_NULL_HANDLE;
	if (s.dev->fn.CreateCommandPool(s.dev->ctx->device, &ci, nullptr, &pool) != VK_SUCCESS)
		return false;
	s.pools.push_back({family, pool});
	*out = pool;
	return true;
}

// Creates what an image slot needs for drawing on the queue family `family`.
bool prepareSlot(OvSwapchain &s, ImageSlot &slot, uint32_t family)
{
	OvDevice &d = *s.dev;
	VkDevice device = d.ctx->device;
	if (slot.cmd == VK_NULL_HANDLE || slot.cmdFamily != family)
	{
		VkCommandPool pool;
		if (!ensurePool(s, family, &pool))
			return false;
		if (slot.cmd != VK_NULL_HANDLE)
		{
			// Family changed (very unusual): give the old buffer back to its pool.
			for (const Pool &p : s.pools)
				if (p.family == slot.cmdFamily)
					d.fn.FreeCommandBuffers(device, p.pool, 1, &slot.cmd);
			slot.cmd = VK_NULL_HANDLE;
		}
		VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ai.commandPool = pool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		if (d.fn.AllocateCommandBuffers(device, &ai, &slot.cmd) != VK_SUCCESS)
		{
			slot.cmd = VK_NULL_HANDLE;
			return false;
		}
		// The loader's dispatch pointer must be written into every dispatchable object created behind its back.
		if (d.ctx->setDeviceLoaderData)
			d.ctx->setDeviceLoaderData(device, slot.cmd);
		slot.cmdFamily = family;
	}
	if (slot.semaphore == VK_NULL_HANDLE)
	{
		VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		if (d.fn.CreateSemaphore(device, &ci, nullptr, &slot.semaphore) != VK_SUCCESS)
			return false;
	}
	if (slot.fence == VK_NULL_HANDLE)
	{
		VkFenceCreateInfo ci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if (d.fn.CreateFence(device, &ci, nullptr, &slot.fence) != VK_SUCCESS)
			return false;
	}
	if (slot.view == VK_NULL_HANDLE)
	{
		VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		ci.image = slot.image;
		ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ci.format = s.format;   // must equal the swapchain format (MUTABLE_FORMAT swapchains too)
		ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		if (d.fn.CreateImageView(device, &ci, nullptr, &slot.view) != VK_SUCCESS)
			return false;
	}
	if (slot.framebuffer == VK_NULL_HANDLE)
	{
		VkFramebufferCreateInfo ci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		ci.renderPass = s.renderPass;
		ci.attachmentCount = 1;
		ci.pAttachments = &slot.view;
		ci.width = s.width;
		ci.height = s.height;
		ci.layers = 1;
		if (d.fn.CreateFramebuffer(device, &ci, nullptr, &slot.framebuffer) != VK_SUCCESS)
			return false;
	}
	return true;
}

void destroySlots(OvSwapchain &s)
{
	OvDevice &d = *s.dev;
	VkDevice device = d.ctx->device;
	// Fences first: a command buffer that is still executing must not be destroyed under the GPU.
	// The application has to idle before destroying a swapchain; this only covers a short remainder.
	for (ImageSlot &slot : s.slots)
		if (slot.submitted && slot.fence)
			d.fn.WaitForFences(device, 1, &slot.fence, VK_TRUE, 500'000'000ull);
	for (ImageSlot &slot : s.slots)
	{
		if (slot.framebuffer) d.fn.DestroyFramebuffer(device, slot.framebuffer, nullptr);
		if (slot.view) d.fn.DestroyImageView(device, slot.view, nullptr);
		if (slot.semaphore) d.fn.DestroySemaphore(device, slot.semaphore, nullptr);
		if (slot.fence) d.fn.DestroyFence(device, slot.fence, nullptr);
		slot = ImageSlot{};
	}
	for (const Pool &p : s.pools)
		d.fn.DestroyCommandPool(device, p.pool, nullptr);   // frees its command buffers
	s.pools.clear();
}

// ---------------------------------------------------------------------------------------
// ImGui backend
// ---------------------------------------------------------------------------------------

// gImguiMutex held.
bool initImGui(OvSwapchain &s, VkQueue queue, uint32_t family)
{
	OvDevice &d = *s.dev;
	DeviceContext &dc = *d.ctx;

	if (!ImGui_ImplVulkan_LoadFunctions(dc.apiVersion, imguiLoader, &dc))
	{
		PML_LOG(LogLevel::Warn, "overlay: cannot resolve the Vulkan functions Dear ImGui needs");
		return false;
	}

	s.imgui = ImGui::CreateContext();
	ImGui::SetCurrentContext(s.imgui);
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
	ImGui::StyleColorsDark();

	ImFontConfig fc;
	fc.FontDataOwnedByAtlas = false;   // the bytes live in this library
	fc.OversampleH = 2;
	fc.OversampleV = 1;
	s.font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(ov_font), int(ov_font_size), 0.0f, &fc);

	ImGui_ImplVulkan_InitInfo info = {};
	info.ApiVersion = dc.apiVersion;
	info.Instance = dc.instance;
	info.PhysicalDevice = dc.physicalDevice;
	info.Device = dc.device;
	info.QueueFamily = family;
	info.Queue = queue;
	info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
	info.MinImageCount = std::max<uint32_t>(2, uint32_t(s.slots.size()));
	// One more vertex/index buffer set than images: a buffer set is reused after ImageCount draws,
	// by when the frame that used it has finished (it held one of the images).
	info.ImageCount = info.MinImageCount + 1;
	// ImGui grows a vertex/index buffer to exactly what a frame needs and re-creates it (vkFreeMemory +
	// vkAllocateMemory, ~150 us) whenever a later frame needs a few bytes more. A generous minimum makes
	// every buffer big enough for the whole overlay from the start.
	info.MinAllocationSize = 256 * 1024;
	info.PipelineInfoMain.RenderPass = s.renderPass;
	info.PipelineInfoMain.Subpass = 0;
	info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	info.CustomShaderFragCreateInfo = fragmentShaderFor(s.mode);
	// The queue is required by Init but never used: the backend only submits to upload textures, and
	// the overlay serves every texture request itself (serveTextures).
	const uint64_t t0 = pml::monotonicNs();
	if (!ImGui_ImplVulkan_Init(&info))
	{
		PML_LOG(LogLevel::Warn, "overlay: ImGui_ImplVulkan_Init failed");
		ImGui::DestroyContext(s.imgui);
		s.imgui = nullptr;
		return false;
	}
	PML_LOG(LogLevel::Debug, "overlay: ImGui_ImplVulkan_Init (pipeline, sampler, descriptor pool) took %.2f ms",
	        double(pml::monotonicNs() - t0) * 1e-6);
	s.imguiReady = true;
	return true;
}

// gImguiMutex held, context current. Bakes every printable glyph at the current scale, so the atlas
// is complete before the first visible frame and no glyph is added mid-frame later. The atlas is
// uploaded by the next serveTextures, in the present's own command buffer.
void warmUp(OvSwapchain &s, float scale)
{
	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2(float(s.width), float(s.height));
	io.DeltaTime = 1.0f / 60.0f;
	const float px = kBaseFontPx * scale;
	ImGui_ImplVulkan_NewFrame();
	for (int i = 0; i < 3; i++)   // auto-sized windows are hidden for their first frame
	{
		ImGui::NewFrame();
		ImGui::PushFont(s.font, px);
		if (ImFontBaked *baked = ImGui::GetFont()->GetFontBaked(px))
			for (const char *c = glyphWarmupText(); *c; c++)
				baked->FindGlyph(ImWchar(*c));
		Snapshot empty;
		UiParams up;
		up.snapshot = &empty;
		up.panels = kDefaultPanels;
		up.scale = scale;
		up.displayWidth = float(s.width);
		up.displayHeight = float(s.height);
		drawOverlayUi(up);
		ImGui::PopFont();
		ImGui::Render();
	}
	s.bakedScale = scale;
}

// ---------------------------------------------------------------------------------------
// Font atlas textures
// ---------------------------------------------------------------------------------------
// Dear ImGui asks for texture creation, updates and destruction through ImTextureData::Status. Its
// Vulkan backend would serve them inside RenderDrawData with vkQueueSubmit + vkQueueWaitIdle on the
// queue it was initialised with: a CPU stall until all queued GPU work (the game's frames too) is
// done, on a queue another thread may own. The overlay serves them itself instead, the way MangoHud
// and Mesa's overlay layer upload their fonts: every new or changed atlas becomes a new image whose
// upload is recorded into the present's command buffer ahead of the render pass. Whatever frames in
// flight may still read (a replaced image, a staging buffer) goes to `garbage` and is freed once the
// fences show the GPU is past it. BackendUserData stays null, so the backend never touches these
// images; ImDrawData::Textures is cleared before RenderDrawData so it never uploads either.

uint32_t memoryType(const OvDevice &d, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback)
{
	for (VkMemoryPropertyFlags w : {want, fallback})
		for (uint32_t i = 0; i < d.memory.memoryTypeCount; i++)
			if ((bits & (1u << i)) && (d.memory.memoryTypes[i].propertyFlags & w) == w)
				return i;
	return UINT32_MAX;
}

// gImguiMutex held, the swapchain's context current (the descriptor set goes back to its pool).
void destroyTexture(OvSwapchain &s, OvTexture &t)
{
	const Fn &fn = s.dev->fn;
	VkDevice device = s.dev->ctx->device;
	if (t.set) ImGui_ImplVulkan_RemoveTexture(t.set);
	if (t.view) fn.DestroyImageView(device, t.view, nullptr);
	if (t.image) fn.DestroyImage(device, t.image, nullptr);
	if (t.memory) fn.FreeMemory(device, t.memory, nullptr);
	if (t.staging) fn.DestroyBuffer(device, t.staging, nullptr);
	if (t.stagingMemory) fn.FreeMemory(device, t.stagingMemory, nullptr);
	t = OvTexture{};
}

// Creates an RGBA8 image for `tex` and a staging buffer with its pixels, and records the upload
// into `cmd` (outside any render pass). The staging buffer is returned separately in `staging`.
bool createTexture(OvSwapchain &s, VkCommandBuffer cmd, ImTextureData &tex, OvTexture *out, OvTexture *staging)
{
	OvDevice &d = *s.dev;
	VkDevice device = d.ctx->device;
	const uint32_t w = uint32_t(tex.Width), h = uint32_t(tex.Height);
	const VkDeviceSize size = VkDeviceSize(w) * h * 4;

	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = VK_FORMAT_R8G8B8A8_UNORM;
	ii.extent = {w, h, 1};
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (d.fn.CreateImage(device, &ii, nullptr, &out->image) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	d.fn.GetImageMemoryRequirements(device, out->image, &req);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = memoryType(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
	if (ai.memoryTypeIndex == UINT32_MAX || d.fn.AllocateMemory(device, &ai, nullptr, &out->memory) != VK_SUCCESS ||
	    d.fn.BindImageMemory(device, out->image, out->memory, 0) != VK_SUCCESS)
		return false;
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = out->image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = VK_FORMAT_R8G8B8A8_UNORM;
	vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (d.fn.CreateImageView(device, &vi, nullptr, &out->view) != VK_SUCCESS)
		return false;

	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = size;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (d.fn.CreateBuffer(device, &bi, nullptr, &staging->staging) != VK_SUCCESS)
		return false;
	d.fn.GetBufferMemoryRequirements(device, staging->staging, &req);
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = memoryType(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	if (ai.memoryTypeIndex == UINT32_MAX || d.fn.AllocateMemory(device, &ai, nullptr, &staging->stagingMemory) != VK_SUCCESS ||
	    d.fn.BindBufferMemory(device, staging->staging, staging->stagingMemory, 0) != VK_SUCCESS)
		return false;
	void *mapped = nullptr;
	if (d.fn.MapMemory(device, staging->stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
		return false;
	std::memcpy(mapped, tex.GetPixels(), size_t(size));
	if (!(d.memory.memoryTypes[ai.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
	{
		VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
		range.memory = staging->stagingMemory;
		range.size = VK_WHOLE_SIZE;
		d.fn.FlushMappedMemoryRanges(device, 1, &range);
	}
	d.fn.UnmapMemory(device, staging->stagingMemory);

	// The image is new, so nothing can be using it; the host writes are visible to the device at submission.
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = 0;
	b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = out->image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	d.fn.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
	                        1, &b);
	VkBufferImageCopy region = {};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {w, h, 1};
	d.fn.CmdCopyBufferToImage(cmd, staging->staging, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	d.fn.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
	                        nullptr, 1, &b);

	out->set = ImGui_ImplVulkan_AddTexture(out->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	return out->set != VK_NULL_HANDLE;
}

// gImguiMutex held, context current. Frees the garbage no submission in flight can still use (all of
// it with `all`, after the caller waited for the fences).
void collectGarbage(OvSwapchain &s, bool all)
{
	if (s.garbage.empty())
		return;
	uint64_t oldest = UINT64_MAX;   // the oldest submission that may still be executing
	if (!all)
	{
		oldest = s.submitSerial + 1;
		for (const ImageSlot &slot : s.slots)
			if (slot.submitted && slot.serial < oldest &&
			    s.dev->fn.GetFenceStatus(s.dev->ctx->device, slot.fence) != VK_SUCCESS)
				oldest = slot.serial;
	}
	size_t keep = 0;
	for (Garbage &g : s.garbage)
	{
		if (g.serial < oldest)
			destroyTexture(s, g.tex);
		else
			s.garbage[keep++] = g;
	}
	s.garbage.resize(keep);
}

// gImguiMutex held, context current, after ImGui::Render() and before the render pass. Serves every
// texture request of this frame; false when a texture could not be created.
bool serveTextures(OvSwapchain &s, VkCommandBuffer cmd, VkQueue queue)
{
	const uint64_t serial = s.submitSerial + 1;   // the submission this command buffer will be
	for (ImTextureData *tex : ImGui::GetPlatformIO().Textures)
	{
		if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates)
		{
			if (tex->Format != ImTextureFormat_RGBA32 || !tex->Pixels || tex->Width <= 0 || tex->Height <= 0)
				return false;
			// A change is uploaded as a whole new image: frames in flight may still read the old one.
			OvTexture fresh, staging;
			const bool ok = createTexture(s, cmd, *tex, &fresh, &staging);
			s.garbage.push_back({serial, staging});
			if (!ok)
			{
				s.garbage.push_back({serial, fresh});
				return false;
			}
			auto it = s.textures.find(tex->UniqueID);
			if (it != s.textures.end())
			{
				s.garbage.push_back({serial, it->second});
				it->second = fresh;
			}
			else
			{
				s.textures.emplace(tex->UniqueID, fresh);
			}
			tex->SetTexID((ImTextureID)fresh.set);
			tex->SetStatus(ImTextureStatus_OK);
			s.uploadSerial = serial;
			s.uploadQueue = queue;
		}
		else if (tex->Status == ImTextureStatus_WantDestroy)
		{
			// Not used by this frame (UnusedFrames > 0); earlier frames may still be reading it.
			auto it = s.textures.find(tex->UniqueID);
			if (it != s.textures.end())
			{
				s.garbage.push_back({serial, it->second});
				s.textures.erase(it);
			}
			tex->SetTexID(ImTextureID_Invalid);
			tex->SetStatus(ImTextureStatus_Destroyed);
		}
	}
	return true;
}

// An atlas image is written by the submission that uploaded it, on that present's queue, and ordered before
// later draws only on the same queue. A swapchain presented from two queues (legal, rare) would sample it from
// the other queue with no ordering, so a present on another queue first waits for the upload's fence while it
// may still be running: once per atlas change, as uploads only happen then. Only the latest upload is
// tracked, which covers the one font atlas the overlay has (an upload replaces the whole image); more
// textures would need a serial and queue per texture. A wait that times out (100 ms) draws anyway.
void waitForUploadOnOtherQueue(OvSwapchain &s, VkQueue queue)
{
	if (!s.uploadSerial || queue == s.uploadQueue)
		return;
	for (const ImageSlot &slot : s.slots)
		if (slot.submitted && slot.serial == s.uploadSerial)
			s.dev->fn.WaitForFences(s.dev->ctx->device, 1, &slot.fence, VK_TRUE, 100'000'000ull);
	// Done, or its slot was reused (which waited for its fence first).
	s.uploadSerial = 0;
}

// gImguiMutex held. The GPU must be done with the swapchain's submissions (destroySlots waited).
void shutdownImGui(OvSwapchain &s)
{
	if (!s.imgui)
		return;
	ImGui::SetCurrentContext(s.imgui);
	if (s.imguiReady)
	{
		collectGarbage(s, true);
		for (auto &kv : s.textures)
			destroyTexture(s, kv.second);
		s.textures.clear();
		ImGui_ImplVulkan_Shutdown();
	}
	ImGui::DestroyContext(s.imgui);
	s.imgui = nullptr;
	s.imguiReady = false;
}

// ---------------------------------------------------------------------------------------
// Description lines
// ---------------------------------------------------------------------------------------

void describe(const OvSwapchain &s, UiParams *up)
{
	const pml::SwapchainInfo &info = s.sc->info;
	uint32_t mode = info.presentMode;
	if (s.stats)
	{
		const uint32_t last = s.stats->lastPresentMode.load(std::memory_order_relaxed);
		if (last != UINT32_MAX)
			mode = last;
	}
	char buf[128];
	std::snprintf(buf, sizeof(buf), "%s  %s  %ux%u", pml::metrics::runtimeName(info.engineName),
	              pml::metrics::presentModeName(mode), info.width, info.height);
	up->runtimeLine = asciiOnly(buf);
	std::snprintf(buf, sizeof(buf), "%s  %s", formatName(info.format), colorSpaceName(info.colorSpace));
	up->formatLine = buf;

	const bool timing = (info.timingFlags & (pml::kSwapchainLayerTiming | pml::kSwapchainAppTiming)) != 0;
	const uint32_t used = info.timingStagesUsed;
	const bool pixelOut = (used & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT) != 0;
	const bool pixelVisible = (used & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT) != 0;
	up->displayTiming = timing && (pixelOut || pixelVisible);
	if (up->displayTiming)
		up->timingLine = pixelVisible ? "timing: pixel-visible + queue-end" : "timing: pixel-out + queue-end";
	else if (timing && (used & VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT))
		up->timingLine = "timing: queue-end only, no screen";
	else
		up->timingLine = "timing: none";
}

// ---------------------------------------------------------------------------------------
// Readback for the dump hook
// ---------------------------------------------------------------------------------------

struct Readback
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDeviceSize size = 0;
	bool coherent = true;
};

bool createReadback(OvSwapchain &s, uint32_t bpp, Readback *rb)
{
	OvDevice &d = *s.dev;
	VkDevice device = d.ctx->device;
	rb->size = VkDeviceSize(s.width) * s.height * bpp;
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = rb->size;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (d.fn.CreateBuffer(device, &bi, nullptr, &rb->buffer) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req;
	d.fn.GetBufferMemoryRequirements(device, rb->buffer, &req);
	// Prefer cached host memory (fast CPU reads); fall back to any host-visible type.
	uint32_t type = UINT32_MAX;
	for (int pass = 0; pass < 2 && type == UINT32_MAX; pass++)
	{
		const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                   (pass == 0 ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
		for (uint32_t i = 0; i < d.memory.memoryTypeCount; i++)
			if ((req.memoryTypeBits & (1u << i)) && (d.memory.memoryTypes[i].propertyFlags & want) == want)
			{
				type = i;
				rb->coherent = (d.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
				break;
			}
	}
	if (type == UINT32_MAX)
		return false;
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = type;
	if (d.fn.AllocateMemory(device, &ai, nullptr, &rb->memory) != VK_SUCCESS)
		return false;
	return d.fn.BindBufferMemory(device, rb->buffer, rb->memory, 0) == VK_SUCCESS;
}

void destroyReadback(OvSwapchain &s, Readback *rb)
{
	VkDevice device = s.dev->ctx->device;
	if (rb->buffer) s.dev->fn.DestroyBuffer(device, rb->buffer, nullptr);
	if (rb->memory) s.dev->fn.FreeMemory(device, rb->memory, nullptr);
	*rb = Readback{};
}

void recordDumpCopy(OvSwapchain &s, VkCommandBuffer cmd, VkImage image, VkBuffer buffer)
{
	const Fn &fn = s.dev->fn;
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	fn.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
	                      nullptr, 1, &b);
	VkBufferImageCopy region = {};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {s.width, s.height, 1};
	fn.CmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
	VkImageMemoryBarrier back = b;
	back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	back.dstAccessMask = 0;
	back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	fn.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
	                      nullptr, 1, &back);
	// Make the copy visible to the host once the fence signals.
	VkBufferMemoryBarrier hb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
	hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	hb.buffer = buffer;
	hb.size = VK_WHOLE_SIZE;
	fn.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0,
	                      nullptr);
}

void finishDump(OvSwapchain &s, ImageSlot &slot, Readback &rb, uint32_t bpp)
{
	OvDevice &d = *s.dev;
	VkDevice device = d.ctx->device;
	DumpConfig &cfg = dumpConfig();
	// Test hook only: this is the one place that blocks on the GPU.
	if (d.fn.WaitForFences(device, 1, &slot.fence, VK_TRUE, 5'000'000'000ull) != VK_SUCCESS)
	{
		PML_LOG(LogLevel::Warn, "overlay: dump readback timed out");
		return;
	}
	void *mapped = nullptr;
	if (d.fn.MapMemory(device, rb.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
		return;
	if (!rb.coherent)
	{
		VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
		range.memory = rb.memory;
		range.size = VK_WHOLE_SIZE;
		d.fn.InvalidateMappedMemoryRanges(device, 1, &range);
	}
	DumpImage img;
	img.vkFormat = uint32_t(s.format);
	img.vkColorSpace = s.sc->info.colorSpace;
	img.width = s.width;
	img.height = s.height;
	img.frameIndex = uint32_t(s.presents);
	img.bytes.assign(static_cast<const uint8_t *>(mapped), static_cast<const uint8_t *>(mapped) + size_t(s.width) * s.height * bpp);
	d.fn.UnmapMemory(device, rb.memory);
	const std::string err = writeDump(img, cfg.path);
	if (err.empty())
		PML_LOG(LogLevel::Info, "overlay: dumped present %llu of swapchain %llu to %s (+ .raw)",
		        (unsigned long long)s.presents, (unsigned long long)s.sc->info.serial, cfg.path.c_str());
	else
		PML_LOG(LogLevel::Warn, "overlay: dump failed: %s", err.c_str());
}

} // namespace

// ---------------------------------------------------------------------------------------
// The seam
// ---------------------------------------------------------------------------------------

bool enabled()
{
	return !envEquals("PRESENTMON_OVERLAY", "off");
}

void onDeviceCreated(DeviceContext &device)
{
	std::call_once(gForkOnce, [] { pthread_atfork(nullptr, nullptr, childAfterFork); });
	auto *d = new OvDevice();
	d->ctx = &device;
	bool ok = true;
#define X(name) \
	d->fn.name = reinterpret_cast<PFN_vk##name>(resolve(device, "vk" #name)); \
	ok = ok && d->fn.name != nullptr;
	OV_DEVICE_FUNCTIONS(X)
#undef X
	d->getFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
	    device.getInstanceProcAddr(device.instance, "vkGetPhysicalDeviceFormatProperties"));
	ok = ok && d->getFormatProperties && device.instanceTable && device.instanceTable->GetPhysicalDeviceQueueFamilyProperties &&
	     device.instanceTable->GetPhysicalDeviceMemoryProperties && device.setDeviceLoaderData;
	if (ok)
	{
		uint32_t n = 0;
		device.instanceTable->GetPhysicalDeviceQueueFamilyProperties(device.physicalDevice, &n, nullptr);
		d->families.resize(n);
		device.instanceTable->GetPhysicalDeviceQueueFamilyProperties(device.physicalDevice, &n, d->families.data());
		device.instanceTable->GetPhysicalDeviceMemoryProperties(device.physicalDevice, &d->memory);
	}
	else
	{
		PML_LOG(LogLevel::Warn, "overlay: missing Vulkan functions, no overlay on this device");
	}
	d->ok = ok;
	device.userData = d;
}

void onDeviceDestroying(DeviceContext &device)
{
	delete deviceOf(device);
	device.userData = nullptr;
}

// Note on PRESENTMON_OVERLAY=0 (hidden): the overlay is loaded, so this adds the colour-attachment
// usage and onSwapchainCreated builds the per-swapchain state, because the usage of a swapchain is
// fixed at creation and `presentmon overlay` can show the overlay at any time. Only
// PRESENTMON_OVERLAY=off leaves swapchains untouched. If the driver refuses the swapchain with the
// added usage, the layer retries without it (hookCreateSwapchain).
bool onSwapchainCreateInfo(DeviceContext &device, VkSwapchainCreateInfoKHR *info)
{
	OvDevice *d = deviceOf(device);
	if (!d || !d->ok)
		return false;
	ColorMode mode;
	if (!colorModeFor(info->imageFormat, info->imageColorSpace, &mode))
	{
		PML_LOG(LogLevel::Info, "overlay: format %d / colour space %d not supported, no overlay on this swapchain",
		        int(info->imageFormat), int(info->imageColorSpace));
		return false;
	}
	if ((info->flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR) || info->imageArrayLayers != 1)
		return false;
	// A shared presentable image lives in SHARED_PRESENT_KHR layout and, in continuous mode, is shown
	// without presents: the overlay's render pass (PRESENT_SRC_KHR in and out) does not apply. Shared
	// modes are only compatible with themselves, so a mode switch cannot lead into one later.
	if (info->presentMode == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR ||
	    info->presentMode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR)
	{
		PML_LOG(LogLevel::Info, "overlay: shared present mode, no overlay on this swapchain");
		return false;
	}

	VkFormatProperties fp;
	d->getFormatProperties(device.physicalDevice, info->imageFormat, &fp);
	const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
	if ((fp.optimalTilingFeatures & need) != need)
		return false;

	// A mutable-format swapchain: every format its images can be viewed as must be drawable too.
	if (const auto *list = findStruct<VkImageFormatListCreateInfo>(info->pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO))
	{
		for (uint32_t i = 0; i < list->viewFormatCount && list->pViewFormats; i++)
		{
			VkFormatProperties vp;
			d->getFormatProperties(device.physicalDevice, list->pViewFormats[i], &vp);
			if ((vp.optimalTilingFeatures & need) != need)
				return false;
		}
	}

	VkSurfaceCapabilitiesKHR caps = {};
	if (!device.instanceTable->GetPhysicalDeviceSurfaceCapabilitiesKHR ||
	    device.instanceTable->GetPhysicalDeviceSurfaceCapabilitiesKHR(device.physicalDevice, info->surface, &caps) != VK_SUCCESS)
		return false;
	if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
		return false;

	info->imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (!dumpConfig().path.empty() && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
	    dumpBytesPerPixel(uint32_t(info->imageFormat)))
		info->imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	return true;
}

void onSwapchainCreated(SwapchainContext &sc)
{
	OvDevice *d = deviceOf(*sc.device);
	if (!d || !d->ok)
		return;
	auto s = std::make_unique<OvSwapchain>();
	s->dev = d;
	s->sc = &sc;
	s->forkGeneration = gForkGeneration.load(std::memory_order_relaxed);
	s->format = sc.createInfo.imageFormat;
	s->width = sc.createInfo.imageExtent.width;
	s->height = sc.createInfo.imageExtent.height;
	s->canDump = (sc.createInfo.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
	if (!colorModeFor(s->format, sc.createInfo.imageColorSpace, &s->mode))
		return;

	VkDevice device = sc.device->device;
	uint32_t count = 0;
	if (d->ctx->deviceTable->GetSwapchainImagesKHR(device, sc.swapchain, &count, nullptr) != VK_SUCCESS || count == 0)
		return;
	std::vector<VkImage> images(count);
	if (d->ctx->deviceTable->GetSwapchainImagesKHR(device, sc.swapchain, &count, images.data()) < 0)
		return;
	s->slots.resize(count);
	for (uint32_t i = 0; i < count; i++)
		s->slots[i].image = images[i];

	// One subpass drawing into the presented image. The image is already in PRESENT_SRC_KHR (that is
	// what the application presents), so the pass takes it from there and returns it there; LOAD
	// keeps the application's picture. No explicit layout barriers are needed.
	VkAttachmentDescription att = {};
	att.format = s->format;
	att.samples = VK_SAMPLE_COUNT_1_BIT;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub = {};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &ref;
	VkSubpassDependency deps[2] = {};
	deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	deps[0].dstSubpass = 0;
	deps[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[1].srcSubpass = 0;
	deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	rp.attachmentCount = 1;
	rp.pAttachments = &att;
	rp.subpassCount = 1;
	rp.pSubpasses = &sub;
	rp.dependencyCount = 2;
	rp.pDependencies = deps;
	if (d->fn.CreateRenderPass(device, &rp, nullptr, &s->renderPass) != VK_SUCCESS)
	{
		PML_LOG(LogLevel::Warn, "overlay: vkCreateRenderPass failed, no overlay on swapchain %llu",
		        (unsigned long long)sc.info.serial);
		return;
	}

	s->stats = std::make_shared<OverlayStats>(sc.info, uint32_t(getpid()), pml::monotonicNs());
	{
		std::lock_guard<std::mutex> lock(gRegistryMutex);
		gStats[sc.info.serial] = s->stats;
	}

	sc.info.timingFlags |= pml::kSwapchainOverlay;
	s->counted = true;
	{
		std::lock_guard<std::mutex> lock(gRegistryMutex);
		gLiveSwapchains++;
		pml::setOverlayAvailable(*sc.segment, true);
	}
	sc.userData = s.release();
	PML_LOG(LogLevel::Info, "overlay: swapchain %llu ready (%u images, colour mode %d)", (unsigned long long)sc.info.serial,
	        count, int(static_cast<OvSwapchain *>(sc.userData)->mode));
}

void onSwapchainDestroying(SwapchainContext &sc)
{
	auto *s = static_cast<OvSwapchain *>(sc.userData);
	if (!s)
		return;
	sc.userData = nullptr;
	if (inherited(*s))
		return;   // a fork()ed child destroying its parent's swapchain: leave the parent's state alone (leaked)
	retireSwapchain(*s);
	{
		std::lock_guard<std::mutex> lock(gRegistryMutex);
		gStats.erase(sc.info.serial);
	}
	{
		std::lock_guard<std::mutex> lock(gImguiMutex);
		gImguiDevice = sc.device;
		destroySlots(*s);
		shutdownImGui(*s);
	}
	if (s->renderPass)
		s->dev->fn.DestroyRenderPass(sc.device->device, s->renderPass, nullptr);
	delete s;
}

// Runs inside the layer's timing broker, which has no handler for exceptions: an allocation failure
// here drops the record (the overlay's numbers miss one frame) instead of unwinding through the present.
void onRecord(const pml::FrameRecord &record, const pml::SwapchainInfo &info)
{
	try
	{
		std::shared_ptr<OverlayStats> stats;
		{
			std::lock_guard<std::mutex> lock(gRegistryMutex);
			auto it = gStats.find(info.serial);
			if (it == gStats.end())
				return;
			stats = it->second;
		}
		stats->push(record);
	}
	catch (...)
	{
	}
}

static void onPresentImpl(PresentContext &ctx)
{
	auto *s = static_cast<OvSwapchain *>(ctx.swapchain->userData);
	if (!s || s->failed || inherited(*s))
		return;
	const uint64_t t0 = pml::monotonicNs();
	s->presents++;

	pollControl(*s);
	const bool visible = s->control.visible != 0;
	if (visible != s->wasVisible)
	{
		if (visible)
			s->stats->reset();   // do not show a window that spans the hidden period
		s->stats->setCollecting(visible);
		s->wasVisible = visible;
	}
	DumpConfig &dump = dumpConfig();
	const bool wantDump = s->canDump && !dump.path.empty() && !dump.done.load(std::memory_order_acquire) &&
	                      s->presents >= dump.frame;
	if (!visible && !wantDump)
		return;

	OvDevice &d = *s->dev;
	if (!isGraphicsFamily(d, ctx.queueFamily))
	{
		if (!s->warnedNoGraphics)
		{
			s->warnedNoGraphics = true;
			PML_LOG(LogLevel::Info, "overlay: present queue family %u has no graphics support, skipping the overlay",
			        ctx.queueFamily);
		}
		return;
	}
	if (ctx.imageIndex >= s->slots.size())
		return;
	ImageSlot &slot = s->slots[ctx.imageIndex];
	VkDevice device = d.ctx->device;

	if (!prepareSlot(*s, slot, ctx.queueFamily))
	{
		PML_LOG(LogLevel::Warn, "overlay: cannot create per-image resources, overlay disabled on swapchain %llu",
		        (unsigned long long)s->sc->info.serial);
		markFailed(*s);
		return;
	}

	// The command buffer of this image may still be executing only if the application re-acquired
	// the image before its previous presentation finished, which the spec forbids. Bounded wait anyway.
	if (slot.submitted)
	{
		if (d.fn.WaitForFences(device, 1, &slot.fence, VK_TRUE, 100'000'000ull) != VK_SUCCESS)
		{
			PML_LOG_ONCE(LogLevel::Warn, "overlay: image still in use by a previous overlay frame, skipping a frame");
			return;
		}
		d.fn.ResetFences(device, 1, &slot.fence);
		slot.submitted = false;
	}

	Readback rb;
	const uint32_t bpp = dumpBytesPerPixel(uint32_t(s->format));
	if (wantDump && !createReadback(*s, bpp, &rb))
	{
		PML_LOG(LogLevel::Warn, "overlay: cannot allocate the dump buffer");
		destroyReadback(*s, &rb);
		dump.done.store(true);
		return;
	}

	VkCommandBuffer cmd = slot.cmd;
	d.fn.ResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (d.fn.BeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
	{
		destroyReadback(*s, &rb);
		return;
	}

	bool drew = false;
	if (visible)
	{
		std::lock_guard<std::mutex> lock(gImguiMutex);
		gImguiDevice = d.ctx;
		if (!s->imguiReady)
		{
			const uint64_t tInit = pml::monotonicNs();
			if (!initImGui(*s, ctx.queue, ctx.queueFamily))
			{
				markFailed(*s);
				d.fn.EndCommandBuffer(cmd);
				destroyReadback(*s, &rb);
				return;
			}
			PML_LOG(LogLevel::Debug, "overlay: backend created in %.2f ms", double(pml::monotonicNs() - tInit) * 1e-6);
		}
		ImGui::SetCurrentContext(s->imgui);
		collectGarbage(*s, false);

		const uint64_t now = pml::monotonicNs();
		s->stats->update(now);

		const float scale = effectiveScale(s->control, s->width, s->height);
		if (s->bakedScale != scale)
			warmUp(*s, scale);

		ImGuiIO &io = ImGui::GetIO();
		io.DisplaySize = ImVec2(float(s->width), float(s->height));
		io.DeltaTime = s->lastFrameNs ? std::clamp(float(now - s->lastFrameNs) * 1e-9f, 1e-5f, 0.1f) : 1.0f / 60.0f;
		s->lastFrameNs = now;

		UiParams up;
		up.snapshot = &s->stats->snapshot();
		up.panels = s->control.panels ? s->control.panels : kDefaultPanels;
		up.position = s->control.position;
		up.scale = scale;
		up.displayWidth = float(s->width);
		up.displayHeight = float(s->height);
		describe(*s, &up);
		if (s->presents % 240 == 0 && logLevel() >= LogLevel::Debug)
			PML_LOG(LogLevel::Debug, "overlay: swapchain %llu %s | %s", (unsigned long long)s->sc->info.serial,
			        up.timingLine.c_str(), snapshotSummary(*up.snapshot, up.displayTiming).c_str());

		ImGui_ImplVulkan_NewFrame();
		ImGui::NewFrame();
		ImGui::PushFont(s->font, kBaseFontPx * scale);
		drawOverlayUi(up);
		ImGui::PopFont();
		ImGui::Render();

		// Atlas uploads first (copies cannot be inside a render pass); the backend must not do its own.
		if (!serveTextures(*s, cmd, ctx.queue))
		{
			PML_LOG(LogLevel::Warn, "overlay: cannot create the font texture, overlay disabled on swapchain %llu",
			        (unsigned long long)s->sc->info.serial);
			markFailed(*s);
			d.fn.EndCommandBuffer(cmd);
			destroyReadback(*s, &rb);
			return;
		}
		waitForUploadOnOtherQueue(*s, ctx.queue);
		ImDrawData *drawData = ImGui::GetDrawData();
		drawData->Textures = nullptr;

		VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rbi.renderPass = s->renderPass;
		rbi.framebuffer = slot.framebuffer;
		rbi.renderArea.extent = {s->width, s->height};
		d.fn.CmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
		d.fn.CmdEndRenderPass(cmd);
		drew = true;
	}
	if (wantDump)
		recordDumpCopy(*s, cmd, slot.image, rb.buffer);
	d.fn.EndCommandBuffer(cmd);

	// Wait for whatever the present was going to wait for, then signal ours. Everything that can throw
	// (allocations) happens before the submission: once it has waited on the application's semaphores,
	// the present has to go down with ours.
	ctx.waits->reserve(std::max<size_t>(ctx.waits->size(), 1));
	std::vector<VkPipelineStageFlags> stages(ctx.waits->size(),
	                                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.waitSemaphoreCount = uint32_t(ctx.waits->size());
	si.pWaitSemaphores = ctx.waits->data();
	si.pWaitDstStageMask = stages.data();
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores = &slot.semaphore;
	const VkResult r = d.fn.QueueSubmit(ctx.queue, 1, &si, slot.fence);
	if (r != VK_SUCCESS)
	{
		PML_LOG(LogLevel::Warn, "overlay: vkQueueSubmit failed (%d), overlay disabled on swapchain %llu", int(r),
		        (unsigned long long)s->sc->info.serial);
		markFailed(*s);
		destroyReadback(*s, &rb);
		return;   // the original wait semaphores stay in the present
	}
	slot.submitted = true;
	slot.serial = ++s->submitSerial;
	ctx.waits->assign(1, slot.semaphore);
	ctx.drawn = drew;

	if (wantDump)
	{
		dump.done.store(true, std::memory_order_release);
		finishDump(*s, slot, rb, bpp);
		destroyReadback(*s, &rb);
	}
	ctx.cpuNs = pml::monotonicNs() - t0;
}

// The seam says "never throw": an exception (allocation failure in a vector, a string or Dear ImGui)
// turns the overlay off for this swapchain and the present goes on without it.
void onPresent(PresentContext &ctx)
{
	try
	{
		onPresentImpl(ctx);
	}
	catch (...)
	{
		if (auto *s = static_cast<OvSwapchain *>(ctx.swapchain->userData))
			markFailed(*s);
		PML_LOG_ONCE(LogLevel::Error, "overlay: internal error, overlay disabled on this swapchain");
	}
}

} // namespace pml::layer::overlay
