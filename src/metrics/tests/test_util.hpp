// SPDX-License-Identifier: MIT
// Helpers to build synthetic FrameRecord timelines.
#pragma once

#include <pml/metrics/engine.hpp>

#include <cstring>
#include <vector>

namespace pmltest {

constexpr uint64_t kMs = 1000000ull;
constexpr uint64_t kT0 = 1000ull * 1000 * 1000;   // 1 s: a non-zero session start

inline pml::FrameRecord makeRecord(uint64_t index, uint64_t presentStartNs, uint64_t presentDurNs,
                                   uint32_t presentMode = pml::metrics::kVkPresentModeFifo)
{
	pml::FrameRecord r;
	std::memset(&r, 0, sizeof(r));
	r.swapchainSerial = 1;
	r.swapchainHandle = 0xABCD;
	r.frameIndex = index;
	r.presentStartNs = presentStartNs;
	r.presentEndNs = presentStartNs + presentDurNs;
	r.threadId = 42;
	r.presentMode = presentMode;
	return r;
}

inline pml::FrameRecord displayed(pml::FrameRecord r, uint64_t readyNs, uint64_t screenNs)
{
	r.readyNs = readyNs;
	r.screenNs = screenNs;
	r.screenStage = 1;
	r.flags |= pml::kFrameTimingRequested | pml::kFrameTimingComplete | pml::kFrameDisplayed;
	return r;
}

inline pml::FrameRecord notDisplayed(pml::FrameRecord r, uint64_t readyNs)
{
	r.readyNs = readyNs;
	r.flags |= pml::kFrameTimingRequested | pml::kFrameTimingComplete | pml::kFrameNotDisplayed;
	return r;
}

inline pml::FrameRecord noInfo(pml::FrameRecord r, uint64_t readyNs = 0)
{
	r.readyNs = readyNs;
	return r;
}

inline std::vector<pml::metrics::Row> feedAll(pml::metrics::SwapchainEngine &e,
                                              const std::vector<pml::FrameRecord> &records,
                                              bool finish = true)
{
	std::vector<pml::metrics::Row> rows;
	for (auto &r : records) {
		auto got = e.process(r);
		rows.insert(rows.end(), got.begin(), got.end());
	}
	if (finish) {
		auto got = e.finish();
		rows.insert(rows.end(), got.begin(), got.end());
	}
	return rows;
}

} // namespace pmltest
