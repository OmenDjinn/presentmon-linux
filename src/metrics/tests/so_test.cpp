// SPDX-License-Identifier: MIT
// Loads the probe shared object (pml_metrics linked into a .so with --no-undefined) and runs
// the engine inside it, while this executable has its own copy of the library statically linked:
// two copies of the static library in one process must not interfere (the layer is loaded into
// games that may link other code).
#include <pml/metrics/engine.hpp>

#include <dlfcn.h>
#include <gtest/gtest.h>

TEST(SharedObject, EngineRunsInsideSharedObject)
{
	void *h = dlopen(PML_PROBE_SO, RTLD_NOW | RTLD_LOCAL);
	ASSERT_NE(h, nullptr) << dlerror();
	using Fn = int (*)(double *, double *, uint32_t *);
	auto fn = reinterpret_cast<Fn>(dlsym(h, "pml_probe_run"));
	ASSERT_NE(fn, nullptr) << dlerror();

	double bp = 0, dt = 0;
	uint32_t rows = 0;
	ASSERT_EQ(fn(&bp, &dt, &rows), 0);
	// 8 presents: the first seeds, rows are released one displayed frame late -> 6 rows.
	EXPECT_EQ(rows, 6u);
	EXPECT_NEAR(bp, 4.0, 1e-9);
	EXPECT_NEAR(dt, 4.0, 1e-9);

	// The same engine in this process (own static copy) gives the same numbers.
	using namespace pml::metrics;
	SwapchainEngine e(SwapchainContext{ 1, "DXVK", 0 }, MetricsMode::V2, 1000);
	uint32_t n = 0;
	for (uint64_t i = 1; i <= 8; ++i) {
		pml::FrameRecord r = {};
		r.frameIndex = i;
		r.presentStartNs = 1000000 + i * 4000000;
		r.presentEndNs = r.presentStartNs + 100000;
		r.screenNs = r.presentStartNs + 5000000;
		r.readyNs = r.presentStartNs + 2000000;
		r.flags = pml::kFrameDisplayed;
		n += uint32_t(e.process(r).size());
	}
	EXPECT_EQ(n, rows);

	// Symbols of the static library must not leak out of the (hidden-visibility) probe.
	EXPECT_EQ(dlsym(h, "_ZN4pmon4util12QpcConverter19TicksToMilliSecondsEm"), nullptr);
	dlclose(h);
}
