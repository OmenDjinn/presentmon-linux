// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

namespace pml {

// Field 22 (starttime, clock ticks since boot) of /proc/<pid>/stat, 0 on failure.
// Together with the pid it identifies a process across PID reuse.
uint64_t readProcessStartTicks(int pid);

// readlink(/proc/<pid>/exe), empty on failure.
std::string readExePath(int pid);

// argv[0] from /proc/<pid>/cmdline, empty on failure.
std::string readArgv0(int pid);

// The name PresentMon would show in its Application column.
//  * Normal processes: basename of /proc/<pid>/exe.
//  * Wine/Proton processes (exe is wine, wine64, wine-preloader, wine64-preloader, ...):
//    the basename of the Windows path in argv[0], e.g. "Z:\\games\\foo\\Foo.exe" -> "Foo.exe".
std::string detectProcessName(int pid);

// True if a process with this pid exists and, when startTicks != 0, has that start time.
bool processAlive(int pid, uint64_t startTicks);

// gettid() of the calling thread.
uint32_t currentThreadId();

} // namespace pml
