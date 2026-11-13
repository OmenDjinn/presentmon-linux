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

// The context the VRR readout gets from the outputs, for a swapchain of that size whose surface has that refresh
// cycle (SwapchainInfo::refreshDurationNs, 0 = unknown): the output the game is on (pml::pickOutput) gives the
// driver's state (its VRR_ENABLED), its mode refresh as the maximum, its EDID minimum as the floor and how the
// display engine behaves there. When that output cannot be told and every readable output reports VRR off: driver
// Off. Otherwise unknown. Without an output the refresh is the swapchain's own.
// `used` (optional) gets the output used, an element of `outputs` (null when none).
pml::metrics::VrrContext vrrContextFromOutputs(const std::vector<OutputVrr> &outputs, uint32_t width, uint32_t height,
                                               uint64_t refreshDurationNs, const OutputVrr **used = nullptr);

} // namespace pml::gui
