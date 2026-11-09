// SPDX-License-Identifier: MIT
// presentmon: PresentMon for Linux. Capture (default), list, overlay, cap.
#include "capture.hpp"
#include "commands.hpp"
#include "options.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
	using namespace pml::cli;
	std::vector<std::string> args(argv + 1, argv + argc);

	if (!args.empty())
	{
		const std::string cmd = args[0];
		if (cmd == "list" || cmd == "overlay" || cmd == "cap")
		{
			args.erase(args.begin());
			return cmd == "list" ? runList(args) : cmd == "overlay" ? runOverlay(args) : runCap(args);
		}
		if (cmd == "capture")
			args.erase(args.begin());
		else if (cmd == "version" || cmd == "--version")
		{
			std::puts(versionString());
			return 0;
		}
	}

	CaptureOptions options;
	ParseResult parsed = parseCaptureOptions(args, &options);
	if (parsed.help)
	{
		printUsage(stdout);
		return 0;
	}
	if (!parsed.ok)
	{
		std::fprintf(stderr, "%s\n", parsed.error.c_str());
		std::fputs("run 'presentmon --help' for usage.\n", stderr);
		return 1;
	}
	for (const auto &w : parsed.warnings)
		std::fprintf(stderr, "%s\n", w.c_str());

	return runCapture(options);
}
