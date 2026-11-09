// SPDX-License-Identifier: MIT
// `presentmon overlay show|hide|toggle|status`: writes the OverlayControl area of live segments.
#include "commands.hpp"

#include "options.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace pml::cli {

std::string describeOverlay(const pml::OverlayControl &o);   // list_cmd.cpp

namespace {

void usage(FILE *out)
{
	std::fputs("usage: presentmon overlay show|hide|toggle|status [--process_name N] [--process_id P]\n"
	           "                                              [--position top-left|top-right|bottom-left|bottom-right]\n"
	           "                                              [--scale F|auto]\n"
	           "  Without --process_name/--process_id every live layer-enabled process is addressed.\n"
	           "  --position and --scale apply to show and toggle; status only reads.\n",
	           out);
}

bool parsePosition(const char *s, pml::OverlayPosition *out)
{
	static const struct { const char *name; pml::OverlayPosition pos; } kNames[] = {
		{ "top-left", pml::OverlayPosition::TopLeft },
		{ "top-right", pml::OverlayPosition::TopRight },
		{ "bottom-left", pml::OverlayPosition::BottomLeft },
		{ "bottom-right", pml::OverlayPosition::BottomRight },
	};
	for (const auto &n : kNames)
		if (strcasecmp(s, n.name) == 0)
		{
			*out = n.pos;
			return true;
		}
	return false;
}

} // namespace

int runOverlay(const std::vector<std::string> &args)
{
	if (args.empty())
	{
		usage(stderr);
		return 1;
	}
	if (matchArg(args[0], "help") || matchArg(args[0], "h") || matchArg(args[0], "?"))
	{
		usage(stdout);
		return 0;
	}
	const std::string &action = args[0];
	if (action != "show" && action != "hide" && action != "toggle" && action != "status")
	{
		std::fprintf(stderr, "error: unknown overlay action '%s' (expected show, hide, toggle or status).\n",
		             action.c_str());
		usage(stderr);
		return 1;
	}

	SegmentFilter filter;
	bool havePosition = false, haveScale = false;
	pml::OverlayPosition position = pml::OverlayPosition::TopLeft;
	uint32_t scalePermille = 0;
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
		if (matchArg(args[i], "position"))
		{
			if (i + 1 >= args.size() || !parsePosition(args[i + 1].c_str(), &position))
			{
				std::fputs("error: --position expects top-left, top-right, bottom-left or bottom-right.\n", stderr);
				return 1;
			}
			++i;
			havePosition = true;
		}
		else if (matchArg(args[i], "scale"))
		{
			if (i + 1 >= args.size())
			{
				std::fputs("error: --scale expecting a number or 'auto'.\n", stderr);
				return 1;
			}
			const std::string &v = args[++i];
			if (strcasecmp(v.c_str(), "auto") == 0)
				scalePermille = 0;
			else
			{
				char *end = nullptr;
				double f = std::strtod(v.c_str(), &end);
				if (end == v.c_str() || *end != '\0' || !(f >= 0.25 && f <= 8.0))
				{
					std::fprintf(stderr, "error: --scale expects a number between 0.25 and 8, or 'auto' (got '%s').\n",
					             v.c_str());
					return 1;
				}
				scalePermille = uint32_t(f * 1000.0 + 0.5);
			}
			haveScale = true;
		}
		else
		{
			std::fprintf(stderr, "error: unrecognized option '%s'.\n", args[i].c_str());
			return 1;
		}
	}
	if (action == "status" && (havePosition || haveScale))
	{
		std::fputs("error: --position and --scale cannot be used with 'status'.\n", stderr);
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

		pml::OverlayControl ctl = pml::readOverlayControl(seg);
		if (action != "status")
		{
			if (action == "show")
				ctl.visible = 1;
			else if (action == "hide")
				ctl.visible = 0;
			else
				ctl.visible = ctl.visible ? 0 : 1;
			if (havePosition)
				ctl.position = uint32_t(position);
			if (haveScale)
				ctl.scalePermille = scalePermille;
			pml::writeOverlayControl(seg, ctl);
			ctl = pml::readOverlayControl(seg);
		}
		std::printf("%s[%d]: overlay %s\n", pml::headerString(h.processName).c_str(), int(h.pid), describeOverlay(ctl).c_str());
		if (action != "status" && !ctl.available)
			std::fprintf(stderr, "warning: the overlay is not loaded in %s[%d]; the setting is stored but nothing will be drawn.\n",
			             pml::headerString(h.processName).c_str(), int(h.pid));
	}

	if (matched == 0)
	{
		const bool filtered = filter.pid != 0 || !filter.processNames.empty();
		if (filtered || action != "status")
		{
			std::fputs("no matching live layer-enabled process\n", stderr);
			return 1;
		}
		std::puts("no layer-enabled processes (start a game with PRESENTMON_LAYER=1)");
	}
	return 0;
}

} // namespace pml::cli
