// SPDX-License-Identifier: MIT
// presentmon-linux: shared-memory protocol between VkLayer_presentmon (producer) and
// readers (the presentmon CLI, tests). See docs/ARCHITECTURE.md, section 3.
//
// One POSIX shared-memory segment per process that created at least one swapchain:
//   name   "presentmon-linux.<pid>"  ->  /dev/shm/presentmon-linux.<pid>, mode 0600
//   layout [SegmentHeader][SwapchainSlot x maxSwapchains][RingSlot x ringCapacity]
// The layer creates, initializes and unlinks the segment. Readers map it read-write
// (they only ever write the control area).
//
// Rules for everything in this file:
//   * Plain trivially-copyable structs only (shared between processes and between the
//     64-bit and the future 32-bit layer build): fixed-size types, every uint64_t at an
//     8-byte aligned offset, explicit reserved fields. No std::atomic members; use the
//     atomicLoad/atomicStore helpers below (std::atomic_ref) for fields that are
//     accessed concurrently.
//   * Times are CLOCK_MONOTONIC nanoseconds. 0 always means "unknown / not available"
//     (PresentMon's metric code treats 0 as missing as well).
//   * Only append fields by consuming reserved space; bump kProtocolVersion when the
//     meaning or layout of an existing field changes.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pml {

// ---------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------
inline constexpr uint32_t kMagic = 0x314C4D50u;               // "PML1" (little endian)
inline constexpr uint32_t kProtocolVersion = 1;
inline constexpr const char kSegmentPrefix[] = "presentmon-linux.";  // + decimal pid
inline constexpr uint32_t kMaxSwapchains = 64;
inline constexpr uint32_t kDefaultRingCapacity = 8192;        // records, power of two
inline constexpr uint64_t kSlotWriting = ~0ull;               // RingSlot::seq while being written

// ---------------------------------------------------------------------------------------
// Atomic access helpers for fields that live in shared memory.
// ---------------------------------------------------------------------------------------
template <class T>
inline T atomicLoad(const T &field, std::memory_order order = std::memory_order_acquire)
{
	static_assert(std::is_integral_v<T>);
	return std::atomic_ref<T>(const_cast<T &>(field)).load(order);
}

template <class T>
inline void atomicStore(T &field, T value, std::memory_order order = std::memory_order_release)
{
	static_assert(std::is_integral_v<T>);
	std::atomic_ref<T>(field).store(value, order);
}

template <class T>
inline T atomicFetchAdd(T &field, T value, std::memory_order order = std::memory_order_acq_rel)
{
	static_assert(std::is_integral_v<T>);
	return std::atomic_ref<T>(field).fetch_add(value, order);
}

// ---------------------------------------------------------------------------------------
// Enums (stored as uint32_t in the structs below)
// ---------------------------------------------------------------------------------------
enum class SurfaceKind : uint32_t
{
	Unknown = 0,
	Wayland = 1,
	Xcb = 2,
	Xlib = 3,
	Display = 4,   // VK_KHR_display
	Headless = 5,
	Other = 6,
};

enum class SegmentState : uint32_t
{
	Initializing = 0,
	Live = 1,
	Exited = 2,    // process shut down cleanly; the name is unlinked right after
};

enum class OverlayPosition : uint32_t
{
	TopLeft = 0,
	TopRight = 1,
	BottomLeft = 2,
	BottomRight = 3,
};

// OverlayControl::panels. 0 means "the overlay's default set".
enum OverlayPanel : uint32_t
{
	kPanelFps = 1u << 0,             // displayed / presented fps, 1% and 0.1% lows
	kPanelFrameTimeGraph = 1u << 1,  // frame time history graph
	kPanelCpu = 1u << 2,             // CPU busy / wait
	kPanelGpu = 1u << 3,             // GPU busy / wait / latency (NA without GPU timing)
	kPanelLatency = 1u << 4,         // display latency, render-present latency, until-displayed
	kPanelSwapchain = 1u << 5,       // runtime, present mode, size, format / HDR, timing stage
	kPanelDropped = 1u << 6,         // dropped (not displayed) frames
	kPanelVrr = 1u << 7,             // VRR readout: refresh rate now, swing, verdict (pml/metrics/vrr.hpp)
	kPanelCap = 1u << 8,             // frame cap: target, method, wait per frame
	// 1u << 9 was the experience score (removed 2026-11-13); the layer ignores the bit.
};

