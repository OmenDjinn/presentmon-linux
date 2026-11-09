// SPDX-License-Identifier: MIT
// Capture mode: discovers layer segments, reads their rings, runs the PresentMon metrics engine
// and writes CSV rows / console statistics. Single-threaded: one loop polls everything.
#pragma once

#include "options.hpp"

namespace pml::cli {

// Runs until SIGINT/SIGTERM, --timed + --terminate_after_timed, or --terminate_on_proc_exit.
// Returns the process exit code (0 on a clean stop).
int runCapture(const CaptureOptions &options);

} // namespace pml::cli
