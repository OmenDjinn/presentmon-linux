// SPDX-License-Identifier: MIT
#include "options.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace pml::cli {

const char *versionString() { return "presentmon-linux 0.1.0"; }

bool matchArg(const std::string &arg, const char *option)
{
	const char *p = arg.c_str();
	if (*p == '/')
		p += 1;
	else if (*p == '-')
	{
		p += 1;
		if (*p == '-')
			p += 1;
	}
	else
		return false;
	return strcasecmp(p, option) == 0;
}

bool parseUint(const std::string &value, uint32_t *out)
{
	if (value.empty() || !std::isdigit(static_cast<unsigned char>(value[0])))
		return false;
	errno = 0;
	char *end = nullptr;
	unsigned long v = std::strtoul(value.c_str(), &end, 10);
	if (errno != 0 || *end != '\0' || v > 0xFFFFFFFFul)
		return false;
	*out = uint32_t(v);
	return true;
}

// Upstream CanonicalizeProcessName: cut the extension, keep what follows the last separator,
// lower case. '\\' is a separator too so Windows paths coming from Wine work.
std::string canonicalizeProcessName(std::string name)
{
	size_t i = name.find_last_of("./\\");
	if (i != std::string::npos && name[i] == '.')
	{
		name.resize(i);
		i = name.find_last_of("/\\");
	}
	name = name.substr(i == std::string::npos ? 0 : i + 1);
	std::transform(name.begin(), name.end(), name.begin(),
	               [](unsigned char c) { return char(std::tolower(c)); });
	return name;
}

namespace {

// Upstream options that exist only because of ETW / Windows. They are recognised so the error says
// why, instead of "unrecognized option".
const char *const kWindowsOnly[] = {
	"etl_file", "hotkey", "scroll_indicator", "session_name", "stop_existing_session",
	"terminate_existing_session", "restart_as_admin", "set_circular_buffer_size",
	"track_hw_measurements", "disable_offline_backpressure", "track_etw_status",
	"include_mixed_reality", "capture_all",
};

} // namespace

