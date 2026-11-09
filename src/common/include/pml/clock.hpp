// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <ctime>

namespace pml {

// The one timebase of presentmon-linux: CLOCK_MONOTONIC in nanoseconds. Shared by every
// process on the machine, so records from different games line up. Never returns 0.
inline uint64_t monotonicNs()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// The frequency to give PresentMon's QpcConverter for monotonicNs() timestamps.
inline constexpr uint64_t kTicksPerSecond = 1000000000ull;

} // namespace pml
