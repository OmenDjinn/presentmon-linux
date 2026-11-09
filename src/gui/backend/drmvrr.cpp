// SPDX-License-Identifier: MIT
#include "drmvrr.hpp"

namespace pml::gui {

pml::metrics::VrrContext vrrContextFromOutputs(const std::vector<OutputVrr> &outputs, std::string *which)
{
	pml::metrics::VrrContext ctx;
	if (which)
		which->clear();
	const OutputVrr *on = nullptr;
	size_t readable = 0, onCount = 0;
	for (const OutputVrr &o : outputs)
	{
		if (o.enabled < 0)
			continue;
		readable++;
		if (o.enabled == 1)
		{
			onCount++;
			on = &o;
		}
	}
	if (onCount == 1)
	{
		ctx.driver = pml::metrics::DriverVrr::On;
		ctx.refreshHz = on->refreshHz;
		ctx.vrrMinHz = on->edidMinHz;
		if (which)
			*which = on->name;
	}
	else if (readable > 0 && onCount == 0)
	{
		ctx.driver = pml::metrics::DriverVrr::Off;
	}
	return ctx;
}

} // namespace pml::gui
