// SPDX-License-Identifier: MIT
// pml::FrameRecord -> PresentMon FrameData, plus the Linux-specific metadata names that the
// CSV writer and the overlay print. See docs/ARCHITECTURE.md section 3.2.
//
// Timebase: record times are CLOCK_MONOTONIC nanoseconds and are used directly as PresentMon
// "QPC" ticks; build the QpcConverter with pml::kTicksPerSecond (see makeQpcConverter()).
#pragma once

#include <CommonUtilities/mc/MetricsTypes.h>
#include <CommonUtilities/mc/UnifiedSwapChain.h>

#include <pml/clock.hpp>
#include <pml/protocol.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace pml::metrics {

// Names of the upstream types, so consumers need not spell the namespaces.
using FrameData = pmon::util::metrics::FrameData;
using FrameMetrics = pmon::util::metrics::FrameMetrics;
using FrameType = ::FrameType;
using QpcConverter = pmon::util::QpcConverter;

// Same as pmon::util::metrics::IsMissingFrameMetricValue (NaN).
using pmon::util::metrics::IsMissingFrameMetricValue;
using pmon::util::metrics::MissingFrameMetricValue;

// VkPresentModeKHR values (checked against the Vulkan headers in the tests); kept as plain
// numbers so this library does not depend on the Vulkan headers.
inline constexpr uint32_t kVkPresentModeImmediate = 0;
inline constexpr uint32_t kVkPresentModeMailbox = 1;
inline constexpr uint32_t kVkPresentModeFifo = 2;
inline constexpr uint32_t kVkPresentModeFifoRelaxed = 3;
inline constexpr uint32_t kVkPresentModeSharedDemandRefresh = 1000111000;
inline constexpr uint32_t kVkPresentModeSharedContinuousRefresh = 1000111001;
inline constexpr uint32_t kVkPresentModeFifoLatestReady = 1000361000;

// What a record says about the screen. Drives the FrameData built for it:
enum class DisplayState
{
	// kFrameDisplayed and screenNs != 0. FrameData: finalState = Presented,
	// displayed = {{Application, screenNs}}.
	Displayed,
	// kFrameNotDisplayed: the driver said the present never reached the screen (replaced by a
	// later one, discarded). FrameData: finalState = Discarded, displayed empty. PresentMon counts
	// it as dropped (Dropped = 1, isDroppedFrame, MsUntilDisplayed = 0 -> "NA" in the CSV).
	NotDisplayed,
	// No display information at all: Xwayland (QUEUE_OPERATIONS_END only), kFrameTimingLost,
	// timing not requested. FrameData: finalState = Presented (so the CSV says Dropped = 0)
	// with an empty display list. CPU columns stay valid; display columns are 0/"NA", never
	// invented, and the engine clears FrameMetrics::isDroppedFrame for these rows.
	Unknown,
};

DisplayState classifyDisplay(const pml::FrameRecord &record);

// Per-swapchain facts the record does not carry.
struct SwapchainContext
{
	uint32_t processId = 0;
	std::string engineName;            // VkApplicationInfo::pEngineName; selects the Runtime name
	uint64_t refreshDurationNs = 0;    // informational (0 = unknown)

	// Builds a context from the segment's swapchain slot.
	static SwapchainContext fromInfo(uint32_t pid, const pml::SwapchainInfo &info);
};

// Record -> FrameData. `frameId` is set to the low 32 bits of record.frameIndex.
// Does not set anything the animation/input math reads that the record does not have.
// runtime/presentMode stay at PresentMon's defaults (the math never reads them); print the
// Linux names with runtimeName()/presentModeName().
FrameData toFrameData(const pml::FrameRecord &record, const SwapchainContext &ctx);

// A record the console app would skip (PresentMon ignores IsLost / PresentFailed presents).
bool isFailedPresent(const pml::FrameRecord &record);

// Runtime column: "DXVK" -> "DXVK", "vkd3d*" -> "VKD3D", anything else "Vulkan".
// Comparison is case-insensitive and by prefix ("vkd3d-proton" is VKD3D).
const char *runtimeName(std::string_view engineName);

// PresentMode column: FIFO, FIFO_RELAXED, MAILBOX, IMMEDIATE, FIFO_LATEST_READY,
// SHARED_DEMAND_REFRESH, SHARED_CONTINUOUS_REFRESH, otherwise "Unknown".
const char *presentModeName(uint32_t vkPresentMode);

// SyncInterval column: 1 for FIFO / FIFO_RELAXED / FIFO_LATEST_READY, otherwise 0.
int32_t syncIntervalFor(uint32_t vkPresentMode);

// AllowsTearing column: true for IMMEDIATE and FIFO_RELAXED.
bool allowsTearingFor(uint32_t vkPresentMode);

// QpcConverter for CLOCK_MONOTONIC-ns timestamps. `sessionStartNs` is the session start the
// CSV's relative times use (pass pml::monotonicNs() at collector start).
inline QpcConverter makeQpcConverter(uint64_t sessionStartNs)
{
	return QpcConverter(pml::kTicksPerSecond, sessionStartNs);
}

} // namespace pml::metrics
