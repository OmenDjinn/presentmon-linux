// SPDX-License-Identifier: MIT
// PresentMon-compatible CSV writer: a port of PresentMon/CsvOutput.cpp (Intel, MIT) for the three
// column sets (default = v2 metrics under the old names, --v1_metrics, --v2_metrics).
// Header text, column order, number formatting and NA rules are upstream's.
#pragma once

#include "options.hpp"

#include <pml/metrics/engine.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

namespace pml::cli {

// Time conversions for the CSV (upstream's PMTraceSession::Timestamp*): timestamps are
// CLOCK_MONOTONIC nanoseconds.
struct CsvClock
{
	uint64_t sessionStartNs = 0;
	int64_t wallOffsetNs = 0;   // CLOCK_REALTIME - CLOCK_MONOTONIC at session start (for --date_time)

	static CsvClock start();    // sessionStart = now

	// Milliseconds since the session start (negative before it). 0 for timestamp 0.
	double msSinceStart(uint64_t ts) const;
	// A tick count (nanoseconds) as milliseconds.
	static double deltaToMs(uint64_t ticks) { return double(ticks) * 1e-6; }
	// "%u-%u-%u %u:%02u:%02u.%09llu" in local time, like upstream (month/day are not zero padded).
	std::string dateTime(uint64_t ts) const;
};

// Application column values must not break the CSV.
std::string csvSafeName(std::string name);

const char *frameTypeToString(FrameType ft);

// Writes the header line (with its trailing newline) for the options' column set.
void writeCsvHeader(FILE *fp, const CaptureOptions &opts);

// Writes one row for `row`. `application` is the process name from the segment header.
void writeCsvRow(FILE *fp, const CaptureOptions &opts, const CsvClock &clock, const std::string &application,
                 const pml::metrics::Row &row);

// UTF-8 byte order mark. Upstream opens its files as "w,ccs=UTF-8", which writes it; stdout gets none.
inline constexpr unsigned char kUtf8Bom[3] = { 0xEF, 0xBB, 0xBF };

// Default file name: PresentMon-YYYY-MM-DDTHHMMSS.csv in the current directory (upstream's pattern),
// or --output_file's base name. With --multi_csv "-PROCESSNAME-PID" is added before the extension.
std::string generateCsvFilename(const CaptureOptions &opts, const std::string &processName, uint32_t processId,
                                time_t now);

} // namespace pml::cli
