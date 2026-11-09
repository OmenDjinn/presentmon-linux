// SPDX-License-Identifier: MIT
// Deterministic synthetic frame timeline shared by the fake producer and the tests that check the
// CSV against hand-computed values. All times are CLOCK_MONOTONIC nanoseconds: base + i * period.
//
// Frame i (1-based), period P = 5 ms, present call duration 0.2 ms:
//   presentStart_i = base + i*P            presentEnd_i = presentStart_i + 0.2 ms
//   ready_i        = presentStart_i + 1 ms
//   screen_i       = presentStart_i + 8 ms (when displayed)
// Hand-computed V2 results for a fully displayed stretch (see cli_tests.cpp):
//   MsBetweenPresents 5, MsInPresentAPI 0.2, MsRenderPresentLatency 1, MsUntilDisplayed 8,
//   MsBetweenDisplayChange 5, CPUBusy 4.8, CPUWait 0.2, FrameTime 5, DisplayedTime 5.
#pragma once

#include <pml/protocol.hpp>

#include <cstdint>
#include <cstring>
#include <string>

namespace faketl {

inline constexpr uint64_t kMs = 1'000'000ull;
inline constexpr uint64_t kPresentCallNs = 200'000;
inline constexpr uint64_t kReadyAfterNs = 1 * kMs;
inline constexpr uint64_t kScreenAfterNs = 8 * kMs;
inline constexpr uint64_t kHandle = 0xABC0DE12ull;
inline constexpr uint32_t kVkFifo = 2;

enum class Mode
{
	Displayed,   // every present reaches the screen
	NoDisplay,   // Xwayland-like: no display information at all
	Drop3,       // every 3rd present is replaced (not displayed)
};

struct Params
{
	uint32_t frames = 100;
	uint64_t periodNs = 5 * kMs;
	Mode mode = Mode::Displayed;
	uint64_t baseNs = 0;
};

inline pml::FrameRecord makeFrame(const Params &p, uint32_t i)
{
	pml::FrameRecord r;
	std::memset(&r, 0, sizeof(r));
	r.swapchainSerial = 1;
	r.swapchainHandle = kHandle;
	r.frameIndex = i;
	r.presentStartNs = p.baseNs + uint64_t(i) * p.periodNs;
	r.presentEndNs = r.presentStartNs + kPresentCallNs;
	r.threadId = 1;
	r.presentMode = kVkFifo;
	switch (p.mode)
	{
	case Mode::NoDisplay: break;
	case Mode::Drop3:
		if (i % 3 == 0)
		{
			r.readyNs = r.presentStartNs + kReadyAfterNs;
			r.flags = pml::kFrameTimingRequested | pml::kFrameTimingComplete | pml::kFrameNotDisplayed;
			break;
		}
		[[fallthrough]];
	case Mode::Displayed:
		r.readyNs = r.presentStartNs + kReadyAfterNs;
		r.screenNs = r.presentStartNs + kScreenAfterNs;
		r.screenStage = 4;
		r.flags = pml::kFrameTimingRequested | pml::kFrameTimingComplete | pml::kFrameDisplayed;
		break;
	}
	return r;
}

inline pml::SwapchainInfo makeSwapchainInfo(uint64_t baseNs)
{
	pml::SwapchainInfo s;
	std::memset(&s, 0, sizeof(s));
	s.serial = 1;
	s.handle = kHandle;
	s.createdNs = baseNs;
	s.width = 2560;
	s.height = 1440;
	s.format = 44;   // B8G8R8A8_UNORM
	s.colorSpace = 0;
	s.presentMode = kVkFifo;
	s.minImageCount = 2;
	s.imageCount = 3;
	s.surfaceKind = uint32_t(pml::SurfaceKind::Wayland);
	s.timingStagesSupported = 0x5;
	s.timingStagesUsed = 0x5;
	s.timingFlags = pml::kSwapchainLayerTiming;
	std::strncpy(s.engineName, "DXVK", sizeof(s.engineName) - 1);
	return s;
}

} // namespace faketl
