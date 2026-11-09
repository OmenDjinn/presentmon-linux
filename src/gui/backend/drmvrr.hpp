// SPDX-License-Identifier: MIT
// What the kernel's DRM driver says about each connected output's VRR, read-only through libdrm (no DRM
// master needed, the same reads as tests/e2e/pmltest.py VrrWatch): the CRTC's VRR_ENABLED property (the
// ground truth: KWin turns it on only while a fullscreen window is active on that output), the current
// mode's refresh rate, the connector's vrr_capable, and the VRR range from the EDID's range descriptor.
// Plain C++, no Qt.
#pragma once

#include <pml/metrics/vrr.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pml::gui {

struct OutputVrr
{
	std::string name;          // "DP-2"
	bool capable = false;      // connector property vrr_capable
	int enabled = -1;          // CRTC property VRR_ENABLED: 1, 0, -1 = unreadable
	double refreshHz = 0;      // current mode, from the pixel clock and totals (0 = no CRTC)
	double edidMinHz = 0;      // EDID display range limits (0 = none)
	double edidMaxHz = 0;
};

// Every connected output on every /dev/dri/card*. Empty (and *error set) when DRM cannot be read.
std::vector<OutputVrr> readDrmOutputs(std::string *error = nullptr);

// EDID base block range descriptor (tag 0xFD): vertical min/max in Hz, with the +255 offset flags.
// False when there is none.
bool edidVerticalRange(const uint8_t *edid, size_t size, double *minHz, double *maxHz);

// The context the VRR verdict gets from the outputs: the one output with VRR_ENABLED on is the game's (KWin
// enables VRR only for the active fullscreen window), so: driver On, its refresh as the maximum and its EDID
// minimum as the floor. When every readable output reports VRR off: driver Off. Otherwise unknown.
// `which` (optional) gets the name of the output used ("" when none).
pml::metrics::VrrContext vrrContextFromOutputs(const std::vector<OutputVrr> &outputs, std::string *which = nullptr);

} // namespace pml::gui
