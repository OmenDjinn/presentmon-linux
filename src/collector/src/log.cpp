// SPDX-License-Identifier: MIT
#include "log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace pml::cli {

namespace {
LogLevel parseEnv()
{
	const char *v = std::getenv("PRESENTMON_LOG");
	if (!v)
		return LogLevel::Info;
	if (!strcasecmp(v, "error")) return LogLevel::Error;
	if (!strcasecmp(v, "warn")) return LogLevel::Warn;
	if (!strcasecmp(v, "debug")) return LogLevel::Debug;
	return LogLevel::Info;
}
LogLevel gLevel = parseEnv();
} // namespace

LogLevel logLevel() { return gLevel; }
void setLogLevel(LogLevel level) { gLevel = level; }

void logf(LogLevel level, const char *fmt, ...)
{
	if (int(level) > int(gLevel))
		return;
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	std::fprintf(stderr, "[presentmon] %s\n", buf);
	std::fflush(stderr);
}

} // namespace pml::cli
