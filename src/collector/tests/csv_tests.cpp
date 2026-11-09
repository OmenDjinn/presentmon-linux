// SPDX-License-Identifier: MIT
// CSV writer and option parser tests; no processes involved.
//
// Headers are compared byte for byte with the first line of PresentMon's own golden CSVs
// (third_party/PresentMon/Tests/Gold). Those files were produced with optional columns turned on,
// so the options below switch the same columns on. Every column of every set is therefore present
// in our output; none is left out. Columns that are off by default upstream as well and enabled
// by a flag (all supported): FrameType (--track_frame_type), VideoBusy (--track_gpu_video),
// MsPCLatency (--track_pc_latency), InstrumentedLatency (--track_app_timing), QPC time units.
#include "csv.hpp"
#include "options.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace pml::cli;
using pml::metrics::Row;

namespace {

CaptureOptions parseOrDie(std::vector<std::string> args)
{
	CaptureOptions o;
	ParseResult r = parseCaptureOptions(args, &o);
	EXPECT_TRUE(r.ok) << r.error;
	return o;
}

// Runs a writer into a memory buffer.
template <class F>
std::string capture(F &&f)
{
	char *buf = nullptr;
	size_t len = 0;
	FILE *fp = open_memstream(&buf, &len);
	f(fp);
	std::fclose(fp);
	std::string s(buf, len);
	std::free(buf);
	return s;
}

std::string headerFor(std::vector<std::string> args)
{
	CaptureOptions o = parseOrDie(std::move(args));
	return capture([&](FILE *fp) { writeCsvHeader(fp, o); });
}

// First line of a gold file without the UTF-8 BOM and the line ending.
std::string goldHeader(const std::string &file)
{
	std::string text = pmltest::readFile(std::string(PML_GOLD_DIR) + "/" + file);
	EXPECT_GE(text.size(), 3u) << file;
	EXPECT_EQ(text.substr(0, 3), std::string("\xEF\xBB\xBF")) << file << " should start with a BOM";
	text = text.substr(3);
	size_t nl = text.find('\n');
	std::string line = text.substr(0, nl);
	if (!line.empty() && line.back() == '\r')
		line.pop_back();
	return line;
}

std::string goldRow(const std::string &file)
{
	auto lines = pmltest::splitLines(pmltest::readFile(std::string(PML_GOLD_DIR) + "/" + file));
	EXPECT_GE(lines.size(), 2u);
	return lines.size() >= 2 ? lines[1] : "";
}

// Options that make the three column sets match the gold files.
const std::vector<std::string> kDefaultGold = { "--qpc_time", "--track_frame_type", "--track_app_timing",
                                                "--track_pc_latency", "--track_gpu_video" };
const std::vector<std::string> kV1Gold = { "--v1_metrics", "--qpc_time", "--track_gpu_video" };
const std::vector<std::string> kV2Gold = { "--v2_metrics", "--qpc_time", "--track_gpu_video", "--track_frame_type",
                                           "--track_app_timing" };

} // namespace

TEST(CsvHeader, DefaultMatchesGoldByteForByte)
{
	for (int i = 0; i <= 5; ++i)
		EXPECT_EQ(headerFor(kDefaultGold), goldHeader("test_case_" + std::to_string(i) + ".csv") + "\n");
}

TEST(CsvHeader, V1MatchesGoldByteForByte)
{
	for (int i = 0; i <= 5; ++i)
		EXPECT_EQ(headerFor(kV1Gold), goldHeader("test_case_" + std::to_string(i) + "_v1.csv") + "\n");
}

TEST(CsvHeader, V2MatchesGoldByteForByte)
{
	for (int i = 0; i <= 5; ++i)
		EXPECT_EQ(headerFor(kV2Gold), goldHeader("test_case_" + std::to_string(i) + "_v2.csv") + "\n");
}

