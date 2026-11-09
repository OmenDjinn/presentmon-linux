// SPDX-License-Identifier: MIT
#include "log.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace pml::layer {

namespace {

LogLevel levelFromEnv()
{
	const char *value = std::getenv("PRESENTMON_LOG");
	if (!value || !*value)
		return LogLevel::Warn;
	if (!strcasecmp(value, "error")) return LogLevel::Error;
	if (!strcasecmp(value, "warn") || !strcasecmp(value, "warning")) return LogLevel::Warn;
	if (!strcasecmp(value, "info")) return LogLevel::Info;
	if (!strcasecmp(value, "debug")) return LogLevel::Debug;
	if (!strcasecmp(value, "none") || !strcasecmp(value, "off")) return LogLevel::None;
	return LogLevel::Warn;
}

std::atomic<int> g_level{-1};

} // namespace

LogLevel logLevel()
{
	int level = g_level.load(std::memory_order_relaxed);
	if (level < 0)
	{
		level = int(levelFromEnv());
		g_level.store(level, std::memory_order_relaxed);
	}
	return LogLevel(level);
}

void setLogLevel(LogLevel level)
{
	g_level.store(int(level), std::memory_order_relaxed);
}

void logf(LogLevel level, const char *fmt, ...)
{
	if (int(logLevel()) < int(level))
		return;
	char buffer[512];
	int prefix = std::snprintf(buffer, sizeof(buffer), "[presentmon] ");
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buffer + prefix, sizeof(buffer) - size_t(prefix) - 2, fmt, args);
	va_end(args);
	size_t length = std::strlen(buffer);
	buffer[length++] = '\n';
	buffer[length] = 0;
	// One write() keeps lines from different threads from interleaving.
	std::fwrite(buffer, 1, length, stderr);
}

} // namespace pml::layer
