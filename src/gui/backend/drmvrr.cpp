// SPDX-License-Identifier: MIT
#include "drmvrr.hpp"

namespace pml::gui {

pml::metrics::VrrContext vrrContextFromOutputs(const std::vector<OutputVrr> &outputs, uint32_t width, uint32_t height,
                                               uint64_t refreshDurationNs, const OutputVrr **used)
{
	pml::metrics::VrrContext ctx;
	if (used)
		*used = nullptr;
	if (refreshDurationNs)
		ctx.refreshHz = 1e9 / double(refreshDurationNs);
	if (const OutputVrr *o = pml::pickOutput(outputs, width, height, refreshDurationNs))
	{
		ctx.refreshHz = o->refreshHz;
		if (o->enabled == 1)
		{
			ctx.driver = pml::metrics::DriverVrr::On;
			ctx.vrrMinHz = o->edidMinHz;
			ctx.floor = pml::rescansAtVrrFloor(*o) ? pml::metrics::VrrFloor::Rescan : pml::metrics::VrrFloor::Unknown;
		}
		else if (o->enabled == 0)
		{
			ctx.driver = pml::metrics::DriverVrr::Off;
		}
		if (used)
			*used = o;
		return ctx;
	}
	// Which output the game is on is unknown (several, no refresh cycle from the driver) and none has VRR on.
	for (const OutputVrr &o : outputs)
		if (o.enabled >= 0)
		{
			ctx.driver = pml::metrics::DriverVrr::Off;
			break;
		}
	return ctx;
}

} // namespace pml::gui