TEST(CsvHeader, PlainDefaultsUseMillisecondsUnit)
{
	// Upstream defaults: ms since start, display+GPU+input tracking on, the beta columns off.
	EXPECT_EQ(headerFor({}),
	          "Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,PresentFlags,AllowsTearing,"
	          "PresentMode,TimeInMs,MsBetweenSimulationStart,MsBetweenPresents,MsBetweenDisplayChange,"
	          "MsInPresentAPI,MsRenderPresentLatency,MsUntilDisplayed,CPUStartTimeInMs,MsBetweenAppStart,"
	          "MsCPUBusy,MsCPUWait,MsGPULatency,MsGPUTime,MsGPUBusy,MsGPUWait,MsAnimationError,AnimationTime,"
	          "MsFlipDelay,MsAllInputToPhotonLatency,MsClickToPhotonLatency\n");
	EXPECT_EQ(headerFor({ "--v2_metrics" }),
	          "Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,PresentFlags,AllowsTearing,"
	          "PresentMode,CPUStartTime,FrameTime,CPUBusy,CPUWait,GPULatency,GPUTime,GPUBusy,GPUWait,"
	          "DisplayLatency,DisplayedTime,AnimationError,AnimationTime,MsFlipDelay,AllInputToPhotonLatency,"
	          "ClickToPhotonLatency\n");
	EXPECT_EQ(headerFor({ "--v1_metrics" }),
	          "Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags,Dropped,TimeInSeconds,"
	          "msInPresentAPI,msBetweenPresents,AllowsTearing,PresentMode,msUntilRenderComplete,msUntilDisplayed,"
	          "msBetweenDisplayChange,msFlipDelay,msUntilRenderStart,msGPUActive,msSinceInput\n");
}

TEST(CsvHeader, OptionalColumnsFollowTheirFlags)
{
	auto h = headerFor({ "--v2_metrics", "--date_time", "--no_track_gpu", "--no_track_input", "--write_frame_id",
	                     "--write_display_time", "--write_display_metadata", "--track_hybrid_present" });
	EXPECT_EQ(h,
	          "Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,PresentFlags,VidPnSourceId,"
	          "LayerIndex,AllowsTearing,PresentMode,HybridPresent,CPUStartDateTime,FrameTime,CPUBusy,CPUWait,"
	          "DisplayLatency,DisplayedTime,AnimationError,AnimationTime,MsFlipDelay,DisplayTimeAbs,FrameId,"
	          "PresentId\n");
	EXPECT_EQ(headerFor({ "--qpc_time_ms", "--no_track_gpu", "--no_track_input" }).find("CPUStartQPCTimeInMs") !=
	              std::string::npos,
	          true);
}

// ---- Rows: the first data row of the gold files, rebuilt from its values ----------------------

namespace {

// The common metadata of the gold row "dwm.exe,1268,0x224B280A1C0,DXGI,1,0,..."
Row goldRowBase()
{
	Row row;
	row.frame.processId = 1268;
	row.frame.swapChainAddress = 0x224B280A1C0ull;
	row.frame.syncInterval = 1;
	row.frame.presentFlags = 0;
	row.frame.finalState = PresentResult::Presented;
	row.runtime = "DXGI";
	row.presentMode = "Hardware: Legacy Flip";
	row.syncInterval = 1;
	row.allowsTearing = false;
	row.metrics.frameType = FrameType::Application;
	row.metrics.cpuStartQpc = 2076511276ull;
	row.metrics.msCPUBusy = 16.3;
	row.metrics.msCPUWait = 0.0893;
	row.metrics.msGPULatency = 0.8529;
	row.metrics.msGPUBusy = 1.0752;
	row.metrics.msGPUWait = 14.4392;
	row.metrics.msVideoBusy = 0.0;
	row.metrics.msDisplayLatency = 32.7296;
	row.metrics.msDisplayedTime = 33.3323;
	row.metrics.msAnimationTime = 332.7030;   // msAnimationError, input, flip delay, ... stay "missing" (NA)
	row.metrics.msBetweenPresents = 16.4754;
	row.metrics.msBetweenDisplayChange = 16.6373;
	row.metrics.msInPresentApi = 0.0893;
	row.metrics.msUntilRenderComplete = 0.0673;
	row.metrics.msUntilDisplayed = 16.4296;
	row.metrics.timeInSeconds = 2076674276ull;
	row.metrics.msUntilRenderStart = -15.4471;
	row.metrics.msGpuDuration = 1.0752;
	return row;
}

std::string rowText(const CaptureOptions &o, const Row &row, uint64_t sessionStartNs)
{
	CsvClock clock;
	clock.sessionStartNs = sessionStartNs;
	return capture([&](FILE *fp) { writeCsvRow(fp, o, clock, "dwm.exe", row); });
}

} // namespace

TEST(CsvRow, DefaultColumnsReproduceGoldRow)
{
	CaptureOptions o = parseOrDie(kDefaultGold);
	Row row = goldRowBase();
	EXPECT_EQ(rowText(o, row, 0), goldRow("test_case_0.csv") + "\n");
}