// FrameRecord::flags
enum FrameFlag : uint32_t
{
	kFrameTimingRequested = 1u << 0,   // a VkPresentTimingInfoEXT was attached (or merged) for this present
	kFrameTimingComplete = 1u << 1,    // the driver returned a report with reportComplete == VK_TRUE
	kFrameTimingLost = 1u << 2,        // no complete report: timeout, error, queue full, swapchain destroyed
	kFrameDisplayed = 1u << 3,         // screenNs is valid
	kFrameNotDisplayed = 1u << 4,      // report complete but no screen time: replaced / discarded
	kFrameAppPresentId = 1u << 5,      // presentId came from the application (VkPresentId2KHR)
	kFrameAppTimingRequest = 1u << 6,  // the application attached its own VkPresentTimingInfoEXT
	kFrameOverlayDrawn = 1u << 7,      // the in-game overlay drew into this image
	kFramePresentFailed = 1u << 8,     // presentResult < 0
	kFrameSuboptimal = 1u << 9,        // presentResult == VK_SUBOPTIMAL_KHR
	kFrameCapWait = 1u << 10,          // the layer's frame cap held this present (capWaitNs is how long)
	kFrameCapReflex = 1u << 11,        // the cap was enforced through the app's NV_low_latency2 interval (no layer wait)
};

// SwapchainInfo::timingFlags
enum SwapchainTimingFlag : uint32_t
{
	kSwapchainLayerTiming = 1u << 0,      // present timing is enabled on this swapchain and used by the layer
	kSwapchainAppTiming = 1u << 1,        // the app created it with VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT
	kSwapchainAppPresentId2 = 1u << 2,    // the app created it with VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR
	kSwapchainLayerPresentId2 = 1u << 3,  // the layer added VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR
	kSwapchainAppPresentId1 = 1u << 4,    // the app chains VkPresentIdKHR (VK_KHR_present_id) on its presents
	kSwapchainOverlay = 1u << 5,          // overlay resources exist for this swapchain
};

// ---------------------------------------------------------------------------------------
// One record per vkQueuePresentKHR call (per swapchain in the call), published in present
// order per swapchain once its present-timing feedback is final (or given up on).
// ---------------------------------------------------------------------------------------
struct alignas(8) FrameRecord
{
	// Identity
	uint64_t swapchainSerial;   // layer-assigned, unique within the process, never reused (1-based)
	uint64_t swapchainHandle;   // VkSwapchainKHR value (printed as SwapChainAddress)
	uint64_t frameIndex;        // 1-based count of presents on this swapchain
	uint64_t presentId;         // present ID used for timing (app's or layer's), 0 = none

	// CPU side
	// The app's view of its present call, like PresentMon's on Windows where a limiter inside Present() counts as
	// time in Present(): a frame cap wait of the layer is inside [presentStartNs, presentEndNs] (capWaitNs says
	// how long; early placement also moves the driver's present, and so the screen time, by that much).
	uint64_t presentStartNs;    // when the app's vkQueuePresentKHR call entered the layer
	uint64_t presentEndNs;      // when the driver's vkQueuePresentKHR returned, or the layer's late cap wait ended
	uint32_t threadId;          // gettid() of the presenting thread
	int32_t presentResult;      // VkResult returned to the app

	// Present-timing feedback, converted to CLOCK_MONOTONIC
	uint64_t readyNs;           // VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT
	uint64_t latchNs;           // VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT (if reported)
	uint64_t screenNs;          // FIRST_PIXEL_VISIBLE if reported, else FIRST_PIXEL_OUT, else 0
	uint32_t screenStage;       // VkPresentStageFlagBitsEXT that produced screenNs (0 = none)
	uint32_t flags;             // FrameFlag bits

	// Present parameters
	uint32_t presentMode;       // VkPresentModeKHR in effect for this present
	uint32_t imageIndex;
	uint64_t overlayCpuNs;      // CPU time the overlay spent inside this present (0 if not drawn)

	// GPU (layer timestamps around queue submissions, src/layer/gpu_intervals.hpp; 0 = no GPU data)
	uint64_t gpuStartNs;        // first GPU timestamp of the frame's work
	uint64_t gpuBusyNs;         // union of busy intervals
	uint64_t gpuVideoBusyNs;

	// VK_NV_low_latency2 markers for this frame (M3)
	uint64_t latencyFrameId;    // VkSetLatencyMarkerInfoNV::presentID the markers below belong to (0 = none)
	uint64_t simStartNs;        // VK_LATENCY_MARKER_SIMULATION_START_NV
	uint64_t simEndNs;          // VK_LATENCY_MARKER_SIMULATION_END_NV
	uint64_t renderSubmitStartNs;  // VK_LATENCY_MARKER_RENDERSUBMIT_START_NV
	uint64_t renderSubmitEndNs;    // VK_LATENCY_MARKER_RENDERSUBMIT_END_NV
	uint64_t markerPresentStartNs; // VK_LATENCY_MARKER_PRESENT_START_NV
	uint64_t markerPresentEndNs;   // VK_LATENCY_MARKER_PRESENT_END_NV
	uint64_t inputSampleNs;        // VK_LATENCY_MARKER_INPUT_SAMPLE_NV
	uint64_t triggerFlashNs;       // VK_LATENCY_MARKER_TRIGGER_FLASH_NV
	uint64_t sleepStartNs;         // vkLatencySleepNV called
	uint64_t sleepEndNs;           // the latency sleep finished