ParseResult parseCaptureOptions(const std::vector<std::string> &args, CaptureOptions *out)
{
	ParseResult result;
	CaptureOptions o;
	bool csvOutputStdout = false;
	bool csvOutputNone = false;
	bool qpcTime = false, qpcmsTime = false, dtTime = false;
	bool outputFileSet = false;

	auto fail = [&](std::string msg) {
		result.ok = false;
		result.error = std::move(msg);
		return result;
	};

	for (size_t i = 0; i < args.size(); ++i)
	{
		const std::string &a = args[i];
		auto is = [&](const char *name) { return matchArg(a, name); };
		// Fetches the value of an option that takes one.
		auto value = [&](std::string *v) {
			if (i + 1 >= args.size())
				return false;
			*v = args[++i];
			return true;
		};
		auto uintValue = [&](uint32_t *v, std::string *err) {
			std::string s;
			if (!value(&s))
			{
				*err = "error: " + a + " expecting argument.";
				return false;
			}
			if (!parseUint(s, v))
			{
				*err = "error: " + a + " expecting a non-negative integer, got '" + s + "'.";
				return false;
			}
			return true;
		};
		std::string err, s;

		// Capture target options
		if (is("process_name"))
		{
			if (!value(&s)) return fail("error: " + a + " expecting argument.");
			o.targetProcessNames.push_back(canonicalizeProcessName(s));
		}
		else if (is("exclude"))
		{
			if (!value(&s)) return fail("error: " + a + " expecting argument.");
			o.excludeProcessNames.push_back(canonicalizeProcessName(s));
		}
		else if (is("process_id"))
		{
			if (!uintValue(&o.targetPid, &err)) return fail(err);
		}
		// Output options
		else if (is("output_file"))
		{
			if (!value(&o.outputCsvFileName)) return fail("error: " + a + " expecting argument.");
			outputFileSet = true;
		}
		else if (is("output_stdout")) csvOutputStdout = true;
		else if (is("multi_csv")) o.multiCsv = true;
		else if (is("no_csv")) csvOutputNone = true;
		else if (is("no_console_stats")) o.consoleStats = false;
		else if (is("qpc_time")) qpcTime = true;
		else if (is("qpc_time_ms")) qpcmsTime = true;
		else if (is("date_time")) dtTime = true;
		else if (is("exclude_dropped")) o.excludeDropped = true;
		else if (is("v1_metrics")) o.useV1Metrics = true;
		else if (is("v2_metrics")) o.useV2Metrics = true;
		else if (is("write_display_metadata")) o.writeDisplayMetadata = true;
		// Recording options
		else if (is("delay"))
		{
			if (!uintValue(&o.delay, &err)) return fail(err);
		}
		else if (is("timed"))
		{
			if (!uintValue(&o.timer, &err)) return fail(err);
			o.startTimer = true;
		}
		else if (is("no_track_gpu")) o.trackGPU = false;
		else if (is("track_gpu_video")) o.trackGPUVideo = true;
		else if (is("no_track_input")) o.trackInput = false;
		else if (is("no_track_display")) o.trackDisplay = false;
		// Execution options
		else if (is("terminate_on_proc_exit")) o.terminateOnProcExit = true;
		else if (is("terminate_after_timed")) o.terminateAfterTimer = true;
		// Beta options
		else if (is("track_frame_type")) o.trackFrameType = true;
		else if (is("track_app_timing")) o.trackAppTiming = true;
		else if (is("track_hybrid_present")) o.trackHybridPresent = true;
		else if (is("track_pc_latency")) o.trackPcLatency = true;
		// Hidden upstream options
		else if (is("write_frame_id")) o.writeFrameId = true;
		else if (is("write_display_time")) o.writeDisplayTime = true;
		// Hidden Linux options (tests)
		else if (is("segment"))
		{
			if (!value(&s)) return fail("error: " + a + " expecting argument.");
			o.segments.push_back(s);
		}
		else if (is("discovery_ms"))
		{
			if (!uintValue(&o.discoveryMs, &err)) return fail(err);
			if (o.discoveryMs == 0) o.discoveryMs = 1;
		}
		else if (is("stats_interval_ms"))
		{
			if (!uintValue(&o.statsIntervalMs, &err)) return fail(err);
			if (o.statsIntervalMs == 0) o.statsIntervalMs = 1;
		}
		else if (is("?") || is("h") || is("help"))
		{
			result.help = true;
			return result;
		}
		else
		{
			bool windowsOnly = false;
			for (const char *name : kWindowsOnly)
				windowsOnly = windowsOnly || is(name);
			if (windowsOnly)
				return fail("error: option '" + a + "' is Windows/ETW specific and not supported by presentmon on Linux.");
			return fail("error: unrecognized option '" + a + "'.");
		}
	}

	auto warn = [&](std::string w) { result.warnings.push_back(std::move(w)); };

	// At most one of --qpc_time --qpc_time_ms --date_time.
	if (int(qpcTime) + int(qpcmsTime) + int(dtTime) > 1)
	{
		std::string m = "error: only one of the following options may be used:";
		if (qpcTime) m += " --qpc_time";
		if (qpcmsTime) m += " --qpc_time_ms";
		if (dtTime) m += " --date_time";
		return fail(m);
	}

	// At most one of --output_file --output_stdout --no_csv (upstream warns and drops the extras).
	if (int(csvOutputNone) + int(csvOutputStdout) + int(outputFileSet) > 1)
	{
		std::string m = "warning: only one of the following options may be used:";
		if (csvOutputNone) m += " --no_csv";
		if (csvOutputStdout) m += " --output_stdout";
		if (outputFileSet) m += " --output_file";
		m += "\n         ignoring:";
		if (csvOutputNone) { csvOutputNone = false; m += " --no_csv"; }
		if (csvOutputStdout && outputFileSet) { csvOutputStdout = false; m += " --output_stdout"; }
		warn(m);
	}

	// CSV-only options make no sense with --no_csv.
	if (csvOutputNone && (qpcTime || qpcmsTime || dtTime || o.multiCsv || o.writeDisplayMetadata))
	{
		std::string m = "warning: ignoring CSV-related options due to --no_csv:";
		if (qpcTime) { qpcTime = false; m += " --qpc_time"; }
		if (qpcmsTime) { qpcmsTime = false; m += " --qpc_time_ms"; }
		if (dtTime) { dtTime = false; m += " --date_time"; }
		if (o.multiCsv) { o.multiCsv = false; m += " --multi_csv"; }
		if (o.writeDisplayMetadata) { o.writeDisplayMetadata = false; m += " --write_display_metadata"; }
		warn(m);
	}

	// stdout carries the CSV, so no console statistics (and --multi_csv only applies to files).
	if (csvOutputStdout)
	{
		o.consoleStats = false;
		if (o.multiCsv)
		{
			warn("warning: ignoring --multi_csv due to --output_stdout.");
			o.multiCsv = false;
		}
	}

	if (o.trackGPUVideo && !o.trackGPU)
	{
		warn("warning: ignoring --track_gpu_video due to --no_track_gpu.");
		o.trackGPUVideo = false;
	}
	if (!o.trackDisplay && o.trackGPU)
	{
		warn("warning: ignoring --no_track_display because display tracking is required when GPU tracking is enabled.");
		o.trackDisplay = true;
	}
	if (o.trackFrameType && o.useV1Metrics)
	{
		warn("warning: ignoring --track_frame_type due to --v1_metrics.");
		o.trackFrameType = false;
	}
	if (o.useV1Metrics && o.useV2Metrics)
	{
		warn("warning: ignoring --v1_metrics due to --v2_metrics.");
		o.useV1Metrics = false;
	}
	if (o.useV1Metrics && o.writeDisplayMetadata)
	{
		warn("warning: ignoring --write_display_metadata due to --v1_metrics.");
		o.writeDisplayMetadata = false;
	}

	o.timeUnit = qpcTime ? TimeUnit::QPC
	           : qpcmsTime ? TimeUnit::QPCMilliSeconds
	           : dtTime ? TimeUnit::DateTime
	                    : TimeUnit::MilliSeconds;
	o.csvOutput = csvOutputNone ? CsvOutput::None : csvOutputStdout ? CsvOutput::Stdout : CsvOutput::File;

	*out = std::move(o);
	return result;
}

