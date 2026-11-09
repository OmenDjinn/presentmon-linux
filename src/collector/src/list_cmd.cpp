// SPDX-License-Identifier: MIT
// `presentmon list`: live layer-enabled processes with their swapchains and overlay state.
#include "commands.hpp"

#include "options.hpp"

#include <pml/clock.hpp>
#include <pml/metrics/adapter.hpp>
#include <pml/process_info.hpp>

#include <cstdio>
#include <cstring>
#include <string>

namespace pml::cli {

int SegmentFilter::parse(const std::vector<std::string> &args, size_t &i, std::string *error)
{
	const std::string &a = args[i];
	if (matchArg(a, "process_name"))
	{
		if (i + 1 >= args.size())
		{
			*error = "error: " + a + " expecting argument.";
			return -1;
		}
		processNames.push_back(canonicalizeProcessName(args[++i]));
		return 1;
	}
	if (matchArg(a, "process_id"))
	{
		if (i + 1 >= args.size() || !parseUint(args[i + 1], &pid))
		{
			*error = "error: " + a + " expecting a process id.";
			return -1;
		}
		++i;
		return 1;
	}
	return 0;
}

bool SegmentFilter::matches(int segmentPid, const std::string &processName) const
{
	if (pid == 0 && processNames.empty())
		return true;
	if (pid != 0 && pid == uint32_t(segmentPid))
		return true;
	const std::string canon = canonicalizeProcessName(processName);
	for (const auto &n : processNames)
		if (n == canon)
			return true;
	return false;
}

bool producerAlive(int pid, uint64_t startTicks)
{
	// pml::processAlive also treats a zombie (exited, not yet reaped) as dead.
	return pml::processAlive(pid, startTicks);
}

std::vector<FoundSegment> findSegments(const SegmentFilter &filter, size_t *unreadable, size_t *hiddenStale)
{
	std::vector<FoundSegment> found;
	const uint64_t now = pml::monotonicNs();
	for (const auto &listing : pml::listSegments())
	{
		// The pid in the name is enough to skip most segments without mapping them.
		if (filter.pid != 0 && filter.processNames.empty() && filter.pid != uint32_t(listing.pid))
			continue;
		pml::OpenFailure why = pml::OpenFailure::None;
		auto segment = pml::Segment::open(listing.name, nullptr, &why);
		if (!segment)
		{
			// Half-created or other-version leftovers of dead processes are not "still initializing".
			const bool leftover = pml::leftoverFailure(why) && pml::segmentFileAbandoned(listing.name);
			if (leftover && hiddenStale)
				++*hiddenStale;
			else if (unreadable)
				++*unreadable;
			continue;
		}
		const auto &h = segment->header();
		if (!filter.matches(h.pid, pml::headerString(h.processName)))
			continue;
		FoundSegment f;
		f.name = listing.name;
		f.stale = pml::segmentIsStale(*segment, now);
		f.alive = !f.stale && pml::atomicLoad(h.state) != uint32_t(pml::SegmentState::Exited) &&
		          producerAlive(h.pid, h.processStartTicks);
		if (!f.alive)
		{
			// A process that died (or was killed) is not listed, whether or not its segment is stale yet.
			if (hiddenStale)
				++*hiddenStale;
			continue;
		}
		f.segment = std::move(segment);
		found.push_back(std::move(f));
	}
	return found;
}

namespace {

const char *vkFormatName(uint32_t f)
{
	switch (f)
	{
	case 37: return "R8G8B8A8_UNORM";
	case 43: return "R8G8B8A8_SRGB";
	case 44: return "B8G8R8A8_UNORM";
	case 50: return "B8G8R8A8_SRGB";
	case 58: return "A2R10G10B10_UNORM";
	case 64: return "A2B10G10R10_UNORM";
	case 97: return "R16G16B16A16_SFLOAT";
	default: return nullptr;
	}
}

const char *colorSpaceName(uint32_t c)
{
	switch (c)
	{
	case 0: return "SRGB_NONLINEAR";
	case 1000104001: return "DISPLAY_P3_NONLINEAR";
	case 1000104002: return "EXTENDED_SRGB_LINEAR";
	case 1000104008: return "HDR10_ST2084";
	case 1000104009: return "DOLBYVISION";
	case 1000104010: return "HDR10_HLG";
	default: return nullptr;
	}
}

const char *surfaceName(uint32_t k)
{
	switch (pml::SurfaceKind(k))
	{
	case pml::SurfaceKind::Wayland: return "Wayland";
	case pml::SurfaceKind::Xcb: return "Xcb";
	case pml::SurfaceKind::Xlib: return "Xlib";
	case pml::SurfaceKind::Display: return "Display";
	case pml::SurfaceKind::Headless: return "Headless";
	case pml::SurfaceKind::Other: return "Other";
	default: return "Unknown";
	}
}

// VkPresentStageFlagBitsEXT
std::string stageNames(uint32_t mask)
{
	if (mask == 0)
		return "none";
	std::string s;
	auto add = [&](uint32_t bit, const char *n) {
		if (mask & bit)
			s += (s.empty() ? "" : "+") + std::string(n);
	};
	add(0x1, "QUEUE_OPERATIONS_END");
	add(0x2, "REQUEST_DEQUEUED");
	add(0x4, "IMAGE_FIRST_PIXEL_OUT");
	add(0x8, "IMAGE_FIRST_PIXEL_VISIBLE");
	if (s.empty())
		s = "0x" + std::to_string(mask);
	return s;
}

std::string timingFlagNames(uint32_t f)
{
	std::string s;
	auto add = [&](uint32_t bit, const char *n) {
		if (f & bit)
			s += (s.empty() ? "" : ",") + std::string(n);
	};
	add(pml::kSwapchainLayerTiming, "layer-timing");
	add(pml::kSwapchainAppTiming, "app-timing");
	add(pml::kSwapchainAppPresentId2, "app-present_id2");
	add(pml::kSwapchainLayerPresentId2, "layer-present_id2");
	add(pml::kSwapchainAppPresentId1, "app-present_id");
	add(pml::kSwapchainOverlay, "overlay-resources");
	return s.empty() ? "-" : s;
}

} // namespace

// Shared with the overlay command so `list` and `overlay status` describe the overlay the same way.
std::string describeOverlay(const pml::OverlayControl &o);

std::string describeOverlay(const pml::OverlayControl &o)
{
	static const char *const kPositions[] = { "top-left", "top-right", "bottom-left", "bottom-right" };
	char scale[32];
	if (o.scalePermille == 0)
		std::snprintf(scale, sizeof(scale), "auto");
	else
		std::snprintf(scale, sizeof(scale), "%.2f", double(o.scalePermille) / 1000.0);
	char buf[160];
	std::snprintf(buf, sizeof(buf), "%s, %s, scale %s, %s", o.visible ? "visible" : "hidden",
	              o.position < 4 ? kPositions[o.position] : "?", scale,
	              o.available ? "available" : "not available (PRESENTMON_OVERLAY=off or not loaded)");
	return buf;
}

int runList(const std::vector<std::string> &args)
{
	SegmentFilter filter;
	for (size_t i = 0; i < args.size(); ++i)
	{
		std::string error;
		int r = filter.parse(args, i, &error);
		if (r < 0)
		{
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		if (r == 0)
		{
			if (matchArg(args[i], "help") || matchArg(args[i], "h") || matchArg(args[i], "?"))
			{
				std::puts("usage: presentmon list [--process_name N] [--process_id P]");
				return 0;
			}
			std::fprintf(stderr, "error: unrecognized option '%s'.\n", args[i].c_str());
			return 1;
		}
	}

	size_t unreadable = 0, hiddenStale = 0;
	auto found = findSegments(filter, &unreadable, &hiddenStale);
	const uint64_t now = pml::monotonicNs();

	if (found.empty())
		std::puts("no layer-enabled processes (start a game with PRESENTMON_LAYER=1)");

	for (const auto &f : found)
	{
		const pml::Segment &seg = *f.segment;
		const pml::SegmentHeader &h = seg.header();
		const uint64_t hb = pml::atomicLoad(h.heartbeatNs);
		char age[48];
		if (hb == 0)
			std::snprintf(age, sizeof(age), "never");
		else
			std::snprintf(age, sizeof(age), "%.2f s ago", now >= hb ? double(now - hb) * 1e-9 : 0.0);

		const char *state = f.alive ? "live" : "dead";
		if (pml::atomicLoad(h.state) == uint32_t(pml::SegmentState::Initializing))
			state = "initializing";
		const std::string procName = pml::headerString(h.processName);
		const std::string engine = pml::headerString(h.engineName);
		const std::string layerVersion = pml::headerString(h.layerVersion);
		std::printf("%s[%d]  engine=%s  layer=%s %u-bit  state=%s  heartbeat=%s\n", procName.c_str(), int(h.pid),
		            engine.empty() ? "-" : engine.c_str(), layerVersion.empty() ? "?" : layerVersion.c_str(),
		            h.layerBits, state, age);
		std::printf("    overlay: %s\n", describeOverlay(pml::readOverlayControl(seg)).c_str());
		std::printf("    cap: %s\n", describeCap(pml::readCapControl(seg)).c_str());
		std::printf("    segment: %s, ring %u records, written %llu, lost-by-layer %llu\n", f.name.c_str(),
		            h.ringCapacity, static_cast<unsigned long long>(pml::atomicLoad(h.writeSeq)),
		            static_cast<unsigned long long>(pml::atomicLoad(h.droppedRecords)));

		const uint32_t used = pml::atomicLoad(h.swapchainHighWater);
		bool any = false;
		for (uint32_t i = 0; i < used && i < pml::kMaxSwapchains; ++i)
		{
			pml::SwapchainInfo info;
			if (!pml::readSwapchain(seg, i, &info))
				continue;
			any = true;
			const char *fmt = vkFormatName(info.format);
			const char *cs = colorSpaceName(info.colorSpace);
			char fmtBuf[48], csBuf[48];
			if (!fmt) { std::snprintf(fmtBuf, sizeof(fmtBuf), "format %u", info.format); fmt = fmtBuf; }
			if (!cs) { std::snprintf(csBuf, sizeof(csBuf), "colorspace %u", info.colorSpace); cs = csBuf; }
			std::printf("    swapchain #%llu 0x%016llX  %ux%u  %s / %s  %s  %s%s\n",
			            static_cast<unsigned long long>(info.serial), static_cast<unsigned long long>(info.handle),
			            info.width, info.height, fmt, cs, pml::metrics::presentModeName(info.presentMode),
			            surfaceName(info.surfaceKind), info.destroyedNs ? "  (destroyed)" : "");
			std::printf("        images %u (min %u)  timing: stages supported=%s used=%s  flags=%s  engine=%s\n",
			            info.imageCount, info.minImageCount, stageNames(info.timingStagesSupported).c_str(),
			            stageNames(info.timingStagesUsed).c_str(), timingFlagNames(info.timingFlags).c_str(),
			            info.engineName[0] ? pml::headerString(info.engineName).c_str() : "-");
		}
		if (!any)
			std::puts("    (no swapchains yet)");
	}
	if (hiddenStale)
		std::printf("(%zu segment%s of dead processes hidden; a running capture or the next game start removes them)\n",
		            hiddenStale, hiddenStale == 1 ? "" : "s");
	if (unreadable)
		std::printf("(%zu segment%s still initializing or unreadable)\n", unreadable, unreadable == 1 ? "" : "s");
	return 0;
}

} // namespace pml::cli
