// SPDX-License-Identifier: MIT
// Stand-in for VkLayer_presentmon in tests: creates this process's segment exactly like the layer
// does, publishes a deterministic timeline (fake_timeline.hpp), stays alive for a while, then exits
// cleanly (state Exited + unlink) or dies without cleanup.
//
//   pml_fake_producer [--frames N] [--period-us U] [--mode displayed|nodisplay|drop3]
//                     [--start-delay-ms D] [--linger-ms L] [--ring N] [--process-name NAME]
//                     [--no-overlay] [--crash]   (--crash: SIGKILL self after lingering, no cleanup)
// Prints "published <n> <segment>" on stdout once all frames are in the ring.
#include "fake_timeline.hpp"

#include <pml/clock.hpp>
#include <pml/shm.hpp>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <time.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t gStop = 0;
void onSignal(int) { gStop = 1; }

void sleepMs(uint32_t ms)
{
	// Sliced so SIGTERM ends the linger promptly.
	for (uint32_t waited = 0; waited < ms && !gStop; waited += 10)
	{
		timespec ts = { 0, 10 * 1000 * 1000 };
		nanosleep(&ts, nullptr);
	}
}
} // namespace

int main(int argc, char **argv)
{
	faketl::Params params;
	uint32_t startDelayMs = 0, lingerMs = 1000, ring = 8192;
	std::string processName = "FakeGame.exe";
	bool overlay = true, crash = false;
	for (int i = 1; i < argc; ++i)
	{
		auto is = [&](const char *n) { return std::strcmp(argv[i], n) == 0 && i + 1 < argc; };
		if (is("--frames")) params.frames = uint32_t(std::atoi(argv[++i]));
		else if (is("--period-us")) params.periodNs = uint64_t(std::atoll(argv[++i])) * 1000;
		else if (is("--start-delay-ms")) startDelayMs = uint32_t(std::atoi(argv[++i]));
		else if (is("--linger-ms")) lingerMs = uint32_t(std::atoi(argv[++i]));
		else if (is("--ring")) ring = uint32_t(std::atoi(argv[++i]));
		else if (is("--process-name")) processName = argv[++i];
		else if (is("--mode"))
		{
			std::string m = argv[++i];
			params.mode = m == "nodisplay" ? faketl::Mode::NoDisplay : m == "drop3" ? faketl::Mode::Drop3
			                                                                         : faketl::Mode::Displayed;
		}
		else if (!std::strcmp(argv[i], "--no-overlay")) overlay = false;
		else if (!std::strcmp(argv[i], "--crash")) crash = true;
		else
		{
			std::fprintf(stderr, "pml_fake_producer: bad argument %s\n", argv[i]);
			return 2;
		}
	}
	std::signal(SIGTERM, onSignal);
	std::signal(SIGINT, onSignal);

	pml::ProcessIdentity id = pml::currentProcessIdentity();
	id.processName = processName;
	id.engineName = "DXVK";
	id.applicationName = "FakeGame";
	id.layerVersion = "fake-0";
	const std::string name = pml::segmentNameForPid(id.pid);

	pml::OverlayControl initial = {};
	initial.visible = 1;
	initial.position = uint32_t(pml::OverlayPosition::TopLeft);
	std::string error;
	auto segment = pml::Segment::create(name, id, ring, initial, &error);
	if (!segment)
	{
		std::fprintf(stderr, "pml_fake_producer: %s\n", error.c_str());
		return 1;
	}
	if (overlay)
		pml::setOverlayAvailable(*segment, true);

	sleepMs(startDelayMs);

	// The timeline starts "now" so it lies inside any recording window of a collector that attached
	// before this process started publishing.
	params.baseNs = pml::monotonicNs();
	pml::publishSwapchain(*segment, faketl::makeSwapchainInfo(params.baseNs));
	pml::RingWriter writer(*segment);
	for (uint32_t i = 1; i <= params.frames; ++i)
	{
		writer.publish(faketl::makeFrame(params, i));
		pml::touchHeartbeat(*segment, pml::monotonicNs());
	}
	std::printf("published %u %s\n", params.frames, name.c_str());
	std::fflush(stdout);

	// Keep the heartbeat fresh while the "game" runs.
	for (uint32_t waited = 0; waited < lingerMs && !gStop; waited += 50)
	{
		pml::touchHeartbeat(*segment, pml::monotonicNs());
		sleepMs(50);
	}

	if (crash)
		::raise(SIGKILL);

	pml::setState(*segment, pml::SegmentState::Exited);
	segment->unlink();
	return 0;
}