void printUsage(FILE *out)
{
	std::fprintf(out, "%s\n", versionString());
	std::fputs(R"(
usage: presentmon [capture] [options]       capture frame metrics from layer-enabled processes (default)
       presentmon list [--process_name N] [--process_id P]
       presentmon overlay show|hide|toggle|status [--process_name N] [--process_id P]
                                                  [--position top-left|top-right|bottom-left|bottom-right]
                                                  [--scale F|auto]

Capture Target Options:
  --process_name name   Only record processes with the specified exe name. Can be repeated.
  --exclude name        Do not record processes with the specified exe name. Can be repeated.
  --process_id id       Only record the process with the specified process ID.

Output Options:
  --output_file path    Write CSV output to the specified path.
  --output_stdout       Write CSV output to STDOUT.
  --multi_csv           Create a separate CSV file for each captured process.
  --no_csv              Do not create any output CSV file.
  --no_console_stats    Do not display active swap chains and frame statistics in the console.
  --qpc_time            Output the CPU start time as a raw timestamp (CLOCK_MONOTONIC nanoseconds).
  --qpc_time_ms         Output the CPU start time as that timestamp converted to milliseconds.
  --date_time           Output the CPU start time as a date and time with nanosecond precision.
  --exclude_dropped     Exclude frames that were not displayed to the screen from the CSV output.
  --v1_metrics          Output a CSV using PresentMon 1.x metrics.
  --v2_metrics          Output a CSV using PresentMon 2.x metrics.
  --write_display_metadata  Write VidPnSourceId, LayerIndex, and PresentId columns to CSV output.

Recording Options:
  --delay seconds       Wait for the specified amount of time before starting to record.
  --timed seconds       Stop recording after the specified amount of time.
  --track_gpu_video     Add the video encode/decode GPU column (always 0 until GPU tracking exists).
  --no_track_display    Do not output display columns (only with --no_track_gpu).
  --no_track_input      Do not output input latency columns.
  --no_track_gpu        Do not output GPU columns.

Execution Options:
  --terminate_on_proc_exit  Terminate when all the target processes have exited.
  --terminate_after_timed   When using --timed, terminate after the timed capture completes.

Beta Options:
  --track_frame_type, --track_app_timing, --track_hybrid_present, --track_pc_latency

Ctrl+C, SIGTERM, SIGHUP or SIGQUIT stops the capture and closes the CSV cleanly (a signal
the parent left ignored stays ignored: under nohup, or SIGINT in a script's & job; use SIGTERM there).
)",
	           out);
}

} // namespace pml::cli
