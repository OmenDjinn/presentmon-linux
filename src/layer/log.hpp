// SPDX-License-Identifier: MIT
// Layer logging: PRESENTMON_LOG=error|warn|info|debug (default warn), written to stderr with the
// prefix "[presentmon] ". Never allocates on the happy path when the level is filtered out.
#pragma once

#include <cstdarg>

namespace pml::layer {

enum class LogLevel : int
{
	None = 0,
	Error = 1,
	Warn = 2,
	Info = 3,
	Debug = 4,
};

LogLevel logLevel();
void setLogLevel(LogLevel level);  // tests
void logf(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Logs the message the first time this call site is reached (per process).
#define PML_LOG_ONCE(level, ...) \
	do { static bool pmlLogged_ = false; if (!pmlLogged_) { pmlLogged_ = true; ::pml::layer::logf(level, __VA_ARGS__); } } while (0)

#define PML_LOG(level, ...) \
	do { if (int(::pml::layer::logLevel()) >= int(level)) ::pml::layer::logf(level, __VA_ARGS__); } while (0)

} // namespace pml::layer