TEST(CsvRow, V2ColumnsReproduceGoldRow)
{
	CaptureOptions o = parseOrDie(kV2Gold);
	EXPECT_EQ(rowText(o, goldRowBase(), 0), goldRow("test_case_0_v2.csv") + "\n");
}

TEST(CsvRow, V1ColumnsReproduceGoldRow)
{
	CaptureOptions o = parseOrDie(kV1Gold);
	Row row = goldRowBase();
	row.frame.presentStartTime = 2076674276ull;
	// TimeInSeconds 0.349003 = (presentStart - sessionStart) in seconds
	EXPECT_EQ(rowText(o, row, 2076674276ull - 349003000ull), goldRow("test_case_0_v1.csv") + "\n");
}

TEST(CsvRow, NaRules)
{
	CaptureOptions o = parseOrDie({ "--v2_metrics", "--qpc_time" });
	Row row = goldRowBase();
	// A row without display information: 0 display metrics print NA, GPU stays numeric.
	row.metrics.msDisplayedTime = 0;
	row.metrics.msDisplayLatency = 0;
	row.metrics.msAnimationTime = pml::metrics::MissingFrameMetricValue();
	std::string text = rowText(o, row, 0);
	EXPECT_NE(text.find(",NA,NA,NA,NA,NA,NA,NA\n"), std::string::npos) << text;
	// AnimationTime for a displayed app frame without a value prints 0.0000, not NA.
	row.metrics.msDisplayedTime = 5;
	row.metrics.msDisplayLatency = 12;
	text = rowText(o, row, 0);
	EXPECT_NE(text.find(",12.0000,5.0000,NA,0.0000,NA,NA,NA\n"), std::string::npos) << text;
}

// Every row has exactly as many fields as the header, for every combination of the flags that
// add or remove columns (--track_pc_latency used to add a column without --track_display).
TEST(CsvRow, FieldCountMatchesHeaderForEveryFlagCombination)
{
	auto fields = [](const std::string &text) { return std::count(text.begin(), text.end(), ',') + 1; };
	// "" is the default layout (neither flag), where MsPCLatency once landed outside the display block.
	for (const char *version : { "--v1_metrics", "--v2_metrics", "" })
		for (int mask = 0; mask < 64; ++mask)
		{
			std::vector<std::string> args;
			if (*version)
				args.push_back(version);
			if (mask & 1) args.push_back("--no_track_gpu");
			if (mask & 2) args.push_back("--no_track_display");
			if (mask & 4) args.push_back("--track_pc_latency");
			if (mask & 8) args.push_back("--track_gpu_video");
			if (mask & 16) args.push_back("--track_frame_type");
			if (mask & 32) args.push_back("--no_track_input");
			CaptureOptions o = parseOrDie(args);
			std::string header = headerFor(args);
			std::string row = rowText(o, goldRowBase(), 0);
			EXPECT_EQ(fields(header.substr(0, header.size() - 1)), fields(row.substr(0, row.size() - 1)))
				<< version << " mask " << mask << "\n" << header << row;
		}
}

TEST(CsvRow, DateTimeUsesUpstreamFormat)
{
	CsvClock clock;
	clock.sessionStartNs = 1;
	clock.wallOffsetNs = 1700000000ll * 1000000000ll + 123456789;   // 2023-11-14 22:13:20 UTC
	std::string s = clock.dateTime(1);
	// month/day are not zero padded, minutes/seconds are, the fraction has nine digits
	EXPECT_NE(s.find(".123456790"), std::string::npos) << s;   // +1 ns from ts
	EXPECT_EQ(s.substr(0, 5), "2023-");
}

TEST(CsvNames, FilenameGeneration)
{
	CaptureOptions o;
	const time_t t = 1700000000;
	std::string def = generateCsvFilename(o, "game.exe", 42, t);
	EXPECT_EQ(def.rfind("PresentMon-", 0), 0u) << def;
	EXPECT_EQ(def.substr(def.size() - 4), ".csv");
	EXPECT_EQ(def.size(), std::string("PresentMon-2023-11-14T221320.csv").size());

	o.outputCsvFileName = "dir/out.csv";
	EXPECT_EQ(generateCsvFilename(o, "game.exe", 42, t), "dir/out.csv");
	o.multiCsv = true;
	EXPECT_EQ(generateCsvFilename(o, "game.exe", 42, t), "dir/out-game.exe-42.csv");
	EXPECT_EQ(generateCsvFilename(o, "<unknown>", 42, t), "dir/out-42.csv");
	o.outputCsvFileName = "noext";
	EXPECT_EQ(generateCsvFilename(o, "g", 7, t), "noext-g-7");
	EXPECT_EQ(csvSafeName("a,b\nc"), "a_b_c");
}

