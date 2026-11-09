// SPDX-License-Identifier: MIT
// Dear ImGui configuration for the in-game overlay (selected with IMGUI_USER_CONFIG).
// The overlay runs inside other people's processes: no demo code, and an
// assertion must never abort the game.
#pragma once

#define IM_ASSERT(_EXPR)                    ((void)(_EXPR))
#define IMGUI_DISABLE_DEMO_WINDOWS
#define IMGUI_DISABLE_DEBUG_TOOLS
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
#define IMGUI_DISABLE_WIN32_FUNCTIONS
#define IMGUI_DISABLE_DEFAULT_FONT_VECTOR   // the overlay embeds its own font; the 9 KB bitmap font stays for headless tests
