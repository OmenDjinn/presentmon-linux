// SPDX-License-Identifier: MIT
// A tiny shared object that links pml_metrics (static) like the Vulkan layer will. Linked with
// -Wl,--no-undefined, so a missing symbol fails the build. Exports one C function.
#include <pml/metrics/engine.hpp>

#include <cstdint>

extern "C" __attribute__((visibility("default")))
int pml_probe_run(double *msBetweenPresents, double *msDisplayedTime, uint32_t *rowCount)
{
	using namespace pml::metrics;
	SwapchainEngine e(SwapchainContext{ 1, "DXVK", 0 }, MetricsMode::V2, 1000);
	uint32_t rows = 0;
	double bp = 0, dt = 0;
	for (uint64_t i = 1; i <= 8; ++i) {
		pml::FrameRecord r = {};
		r.frameIndex = i;
		r.presentStartNs = 1000000 + i * 4000000;
		r.presentEndNs = r.presentStartNs + 100000;
		r.screenNs = r.presentStartNs + 5000000;
		r.readyNs = r.presentStartNs + 2000000;
		r.flags = pml::kFrameDisplayed;
		r.presentMode = kVkPresentModeFifo;
		for (auto &row : e.process(r)) {
			++rows;
			bp = row.metrics.msBetweenPresents;
			dt = row.metrics.msDisplayedTime;
		}
	}
	*msBetweenPresents = bp;
	*msDisplayedTime = dt;
	*rowCount = rows;
	return 0;
}
