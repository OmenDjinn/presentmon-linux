// SPDX-License-Identifier: MIT
// `presentmon cap <fps>|auto|off|status`: writes the CapControl area of live segments (src/layer/frame_limiter.hpp).
#include "commands.hpp"

#include "options.hpp"

#include <pml/clock.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <unistd.h>

namespace pml::cli {

namespace {

void usage(FILE *out)
{
	std::fputs("usage: presentmon cap <fps>|auto|off|status [--process_name N] [--process_id P]\n"
	           "                      [--placement late|early] [--max F] [--reflex|--no-reflex]\n"
	           "  <fps>        cap at this frame rate (e.g. 141 or 59.94)\n"
	           "  auto         just under the VRR maximum of the output with VRR on (224 at 240 Hz); no cap while\n"
	           "               VRR is off. --max F also caps it at F\n"
	           "  off          no cap\n"
	           "  status       print the setting and what is in force\n"
	           "  --placement  late (default): wait after the driver's present, or, when the game has a frame-pacing\n"
	           "               thread (DXVK, VKD3D), hold that thread instead, as their own limiters do (lowest latency)\n"
	           "               early: wait before the present (most even present spacing, more latency)\n"
	           "  --no-reflex  never use the game's Reflex (NV_low_latency2) interval; --reflex allows it again\n"
	           "  Without --process_name/--process_id every live layer-enabled process is addressed.\n",
	           out);
}

const char *methodName(uint32_t m)
{
	switch (pml::CapMethod(m))
	{
	case pml::CapMethod::None: return "none";
	case pml::CapMethod::PresentLate: return "present wait (late)";
	case pml::CapMethod::PresentEarly: return "present wait (early)";
	case pml::CapMethod::Reflex: return "Reflex interval";
	case pml::CapMethod::FrameThread: return "hold on the game's frame-pacing thread";
	}
	return "?";
}

} // namespace

// pending: the setting was just written and the layer has not applied it yet (no present since).
std::string describeCap(const pml::CapControl &c, bool pending)
{
	if (!c.available)
		return "not available (the process runs an older layer: restart it)";
	char setting[96];
	switch (pml::CapMode(c.mode))
	{
	case pml::CapMode::Fixed:
		std::snprintf(setting, sizeof(setting), "%.3f fps", double(c.fpsMilli) / 1000.0);
		break;
	case pml::CapMode::Auto:
		if (c.fpsMilli)
			std::snprintf(setting, sizeof(setting), "auto (VRR), at most %.3f fps", double(c.fpsMilli) / 1000.0);
		else
			std::snprintf(setting, sizeof(setting), "auto (VRR)");
		break;
	default:
		std::snprintf(setting, sizeof(setting), "off");
		break;
	}
	std::string out = setting;
	if (c.mode != uint32_t(pml::CapMode::Off))
	{
		out += c.placement == uint32_t(pml::CapPlacement::Early) ? ", early" : ", late";
		if (c.flags & pml::kCapNoReflex)
			out += ", no Reflex";
	}
	char state[192];
	if (pending)
		out += "; not applied yet (the game has not presented a frame since)";
	else if (c.effectiveMilli)
	{
		int n = std::snprintf(state, sizeof(state), "; in force: %.3f fps via %s", double(c.effectiveMilli) / 1000.0,
		                      methodName(c.method));
		if (c.method == uint32_t(pml::CapMethod::PresentLate) || c.method == uint32_t(pml::CapMethod::PresentEarly) ||
		    c.method == uint32_t(pml::CapMethod::FrameThread))
			std::snprintf(state + n, sizeof(state) - size_t(n), ", mean wait %.2f ms, release p99 %u us",
			              double(c.meanWaitUs) / 1000.0, c.lateP99Us);
		out += state;
	}
	else if (c.mode != uint32_t(pml::CapMode::Off))
	{
		out += "; in force: nothing";
		if (c.mode == uint32_t(pml::CapMode::Auto))
			out += c.vrr == 2 ? " (VRR is off on every output)" : c.vrr == 0 ? " (VRR state unknown)" : "";
	}
	if (c.mode == uint32_t(pml::CapMode::Auto) && c.refreshMilli)
	{
		char hz[48];
		std::snprintf(hz, sizeof(hz), " (VRR output %.2f Hz)", double(c.refreshMilli) / 1000.0);
		out += hz;
	}
	return out;
}

int runCap(const std::vector<std::string> &args)
{
	if (args.empty())
	{
		usage(stderr);
		return 1;
	}
	if (args[0] == "help" || matchArg(args[0], "help") || matchArg(args[0], "h") || matchArg(args[0], "?"))
	{
		usage(stdout);
		return 0;
	}
	const std::string &action = args[0];
	const bool status = action == "status";
	uint32_t mode = 0, fpsMilli = 0;
	if (!status)
	{
		if (strcasecmp(action.c_str(), "off") == 0)
			mode = uint32_t(pml::CapMode::Off);
		else if (strcasecmp(action.c_str(), "auto") == 0)
			mode = uint32_t(pml::CapMode::Auto);
		else
		{
			char *end = nullptr;
			const double fps = std::strtod(action.c_str(), &end);
			if (end == action.c_str() || *end != '\0' || !(fps >= 1.0 && fps <= 10000.0))
			{
				std::fprintf(stderr, "error: '%s' is not a frame rate (1-10000), 'auto', 'off' or 'status'.\n", action.c_str());
				usage(stderr);
				return 1;
			}
			mode = uint32_t(pml::CapMode::Fixed);
			fpsMilli = uint32_t(std::lround(fps * 1000.0));
		}
	}

	SegmentFilter filter;
	bool havePlacement = false, haveReflex = false, haveMax = false;
	uint32_t placement = 0, maxMilli = 0;
	bool reflex = true;
	for (size_t i = 1; i < args.size(); ++i)
	{
		std::string error;
		int r = filter.parse(args, i, &error);
		if (r < 0)
		{
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		if (r > 0)
			continue;
		if (matchArg(args[i], "placement"))
		{
			if (i + 1 >= args.size() || (strcasecmp(args[i + 1].c_str(), "late") && strcasecmp(args[i + 1].c_str(), "early")))
			{
				std::fputs("error: --placement expects late or early.\n", stderr);
				return 1;
			}
			placement = strcasecmp(args[++i].c_str(), "early") == 0 ? uint32_t(pml::CapPlacement::Early)
			                                                         : uint32_t(pml::CapPlacement::Late);
			havePlacement = true;
		}
		else if (matchArg(args[i], "max"))
		{
			char *end = nullptr;
			const double f = i + 1 < args.size() ? std::strtod(args[i + 1].c_str(), &end) : 0.0;
			if (i + 1 >= args.size() || end == args[i + 1].c_str() || *end != '\0' || !(f >= 1.0 && f <= 10000.0))
			{
				std::fputs("error: --max expects a frame rate (1-10000).\n", stderr);
				return 1;
			}
			++i;
			maxMilli = uint32_t(std::lround(f * 1000.0));
			haveMax = true;
		}
		else if (matchArg(args[i], "reflex") || matchArg(args[i], "no-reflex"))
		{
			reflex = matchArg(args[i], "reflex");
			haveReflex = true;
		}
		else
		{
			std::fprintf(stderr, "error: unrecognized option '%s'.\n", args[i].c_str());
			return 1;
		}
	}
	if (status && (havePlacement || haveReflex || haveMax))
	{
		std::fputs("error: options cannot be used with 'status'.\n", stderr);
		return 1;
	}
	if (haveMax && mode != uint32_t(pml::CapMode::Auto))
	{
		std::fputs("error: --max only applies to 'auto'.\n", stderr);
		return 1;
	}

	size_t unreadable = 0, hiddenStale = 0;
	auto found = findSegments(filter, &unreadable, &hiddenStale);
	size_t matched = 0;
	for (auto &f : found)
	{
		if (!f.alive)
			continue;
		pml::Segment &seg = *f.segment;
		const auto &h = seg.header();
		++matched;
		pml::CapControl ctl = pml::readCapControl(seg);
		bool pending = false;
		if (!status)
		{
			ctl.mode = mode;
			ctl.fpsMilli = mode == uint32_t(pml::CapMode::Auto) ? (haveMax ? maxMilli : 0) : fpsMilli;
			if (havePlacement)
				ctl.placement = placement;
			if (haveReflex)
				ctl.flags = reflex ? (ctl.flags & ~uint32_t(pml::kCapNoReflex)) : (ctl.flags | pml::kCapNoReflex);
			const uint32_t written = pml::writeCapControl(seg, ctl);
			// The layer applies it on its next present and echoes the generation with its status: print that
			// status, not the one from before the change (a game that is not presenting applies it later).
			const uint64_t until = pml::monotonicNs() + 500'000'000ull;
			for (;;)
			{
				ctl = pml::readCapControl(seg);
				pending = ctl.available && int32_t(ctl.appliedGeneration - written) < 0;
				if (!pending || pml::monotonicNs() >= until)
					break;
				::usleep(10'000);
			}
		}
		std::printf("%s[%d]: cap %s\n", pml::headerString(h.processName).c_str(), int(h.pid),
		            describeCap(ctl, pending).c_str());
		if (!status && !ctl.available)
			std::fprintf(stderr, "warning: %s[%d] runs a layer without frame caps; the setting is stored but has no effect.\n",
			             pml::headerString(h.processName).c_str(), int(h.pid));
	}

	if (matched == 0)
	{
		const bool filtered = filter.pid != 0 || !filter.processNames.empty();
		if (filtered || !status)
		{
			std::fputs("no matching live layer-enabled process\n", stderr);
			return 1;
		}
		std::puts("no layer-enabled processes (start a game with PRESENTMON_LAYER=1)");
	}
	return 0;
}

} // namespace pml::cli
