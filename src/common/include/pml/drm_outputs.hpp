// SPDX-License-Identifier: MIT
// What the kernel's DRM driver says about each connected output: the CRTC's VRR_ENABLED property (the ground truth
// for VRR: KWin turns it on only while a fullscreen window is active on that output, or always with policy
// "Always"), the current mode's refresh rate and size, the connector's vrr_capable, and the VRR range from the EDID's
// range descriptor. Read-only, no DRM master needed (the same reads as tests/e2e/pmltest.py VrrWatch).
//
// libdrm is loaded with dlopen("libdrm.so.2") on first use, so nothing links against it: the layer runs inside games
// (and their Steam runtime container), where it may be missing; then readDrmOutputs() returns nothing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pml {

struct DrmOutput
{
	std::string name;          // "DP-2"
	bool capable = false;      // connector property vrr_capable
	int enabled = -1;          // CRTC property VRR_ENABLED: 1, 0, -1 = unreadable
	double refreshHz = 0;      // current mode, from the pixel clock and totals (0 = no CRTC)
	double edidMinHz = 0;      // EDID display range limits (0 = none)
	double edidMaxHz = 0;
	uint32_t width = 0;        // current mode
	uint32_t height = 0;
	std::string driver;        // the card's DRM driver ("nvidia-drm", "amdgpu", "i915", ...), "" = unknown
};

// Every connected output on every /dev/dri/card*. Empty (and *error set) when DRM or libdrm is unavailable.
std::vector<DrmOutput> readDrmOutputs(std::string *error = nullptr);

// EDID base block range descriptor (tag 0xFD): vertical min/max in Hz, with the +255 offset flags. False when
// there is none.
bool edidVerticalRange(const uint8_t *edid, size_t size, double *minHz, double *maxHz);

// The output a game presenting a `width` x `height` swapchain most likely uses for VRR: the one output with
// VRR_ENABLED on; with several, the one whose mode matches the size (else the orientation), then the highest
// refresh. Null when no output has VRR on. A guess: it cannot know which screen the game is on (pickOutput can).
const DrmOutput *pickVrrOutput(const std::vector<DrmOutput> &outputs, uint32_t width, uint32_t height);

// The output the game's swapchain is on, whatever its VRR state. `refreshDurationNs` is the driver's refresh cycle
// for the swapchain's surface (vkGetSwapchainTimingPropertiesEXT, valid once the surface is on a screen; 0 =
// unknown): the output whose mode refresh equals it (within kRefreshMatch; the two derive from the same mode
// timing and agree to a few parts per million). Several outputs with that mode, or no refresh cycle known: among
// those candidates the pickVrrOutput guess, else (none of them with VRR on) the one they all are when there is
// just one. Null when the outputs cannot tell.
// Why not pickVrrOutput alone: with one screen on KWin's VRR policy "Always" and the game on another screen whose
// VRR is off (windowed, not the active window), the only output with VRR on is the wrong one.
inline constexpr double kRefreshMatch = 1e-4;   // relative; 59.94 and 60 Hz are 1e-3 apart
const DrmOutput *pickOutput(const std::vector<DrmOutput> &outputs, uint32_t width, uint32_t height,
                            uint64_t refreshDurationNs);

// True when this output's display engine re-scans the last frame by itself once no new one arrived within
// 1 / (lowest VRR rate) (pml::metrics::VrrFloor::Rescan): NVIDIA's (nvkms runs the head one-shot with a
// MIN_REFRESH timer, open-gpu-kernel-modules nvkms-vrr.c; measured, docs/FINDINGS.md 7). Other drivers: unknown.
bool rescansAtVrrFloor(const DrmOutput &output);

} // namespace pml
