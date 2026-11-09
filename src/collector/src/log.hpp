// SPDX-License-Identifier: MIT
// stderr logging for the CLI. Level from PRESENTMON_LOG=error|warn|info|debug (default info),
// every line is prefixed "[presentmon] " like the layer's.
#pragma once

namespace pml::cli {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

LogLevel logLevel();
void setLogLevel(LogLevel level);

__attribute__((format(printf, 2, 3))) void logf(LogLevel level, const char *fmt, ...);

} // namespace pml::cli

#define PML_LOG_ERROR(...) ::pml::cli::logf(::pml::cli::LogLevel::Error, __VA_ARGS__)
#define PML_LOG_WARN(...) ::pml::cli::logf(::pml::cli::LogLevel::Warn, __VA_ARGS__)
#define PML_LOG_INFO(...) ::pml::cli::logf(::pml::cli::LogLevel::Info, __VA_ARGS__)
#define PML_LOG_DEBUG(...) ::pml::cli::logf(::pml::cli::LogLevel::Debug, __VA_ARGS__)
