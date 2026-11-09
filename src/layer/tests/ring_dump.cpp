// SPDX-License-Identifier: MIT
// ring_dump: prints the FrameRecords of a presentmon-linux shared-memory segment as CSV.
//
//   ring_dump --list                          segments currently in /dev/shm
//   ring_dump --pid 1234 [--seconds 10]       follow a live process (stops when it exits)
//   ring_dump --name presentmon-linux.1234 --oldest --once   dump what is in the ring now
//   ring_dump --pid 1234 --info               segment header + swapchain table only
//   ring_dump --wait-for-segment 5 --pid ...  wait for the segment to appear
//
// Records are printed in ring order. Times are CLOCK_MONOTONIC nanoseconds (0 = unknown). The reader
// maps the segment before the producer unlinks it, so everything stays readable after the process exited.
// SIGINT/SIGTERM stop it cleanly: what is in the ring is printed and stdout flushed first.
#include "pml/clock.hpp"
#include "pml/shm.hpp"

#include <signal.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace pml;

namespace {

volatile std::sig_atomic_t gStop = 0;
void onStop(int) { gStop = 1; }

std::string flagText(uint32_t flags)
{
	std::string s;
	auto add = [&](uint32_t bit, char c) {
		if (flags & bit)
			s += c;
	};
	add(kFrameTimingRequested, 'T');
	add(kFrameTimingComplete, 'C');
	add(kFrameTimingLost, 'L');
	add(kFrameDisplayed, 'D');
	add(kFrameNotDisplayed, 'N');
	add(kFrameAppPresentId, 'I');
	add(kFrameAppTimingRequest, 'A');
	add(kFrameOverlayDrawn, 'O');
	add(kFramePresentFailed, 'F');
	add(kFrameSuboptimal, 'S');
	add(kFrameCapWait, 'W');
	add(kFrameCapReflex, 'R');
	return s.empty() ? "-" : s;
}

void printInfo(const Segment &segment)
{
	const SegmentHeader &h = segment.header();
	std::printf("# segment %s pid %d process '%s' engine '%s' app '%s' state %u layer %s bits %u ring %u\n",
	            segment.name().c_str(), h.pid, h.processName, h.engineName, h.applicationName, atomicLoad(h.state),
	            h.layerVersion, h.layerBits, h.ringCapacity);
	uint32_t high = atomicLoad(h.swapchainHighWater);
	for (uint32_t i = 0; i < high; i++)
	{
		SwapchainInfo info;
		if (!readSwapchain(segment, i, &info))
			continue;
		std::printf("# swapchain serial %llu handle 0x%llx %ux%u fmt %u cs %u mode %u images %u surface %u refresh_ns %llu "
		            "stages_supported 0x%x used 0x%x timing_flags 0x%x domain %u domain_id %llu destroyed_ns %llu\n",
		            (unsigned long long)info.serial, (unsigned long long)info.handle, info.width, info.height, info.format,
		            info.colorSpace, info.presentMode, info.imageCount, info.surfaceKind,
		            (unsigned long long)info.refreshDurationNs, info.timingStagesSupported, info.timingStagesUsed,
		            info.timingFlags, info.timeDomain, (unsigned long long)info.timeDomainId,
		            (unsigned long long)info.destroyedNs);
	}
}

void printHeader()
{
	std::printf("swapchain,frame,present_id,present_start_ns,present_end_ns,ready_ns,latch_ns,screen_ns,screen_stage,"
	            "flags,flag_text,result,present_mode,image,overlay_cpu_ns,thread,gpu_start_ns,gpu_busy_ns,cap_wait_ns\n");
}

void printRecord(const FrameRecord &r)
{
	std::printf("%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,0x%x,%s,%d,%u,%u,%llu,%u,%llu,%llu,%llu\n",
	            (unsigned long long)r.swapchainSerial, (unsigned long long)r.frameIndex, (unsigned long long)r.presentId,
	            (unsigned long long)r.presentStartNs, (unsigned long long)r.presentEndNs, (unsigned long long)r.readyNs,
	            (unsigned long long)r.latchNs, (unsigned long long)r.screenNs, r.screenStage, r.flags,
	            flagText(r.flags).c_str(), r.presentResult, r.presentMode, r.imageIndex,
	            (unsigned long long)r.overlayCpuNs, r.threadId, (unsigned long long)r.gpuStartNs,
	            (unsigned long long)r.gpuBusyNs, (unsigned long long)r.capWaitNs);
}

} // namespace

int main(int argc, char **argv)
{
	std::string name;
	bool list = false, info = false, oldest = false, once = false, noHeader = false;
	double seconds = 0, waitForSegment = 0;
	for (int i = 1; i < argc; i++)
	{
		std::string a = argv[i];
		auto value = [&]() -> const char * { return i + 1 < argc ? argv[++i] : ""; };
		if (a == "--list") list = true;
		else if (a == "--pid") name = segmentNameForPid(std::atoi(value()));
		else if (a == "--name") name = value();
		else if (a == "--seconds") seconds = std::atof(value());
		else if (a == "--info") info = true;
		else if (a == "--oldest") oldest = true;
		else if (a == "--once") once = true;
		else if (a == "--no-header") noHeader = true;
		else if (a == "--wait-for-segment") waitForSegment = std::atof(value());
		else
		{
			std::fprintf(stderr, "usage: ring_dump [--list] [--pid N | --name NAME] [--seconds S] [--info] [--oldest] [--once]\n");
			return 2;
		}
	}

	if (list)
	{
		for (const SegmentListing &l : listSegments())
			std::printf("%s pid=%d\n", l.name.c_str(), l.pid);
		return 0;
	}
	if (name.empty())
	{
		auto segments = listSegments();
		if (segments.size() != 1)
		{
			std::fprintf(stderr, "ring_dump: %zu segments; pass --pid or --name\n", segments.size());
			return 2;
		}
		name = segments[0].name;
	}

	std::unique_ptr<Segment> segment;
	std::string error;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(waitForSegment * 1000));
	for (;;)
	{
		segment = Segment::open(name, &error);
		if (segment || std::chrono::steady_clock::now() >= deadline)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	if (!segment)
	{
		std::fprintf(stderr, "ring_dump: cannot open %s: %s\n", name.c_str(), error.c_str());
		return 1;
	}

	if (info)
	{
		printInfo(*segment);
		return 0;
	}

	RingReader reader(*segment, oldest ? RingReader::Start::Oldest : RingReader::Start::Newest);
	struct sigaction sa = {};
	sa.sa_handler = onStop;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);
	if (!noHeader)
		printHeader();
	std::vector<FrameRecord> buffer(256);
	auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(seconds * 1000));
	for (;;)
	{
		size_t n = reader.read(buffer.data(), buffer.size());
		for (size_t i = 0; i < n; i++)
			printRecord(buffer[i]);
		if (once && n == 0)
			break;
		if (n == 0)
		{
			if (gStop || atomicLoad(segment->header().state) == uint32_t(SegmentState::Exited))
				break;
			if (seconds > 0 && std::chrono::steady_clock::now() >= end)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}
	if (reader.lostRecords())
		std::fprintf(stderr, "ring_dump: %llu records lost (ring overrun)\n", (unsigned long long)reader.lostRecords());
	std::fflush(stdout);
	return 0;
}