	// Input (M4)
	uint64_t inputNs;           // latest input event attributed to this frame
	uint64_t mouseClickNs;      // latest mouse click attributed to this frame

	// Pipeline compile activity attributed to this frame (M3)
	uint64_t pipelineCompileCount;
	uint64_t pipelineCompileBusyNs;    // union of compile intervals
	uint64_t pipelineCompileSummedNs;  // sum of compile intervals

	// Frame cap (src/layer/frame_limiter.hpp): time the layer held this present inside the application's
	// vkQueuePresentKHR call, included in presentEndNs (late placement) or before the driver call (early).
	uint64_t capWaitNs;

	uint64_t reserved[7];
};

// ---------------------------------------------------------------------------------------
// Swapchain metadata, published by the layer when a swapchain is created and updated when
// it is destroyed. Records refer to it through swapchainSerial.
// ---------------------------------------------------------------------------------------
struct alignas(8) SwapchainInfo
{
	uint64_t serial;            // 0 = unused slot
	uint64_t handle;            // VkSwapchainKHR
	uint64_t createdNs;
	uint64_t destroyedNs;       // 0 while alive
	uint64_t refreshDurationNs; // from vkGetSwapchainTimingPropertiesEXT when known, else 0
	uint32_t width;
	uint32_t height;
	uint32_t format;            // VkFormat
	uint32_t colorSpace;        // VkColorSpaceKHR
	uint32_t presentMode;       // VkPresentModeKHR at creation
	uint32_t minImageCount;
	uint32_t imageCount;
	uint32_t imageUsage;        // VkImageUsageFlags requested by the app
	uint32_t surfaceKind;       // SurfaceKind
	uint32_t timingStagesSupported; // VkPresentStageFlagsEXT the surface supports (0 = no present timing)
	uint32_t timingStagesUsed;  // stages the layer requests for its own records
	uint32_t timingFlags;       // SwapchainTimingFlag bits
	uint32_t timeDomain;        // VkTimeDomainKHR the layer uses for its own requests
	uint32_t reserved32;
	uint64_t timeDomainId;
	char engineName[32];        // VkApplicationInfo::pEngineName of the owning instance ("DXVK", "vkd3d", ...)
	uint64_t reserved[2];
};

// ---------------------------------------------------------------------------------------
// Overlay control: written by readers (presentmon CLI), read by the layer on every present.
// Writers store the fields, then bump `generation` with release order; the layer loads
// `generation` with acquire order and re-reads the fields when it changed.
// ---------------------------------------------------------------------------------------
struct alignas(8) OverlayControl
{
	uint32_t generation;
	uint32_t visible;           // 0 = hidden, 1 = visible
	uint32_t position;          // OverlayPosition
	uint32_t scalePermille;     // 1000 = 1.0, 0 = automatic (from the swapchain height)
	uint32_t panels;            // OverlayPanel bits, 0 = default set
	uint32_t available;         // written by the layer: 1 if the overlay is loaded (PRESENTMON_OVERLAY != off)
	uint32_t reserved[10];
};

// Frame cap control (CapControl::mode).
enum class CapMode : uint32_t
{
	Off = 0,
	Fixed = 1,      // fpsMilli
	Auto = 2,       // just under the VRR maximum of the output with VRR on (min with fpsMilli when set)
};

// Where the present-side limiter waits (CapControl::placement).
enum class CapPlacement : uint32_t
{
	Late = 0,       // after the driver's present returned: the next frame starts later (lowest latency; MangoHud's default)
	Early = 1,      // before the driver call: the finished frame waits (most even present spacing, more latency)
};

// CapControl::flags
enum CapFlag : uint32_t
{
	kCapNoReflex = 1u << 0,   // never use the app's NV_low_latency2 interval, always wait in the present
};

// How the cap is enforced right now (CapControl::method, written by the layer).
enum class CapMethod : uint32_t
{
	None = 0,           // no cap in force
	PresentLate = 1,
	PresentEarly = 2,
	Reflex = 3,         // through the app's VK_NV_low_latency2 minimumIntervalUs (the wait is the game's own sleep)
	FrameThread = 4,    // late placement, held on the app's frame-pacing thread (in its vkWaitForPresentKHR/2KHR, or
	                    // its per-frame vkGetPastPresentationTimingEXT poll): where DXVK's and vkd3d-proton's own
	                    // limiters wait before they release the game thread
};

