// SPDX-License-Identifier: MIT
// QpcConverter methods copied from PresentMon (third_party/PresentMon/IntelPresentMon/
// CommonUtilities/Qpc.cpp, Copyright (C) 2022 Intel Corporation, MIT). Qpc.cpp itself cannot
// be compiled on Linux (it includes win/WinAPI.h and log/Log.h for QueryPerformanceCounter
// and friends), and the header declares these four methods out of line. The bodies are
// verbatim; only the unrelated Windows functions of Qpc.cpp are left out.
#include <IntelPresentMon/CommonUtilities/Qpc.h>

namespace pmon::util
{
	// Duration in ticks -> ms
	double QpcConverter::TicksToMilliSeconds(uint64_t ticks) const noexcept
	{
		return ticks * msPerTick_;
	}

	// Unsigned delta (0 if end <= start or either is 0)
	double QpcConverter::DeltaUnsignedMilliSeconds(uint64_t start, uint64_t end) const noexcept
	{
		return (end <= start || start == 0 || end == 0) ? 0.0 : TicksToMilliSeconds(end - start);
	}

	// Signed delta (positive if end > start; negative if end < start; 0 if invalid)
	double QpcConverter::DeltaSignedMilliSeconds(uint64_t start, uint64_t end) const noexcept
	{
		if (start == 0 || end == 0 || start == end) {
			return 0.0;
		}
		return end > start ? TicksToMilliSeconds(end - start)
			: -TicksToMilliSeconds(start - end);
	}

	// Convenience: raw duration already a tick count (e.g. TimeInPresent)
	double QpcConverter::DurationMilliSeconds(uint64_t tickCount) const noexcept
	{
		return TicksToMilliSeconds(tickCount);
	}
}
