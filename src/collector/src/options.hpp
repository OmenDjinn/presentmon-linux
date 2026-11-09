// SPDX-License-Identifier: MIT
// Command line of the capture mode. Option names, spelling rules (/x, -x, --x, case-insensitive)
// and the cross-option fix-ups follow PresentMon/CommandLine.cpp (Intel, MIT).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pml::cli {

enum class TimeUnit
{
	MilliSeconds,     // default: relative to the start of the capture
	QPC,              // raw CLOCK_MONOTONIC nanoseconds (our "QPC")
	QPCMilliSeconds,  // --qpc_time_ms
	DateTime,         // --date_time
};

enum class CsvOutput { None, File, Stdout };

// Mirrors upstream's CommandLineArgs for the fields that make sense on Linux.
struct CaptureOptions
{
	// Capture target. Names are stored canonical (lower case, no directory, no extension).
	std::vector<std::string> targetProcessNames;
	std::vector<std::string> excludeProcessNames;
	uint32_t targetPid = 0;

	// Output
	std::string outputCsvFileName;   // empty = default name
	CsvOutput csvOutput = CsvOutput::File;
	TimeUnit timeUnit = TimeUnit::MilliSeconds;
	bool multiCsv = false;
	bool consoleStats = true;
	bool excludeDropped = false;
	bool useV1Metrics = false;
	bool useV2Metrics = false;
	bool writeDisplayMetadata = false;
	bool writeFrameId = false;       // hidden upstream option
	bool writeDisplayTime = false;   // hidden upstream option

	// Recording
	uint32_t delay = 0;              // seconds before recording starts
	uint32_t timer = 0;              // seconds of recording (when startTimer)
	bool startTimer = false;
	bool terminateAfterTimer = false;
	bool terminateOnProcExit = false;
	bool trackDisplay = true;
	bool trackInput = true;
	bool trackGPU = true;
	bool trackGPUVideo = false;
	bool trackFrameType = false;
	bool trackAppTiming = false;
	bool trackHybridPresent = false;
	bool trackPcLatency = false;

	// Linux-only, hidden (tests): restrict capture to these segment names, and set the cadence.
	std::vector<std::string> segments;
	uint32_t discoveryMs = 250;
	uint32_t statsIntervalMs = 1000;
};

struct ParseResult
{
	bool ok = true;
	bool help = false;                  // usage was requested (--help); not an error
	std::string error;                  // set when !ok
	std::vector<std::string> warnings;  // printed by the caller, like upstream's PrintWarning
};

// Parses the arguments after the program name (and after an optional "capture" word).
ParseResult parseCaptureOptions(const std::vector<std::string> &args, CaptureOptions *out);

// Lower case, without directory and extension. "Z:\\Games\\Foo.exe" -> "foo", "/usr/bin/vkcube" -> "vkcube".
std::string canonicalizeProcessName(std::string name);

// Shared by list/overlay: parses "--process_name N" / "--process_id P" style filters. Returns false
// and sets *error when `value` is not a number.
bool parseUint(const std::string &value, uint32_t *out);

// True when `arg` is `option` with a /, - or -- prefix, compared case-insensitively.
bool matchArg(const std::string &arg, const char *option);

void printUsage(FILE *out);
const char *versionString();

} // namespace pml::cli