// Frame cap control, in the control area. Readers (CLI, GUI) write the first half and bump `generation`;
// the layer writes the second half (status), each field on its own. Old layers leave it all zero
// (`available` 0).
struct alignas(8) CapControl
{
	// Written by readers, as a seqlock: `generation` is odd while a writer is between its first and last store
	// (pml::writeCapControl), even otherwise; a snapshot whose generation was odd or changed while it was read is
	// torn. It starts at 2.
	uint32_t generation;
	uint32_t mode;              // CapMode
	uint32_t fpsMilli;          // cap in 1/1000 fps (Fixed), upper bound for Auto (0 = none)
	uint32_t placement;         // CapPlacement
	uint32_t flags;             // CapFlag bits
	uint32_t reservedIn[3];

	// Written by the layer.
	uint32_t available;         // 1: the layer implements frame caps
	uint32_t method;            // CapMethod in force on the most recently presented capped swapchain
	uint32_t effectiveMilli;    // cap in force, 1/1000 fps (0 = none)
	uint32_t refreshMilli;      // Auto: refresh of the VRR output used, 1/1000 Hz (0 = unknown)
	uint32_t vrr;               // Auto, the game's output: 0 unknown, 1 VRR on, 2 VRR off (or off on every output)
	uint32_t meanWaitUs;        // mean limiter wait per frame over the last ~second
	uint32_t lateP99Us;         // p99 of how late the limiter released a frame after its deadline
	uint32_t appliedGeneration; // the reader generation whose settings this status reflects (0 = none yet)
};

// ---------------------------------------------------------------------------------------
// Segment header
// ---------------------------------------------------------------------------------------
struct alignas(64) SegmentHeader
{
	// Immutable after initialization. Valid once atomicLoad(magic) == kMagic.
	uint32_t magic;
	uint32_t version;
	uint32_t headerSize;
	uint32_t swapchainSlotSize;
	uint32_t ringSlotSize;
	uint32_t frameRecordSize;
	uint32_t maxSwapchains;
	uint32_t ringCapacity;          // power of two
	int32_t pid;                    // getpid() of the producer (in its PID namespace)
	uint32_t layerBits;             // 64 or 32
	uint64_t processStartTicks;     // field 22 of /proc/<pid>/stat, guards against PID reuse
	uint64_t createdNs;
	char processName[256];          // display name: Windows exe basename for Wine processes
	char exePath[512];              // readlink(/proc/self/exe)
	char engineName[64];            // VkApplicationInfo of the instance that created the first swapchain
	char applicationName[128];
	uint32_t engineVersion;
	uint32_t applicationVersion;
	uint32_t apiVersion;
	uint32_t reserved0;
	char layerVersion[32];
	uint64_t reservedImmutable[8];

	// Live state, written by the layer.
	alignas(64) uint64_t writeSeq;  // records published so far; record n lives in ring slot n % capacity
	uint64_t heartbeatNs;           // CLOCK_MONOTONIC of the most recent present on any swapchain
	uint64_t droppedRecords;        // records the layer could not produce (e.g. lost bookkeeping)
	uint32_t state;                 // SegmentState
	uint32_t swapchainHighWater;    // number of swapchain slots used so far
	uint64_t reservedLive[4];

	// Control area, written by readers.
	alignas(64) OverlayControl overlay;
	CapControl cap;
};

struct alignas(8) SwapchainSlot
{
	uint64_t seq;                   // seqlock: odd while being written, even when stable
	SwapchainInfo info;
};

struct alignas(8) RingSlot
{
	uint64_t seq;                   // n + 1 once record n is stored here, kSlotWriting while being written
	FrameRecord record;
};

// Layout checks. These sizes are part of the protocol: the 32-bit layer must match them.
static_assert(std::is_trivially_copyable_v<FrameRecord> && std::is_standard_layout_v<FrameRecord>);
static_assert(std::is_trivially_copyable_v<SwapchainInfo> && std::is_standard_layout_v<SwapchainInfo>);
static_assert(std::is_trivially_copyable_v<SegmentHeader> && std::is_standard_layout_v<SegmentHeader>);
static_assert(sizeof(FrameRecord) == 320);
static_assert(sizeof(SwapchainInfo) == 152);
static_assert(sizeof(OverlayControl) == 64);
static_assert(sizeof(CapControl) == 64);
static_assert(sizeof(SwapchainSlot) == 160);
static_assert(sizeof(RingSlot) == 328);
static_assert(offsetof(SegmentHeader, writeSeq) % 64 == 0);
static_assert(offsetof(SegmentHeader, overlay) % 64 == 0);
static_assert(sizeof(SegmentHeader) % 64 == 0);

} // namespace pml