// ---- Options ---------------------------------------------------------------------------------

TEST(Options, PrefixesAndCase)
{
	CaptureOptions o;
	auto r = parseCaptureOptions({ "-PROCESS_NAME", "Foo.exe", "/process_name", "bar", "--process_id", "12", "-Timed", "5" }, &o);
	ASSERT_TRUE(r.ok) << r.error;
	EXPECT_EQ(o.targetProcessNames, (std::vector<std::string>{ "foo", "bar" }));
	EXPECT_EQ(o.targetPid, 12u);
	EXPECT_TRUE(o.startTimer);
	EXPECT_EQ(o.timer, 5u);
}

TEST(Options, CanonicalNames)
{
	EXPECT_EQ(canonicalizeProcessName("Z:\\Games\\Foo.EXE"), "foo");
	EXPECT_EQ(canonicalizeProcessName("/usr/bin/vkcube"), "vkcube");
	EXPECT_EQ(canonicalizeProcessName("Game.x86_64"), "game");
	EXPECT_EQ(canonicalizeProcessName("plain"), "plain");
}

TEST(Options, UnknownAndUnsupportedFlagsGiveClearErrors)
{
	CaptureOptions o;
	auto r = parseCaptureOptions({ "--frobnicate" }, &o);
	EXPECT_FALSE(r.ok);
	EXPECT_NE(r.error.find("unrecognized option '--frobnicate'"), std::string::npos) << r.error;

	for (const char *flag : { "--etl_file", "--hotkey", "--session_name", "--stop_existing_session",
	                          "--terminate_existing_session", "--restart_as_admin", "--set_circular_buffer_size",
	                          "--track_hw_measurements", "--scroll_indicator", "--track_etw_status" })
	{
		r = parseCaptureOptions({ flag, "x" }, &o);
		EXPECT_FALSE(r.ok) << flag;
		EXPECT_NE(r.error.find("not supported"), std::string::npos) << flag << ": " << r.error;
	}

	r = parseCaptureOptions({ "--process_id" }, &o);
	EXPECT_FALSE(r.ok);
	EXPECT_NE(r.error.find("expecting argument"), std::string::npos);
	r = parseCaptureOptions({ "--timed", "soon" }, &o);
	EXPECT_FALSE(r.ok);
	r = parseCaptureOptions({ "--qpc_time", "--date_time" }, &o);
	EXPECT_FALSE(r.ok);
	EXPECT_NE(r.error.find("only one of"), std::string::npos);
}

TEST(Options, UpstreamFixups)
{
	CaptureOptions o;
	auto r = parseCaptureOptions({ "--output_stdout", "--multi_csv", "--v1_metrics", "--track_frame_type",
	                               "--no_track_gpu", "--track_gpu_video", "--no_track_display" }, &o);
	ASSERT_TRUE(r.ok) << r.error;
	EXPECT_FALSE(o.consoleStats);            // stdout carries the CSV
	EXPECT_FALSE(o.multiCsv);
	EXPECT_FALSE(o.trackFrameType);          // not available in v1
	EXPECT_FALSE(o.trackGPUVideo);           // needs GPU tracking
	EXPECT_FALSE(o.trackDisplay);            // allowed once GPU tracking is off
	EXPECT_EQ(o.csvOutput, CsvOutput::Stdout);
	EXPECT_GE(r.warnings.size(), 3u);

	r = parseCaptureOptions({ "--no_track_display" }, &o);   // GPU tracking is on by default
	EXPECT_TRUE(o.trackDisplay);
	ASSERT_EQ(r.warnings.size(), 1u);

	r = parseCaptureOptions({ "--no_csv", "--output_file", "x.csv", "--qpc_time" }, &o);
	ASSERT_TRUE(r.ok);
	EXPECT_EQ(o.csvOutput, CsvOutput::File);   // upstream drops --no_csv when more than one is given
	r = parseCaptureOptions({ "--no_csv", "--qpc_time" }, &o);
	EXPECT_EQ(o.csvOutput, CsvOutput::None);
	EXPECT_EQ(o.timeUnit, TimeUnit::MilliSeconds);   // CSV-only option ignored
	EXPECT_FALSE(r.warnings.empty());

	r = parseCaptureOptions({ "--v1_metrics", "--v2_metrics" }, &o);
	EXPECT_TRUE(o.useV2Metrics);
	EXPECT_FALSE(o.useV1Metrics);
}
