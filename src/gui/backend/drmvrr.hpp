// SPDX-License-Identifier: MIT
// The outputs' VRR state for the VRR verdict: pml::readDrmOutputs (src/common, libdrm through dlopen) and the
// verdict's context derived from it. Plain C++, no Qt.
#pragma once

#include <pml/drm_outputs.hpp>
#include <pml/metrics/vrr.hpp>

#include <string>
#include <vector>

namespace pml::gui {

using OutputVrr = pml::DrmOutput;
using pml::edidVerticalRange;
using pml::readDrmOutputs;

// The context the VRR verdict gets from the outputs: the one output with VRR_ENABLED on is the game's (KWin enables
// VRR only for the active fullscreen window), so: driver On, its refresh as the maximum and its EDID minimum as the
// floor. When every readable output reports VRR off: driver Off. Otherwise unknown.
// `which` (optional) gets the name of the output used ("" when none).
pml::metrics::VrrContext vrrContextFromOutputs(const std::vector<OutputVrr> &outputs, std::string *which = nullptr);

} // namespace pml::gui
