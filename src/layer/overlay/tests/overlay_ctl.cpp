// SPDX-License-Identifier: MIT
// Test helper: changes the overlay control area of a process's shared-memory segment, the same
// thing `presentmon overlay ...` will do.
//   overlay_ctl --pid N show|hide|toggle|status
//   overlay_ctl --pid N position top-left|top-right|bottom-left|bottom-right
//   overlay_ctl --pid N scale <float>|auto
//   overlay_ctl --pid N panels <mask>|default       (mask: OverlayPanel bits, decimal or 0x..)
#include <pml/shm.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv)
{
	int pid = 0;
	int i = 1;
	if (i + 1 < argc && !std::strcmp(argv[i], "--pid"))
	{
		pid = std::atoi(argv[i + 1]);
		i += 2;
	}
	if (!pid || i >= argc)
	{
		std::fprintf(stderr, "usage: overlay_ctl --pid N show|hide|toggle|status|position P|scale F|panels M\n");
		return 1;
	}
	std::string error;
	auto seg = pml::Segment::open(pml::segmentNameForPid(pid), &error);
	if (!seg)
	{
		std::fprintf(stderr, "overlay_ctl: cannot open segment of pid %d: %s\n", pid, error.c_str());
		return 2;
	}
	pml::OverlayControl c = pml::readOverlayControl(*seg);
	const std::string cmd = argv[i];
	const char *arg = i + 1 < argc ? argv[i + 1] : "";
	if (cmd == "show") c.visible = 1;
	else if (cmd == "hide") c.visible = 0;
	else if (cmd == "toggle") c.visible = c.visible ? 0 : 1;
	else if (cmd == "position")
	{
		const std::string p = arg;
		if (p == "top-left") c.position = uint32_t(pml::OverlayPosition::TopLeft);
		else if (p == "top-right") c.position = uint32_t(pml::OverlayPosition::TopRight);
		else if (p == "bottom-left") c.position = uint32_t(pml::OverlayPosition::BottomLeft);
		else if (p == "bottom-right") c.position = uint32_t(pml::OverlayPosition::BottomRight);
		else { std::fprintf(stderr, "bad position\n"); return 1; }
	}
	else if (cmd == "scale") c.scalePermille = std::string(arg) == "auto" ? 0 : uint32_t(std::atof(arg) * 1000.0 + 0.5);
	else if (cmd == "panels") c.panels = std::string(arg) == "default" ? 0 : uint32_t(std::strtoul(arg, nullptr, 0));
	else if (cmd != "status") { std::fprintf(stderr, "unknown command %s\n", cmd.c_str()); return 1; }

	if (cmd != "status")
		pml::writeOverlayControl(*seg, c);
	c = pml::readOverlayControl(*seg);
	std::printf("generation=%u visible=%u position=%u scale_permille=%u panels=0x%x available=%u\n", c.generation,
	            c.visible, c.position, c.scalePermille, c.panels, c.available);
	return 0;
}
