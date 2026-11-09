// SPDX-License-Identifier: MIT
// The overlay's Dear ImGui layout. Draws one borderless window from a Snapshot; knows nothing
// about Vulkan.
#pragma once

#include "stats.hpp"

#include <pml/protocol.hpp>

#include <string>

namespace pml::layer::overlay {

struct UiParams
{
	const Snapshot *snapshot = nullptr;
	uint32_t panels = 0;                 // OverlayPanel bits, already resolved (never 0)
	pml::OverlayPosition position = pml::OverlayPosition::TopLeft;
	float scale = 1.0f;                  // 1.0 = 16 px font
	float displayWidth = 0;
	float displayHeight = 0;
	std::string runtimeLine;             // "DXVK  FIFO  2560x1440"
	std::string formatLine;              // "B8G8R8A8_SRGB  sRGB"
	std::string timingLine;              // "timing: pixel-out + queue-end"
	bool displayTiming = true;           // the swapchain can deliver screen times
};

inline constexpr float kBaseFontPx = 16.0f;

// Overlay scale when none is configured. The panel is a fixed number of characters wide, so it
// follows the shorter swapchain edge: a portrait 1440x2560 screen gets the same panel as a
// landscape 2560x1440 one instead of a panel that covers half the width.
float autoOverlayScale(uint32_t width, uint32_t height);

// Default panel set when OverlayControl::panels == 0.
inline constexpr uint32_t kDefaultPanels = pml::kPanelFps | pml::kPanelFrameTimeGraph | pml::kPanelCpu |
                                           pml::kPanelGpu | pml::kPanelLatency | pml::kPanelSwapchain |
                                           pml::kPanelDropped | pml::kPanelVrr;

// Builds the window; call between ImGui::NewFrame() and ImGui::Render(), with the font already pushed.
void drawOverlayUi(const UiParams &params);

// One-line text form of what the overlay shows ("NA" where it shows NA). Used for the debug log and tests.
std::string snapshotSummary(const Snapshot &snapshot, bool displayTiming);

// Text that comes from the application (engine name, ...) with every byte outside printable ASCII
// replaced by '?': the font atlas holds only the warmed-up ASCII glyphs, and baking another glyph
// mid-frame makes Dear ImGui's Vulkan backend upload the atlas with a queue submit + wait-idle.
std::string asciiOnly(const std::string &text);

// Every printable ASCII character, used to bake the glyphs before the first real frame.
const char *glyphWarmupText();

} // namespace pml::layer::overlay
