// SPDX-License-Identifier: MIT
// End-to-end tests of the presentmon executable against a fake producer (a real shared-memory
// segment with a deterministic timeline, see fake_timeline.hpp). No GPU, no Vulkan.
//
// Every test uses its own process name so concurrent runs and real layer segments cannot interfere:
// the CLI is always started with --process_name for it. The CLI is started first and the producer
// afterwards; a segment created after the capture started is read from its first record.
#include "fake_timeline.hpp"
#include "test_support.hpp"

#include <pml/clock.hpp>
#include <pml/shm.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>

using namespace pmltest;

namespace {

const std::string kCli = PML_PRESENTMON_EXE;
const std::string kProducer = PML_FAKE_PRODUCER_EXE;

class CliTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
		tag_ = std::string(info->name());
		// Unique per test and per run: pid of the test process + test name.
		procName_ = "pmltest_" + tag_ + "_" + std::to_string(::getpid());
		dir_ = std::string(PML_TEST_TMP) + "/" + procName_;
		std::filesystem::create_directories(dir_);
	}

	void TearDown() override
	{
		if (!::testing::Test::HasFailure())
			std::filesystem::remove_all(dir_);
		else
			ADD_FAILURE_AT(__FILE__, __LINE__) << "kept " << dir_ << " for inspection";
	}

	// Starts presentmon with the common test options and waits until it is capturing.
	void startCli(std::vector<std::string> args, bool filterByName = true, const char *logLevel = "info")
	{
		std::vector<std::string> full;
		if (filterByName)
		{
			full.push_back("--process_name");
			full.push_back(procName_);
		}
		full.insert(full.end(), args.begin(), args.end());
		ASSERT_TRUE(spawn(cli_, kCli, full, dir_, "cli", dir_, { std::string("PRESENTMON_LOG=") + logLevel }));
		ASSERT_TRUE(waitForText(cli_.errPath, "capture started", 10000)) << cli_.err();
	}

	// Starts the fake producer (after the CLI) and waits until its frames are in the ring.
	void startProducer(std::vector<std::string> args = {}, const std::string &tag = "producer")
	{
		std::vector<std::string> full = { "--process-name", procName_ + ".exe" };
		full.insert(full.end(), args.begin(), args.end());
		Proc &p = producers_.emplace_back();
		ASSERT_TRUE(spawn(p, kProducer, full, dir_, tag + std::to_string(producers_.size())));
		ASSERT_TRUE(waitForText(p.outPath, "published", 10000)) << p.err();
	}

	pid_t producerPid(size_t i = 0) { return producers_.at(i).pid; }

	// All parsed CSV rows, keyed by column name.
	struct Csv
	{
		std::vector<std::string> header;
		std::vector<std::vector<std::string>> rows;
		std::string bom;
		double num(size_t row, const std::string &col) const
		{
			auto it = std::find(header.begin(), header.end(), col);
			EXPECT_TRUE(it != header.end()) << "no column " << col;
			return std::stod(rows.at(row).at(size_t(it - header.begin())));
		}
		const std::string &str(size_t row, const std::string &col) const
		{
			auto it = std::find(header.begin(), header.end(), col);
			EXPECT_TRUE(it != header.end()) << "no column " << col;
			return rows.at(row).at(size_t(it - header.begin()));
		}
	};

	static Csv parseCsv(const std::string &text)
	{
		Csv csv;
		std::string body = text;
		if (body.size() >= 3 && body.compare(0, 3, "\xEF\xBB\xBF") == 0)
		{
			csv.bom = body.substr(0, 3);
			body = body.substr(3);
		}
		auto lines = splitLines(body);
		if (lines.empty())
			return csv;
		csv.header = splitCsv(lines[0]);
		for (size_t i = 1; i < lines.size(); ++i)
		{
			auto cols = splitCsv(lines[i]);
			EXPECT_EQ(cols.size(), csv.header.size()) << "line " << i << ": " << lines[i];
			csv.rows.push_back(std::move(cols));
		}
		return csv;
	}

	std::string tag_, procName_, dir_;
	Proc cli_;
	std::vector<Proc> producers_;
};

} // namespace

// V2 columns: values computed by hand from fake_timeline.hpp (P = 5 ms, present call 0.2 ms,
// ready +1 ms, screen +8 ms).
TEST_F(CliTest, V2CsvValuesMatchHandComputation)
{
	startCli({ "--v2_metrics", "--qpc_time", "--output_file", "out.csv", "--no_console_stats",
	           "--terminate_on_proc_exit", "--timed", "30", "--terminate_after_timed" });
	startProducer({ "--frames", "100", "--linger-ms", "300" });
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();

	Csv csv = parseCsv(readFile(dir_ + "/out.csv"));
	EXPECT_EQ(csv.bom, "\xEF\xBB\xBF");
	EXPECT_EQ(csv.header.size(), 23u);
	// Frame 1 only seeds the state; frame k is released when k+1 is displayed, so the 100 presents
	// give 98 rows (PresentMon never writes the held tail either).
	ASSERT_EQ(csv.rows.size(), 98u);

	const std::string pid = std::to_string(producerPid());
	double prevStart = 0;
	for (size_t i = 0; i < csv.rows.size(); ++i)
	{
		SCOPED_TRACE("row " + std::to_string(i));
		EXPECT_EQ(csv.str(i, "Application"), procName_ + ".exe");
		EXPECT_EQ(csv.str(i, "ProcessID"), pid);
		EXPECT_EQ(csv.str(i, "SwapChainAddress"), "0xABC0DE12");
		EXPECT_EQ(csv.str(i, "PresentRuntime"), "DXVK");
		EXPECT_EQ(csv.str(i, "SyncInterval"), "1");
		EXPECT_EQ(csv.str(i, "PresentFlags"), "0");
		EXPECT_EQ(csv.str(i, "AllowsTearing"), "0");
		EXPECT_EQ(csv.str(i, "PresentMode"), "FIFO");
		EXPECT_NEAR(csv.num(i, "FrameTime"), 5.0, 1e-3);
		EXPECT_NEAR(csv.num(i, "CPUBusy"), 4.8, 1e-3);    // 5 ms period - 0.2 ms inside the previous present
		EXPECT_NEAR(csv.num(i, "CPUWait"), 0.2, 1e-3);
		EXPECT_NEAR(csv.num(i, "DisplayLatency"), 12.8, 1e-3);   // screen(i) - cpuStart(i) = 8 + 5 - 0.2
		EXPECT_NEAR(csv.num(i, "DisplayedTime"), 5.0, 1e-3);
		// GPU is not tracked yet: upstream prints the zero-initialised numbers.
		EXPECT_EQ(csv.str(i, "GPULatency"), "0.0000");
		EXPECT_EQ(csv.str(i, "GPUTime"), "0.0000");
		EXPECT_EQ(csv.str(i, "GPUBusy"), "0.0000");
		EXPECT_EQ(csv.str(i, "GPUWait"), "0.0000");
		// No instrumentation, no input tracking: NA.
		EXPECT_EQ(csv.str(i, "MsFlipDelay"), "NA");
		EXPECT_EQ(csv.str(i, "AllInputToPhotonLatency"), "NA");
		EXPECT_EQ(csv.str(i, "ClickToPhotonLatency"), "NA");
		// CPUStartQPC is the previous present's end: consecutive rows are exactly one period apart.
		double start = csv.num(i, "CPUStartQPC");
		if (i > 0) {
			EXPECT_DOUBLE_EQ(start - prevStart, 5e6);
		}
		prevStart = start;
	}
	EXPECT_NE(cli_.err().find("98 rows written"), std::string::npos) << cli_.err();
}

TEST_F(CliTest, V1CsvHasOneRowPerPresent)
{
	startCli({ "--v1_metrics", "--qpc_time", "--output_file", "v1.csv", "--no_console_stats",
	           "--terminate_on_proc_exit", "--timed", "30", "--terminate_after_timed" });
	startProducer({ "--frames", "60", "--linger-ms", "300" });
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();

	Csv csv = parseCsv(readFile(dir_ + "/v1.csv"));
	EXPECT_EQ(csv.bom, "\xEF\xBB\xBF");
	ASSERT_EQ(csv.rows.size(), 59u);   // every present but the seed
	for (size_t i = 0; i < csv.rows.size(); ++i)
	{
		SCOPED_TRACE("row " + std::to_string(i));
		EXPECT_EQ(csv.str(i, "Application"), procName_ + ".exe");
		EXPECT_EQ(csv.str(i, "SwapChainAddress"), "0x00000000ABC0DE12");   // v1 pads to 16 digits
		EXPECT_EQ(csv.str(i, "Runtime"), "DXVK");
		EXPECT_EQ(csv.str(i, "Dropped"), "0");
		EXPECT_EQ(csv.str(i, "PresentMode"), "FIFO");
		EXPECT_NEAR(csv.num(i, "msBetweenPresents"), 5.0, 1e-9);
		EXPECT_NEAR(csv.num(i, "msInPresentAPI"), 0.2, 1e-9);
		EXPECT_NEAR(csv.num(i, "msUntilRenderComplete"), 1.0, 1e-9);
		EXPECT_NEAR(csv.num(i, "msUntilDisplayed"), 8.0, 1e-9);
		EXPECT_NEAR(csv.num(i, "msBetweenDisplayChange"), 5.0, 1e-9);
		EXPECT_EQ(csv.str(i, "msFlipDelay"), "0.00000000000000");
		if (i > 0) {
			EXPECT_DOUBLE_EQ(csv.num(i, "QPCTime") - csv.num(i - 1, "QPCTime"), 5e6);
		}
	}
}

TEST_F(CliTest, DefaultColumnSetAndNaForMissingDisplayData)
{
	startCli({ "--output_file", "plain.csv", "--no_console_stats", "--terminate_on_proc_exit", "--timed", "30",
	           "--terminate_after_timed", "--qpc_time" });
	// Xwayland-like: no display information at all.
	startProducer({ "--frames", "40", "--mode", "nodisplay", "--linger-ms", "300" });
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();

	Csv csv = parseCsv(readFile(dir_ + "/plain.csv"));
	ASSERT_EQ(csv.rows.size(), 39u);   // not displayed or dropped: released one per present
	for (size_t i = 1; i < csv.rows.size(); ++i)
	{
		SCOPED_TRACE("row " + std::to_string(i));
		EXPECT_EQ(csv.str(i, "MsUntilDisplayed"), "NA");
		EXPECT_EQ(csv.str(i, "MsBetweenDisplayChange"), "NA");
		EXPECT_EQ(csv.str(i, "MsBetweenSimulationStart"), "NA");
		EXPECT_EQ(csv.str(i, "MsFlipDelay"), "NA");
		EXPECT_EQ(csv.str(i, "AnimationTime"), "NA");
		// CPU columns are still real
		EXPECT_NEAR(csv.num(i, "MsBetweenPresents"), 5.0, 1e-9);
		EXPECT_NEAR(csv.num(i, "MsCPUBusy"), 4.8, 1e-3);
		EXPECT_NEAR(csv.num(i, "MsCPUWait"), 0.2, 1e-3);
		EXPECT_NEAR(csv.num(i, "MsBetweenAppStart"), 5.0, 1e-3);
		EXPECT_NEAR(csv.num(i, "MsRenderPresentLatency"), 0.0, 1e-3);   // no ready time without timing
	}
}

TEST_F(CliTest, DroppedFramesInV1AndExcludeDropped)
{
	// frames 3, 6, ... are replaced; V1 reports them with Dropped = 1.
	startCli({ "--v1_metrics", "--output_file", "d.csv", "--no_console_stats", "--terminate_on_proc_exit",
	           "--timed", "30", "--terminate_after_timed" });
	startProducer({ "--frames", "100", "--mode", "drop3", "--linger-ms", "300" });
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();
	Csv csv = parseCsv(readFile(dir_ + "/d.csv"));
	ASSERT_EQ(csv.rows.size(), 99u);
	size_t dropped = 0;
	for (size_t i = 0; i < csv.rows.size(); ++i)
		dropped += csv.str(i, "Dropped") == "1";
	EXPECT_EQ(dropped, 33u);   // multiples of 3 within 2..100

	// A second capture of the same kind with --exclude_dropped
	producers_.clear();
	Proc second;
	std::swap(cli_, second);
	startCli({ "--v1_metrics", "--exclude_dropped", "--output_file", "e.csv", "--no_console_stats",
	           "--terminate_on_proc_exit", "--timed", "30", "--terminate_after_timed" });
	startProducer({ "--frames", "100", "--mode", "drop3", "--linger-ms", "300" });
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();
	csv = parseCsv(readFile(dir_ + "/e.csv"));
	EXPECT_EQ(csv.rows.size(), 66u);
	for (size_t i = 0; i < csv.rows.size(); ++i)
		EXPECT_EQ(csv.str(i, "Dropped"), "0");
}

TEST_F(CliTest, DefaultFileNameBomAndMultiCsv)
{
	startCli({ "--no_console_stats", "--terminate_on_proc_exit", "--timed", "30", "--terminate_after_timed" });
	startProducer({ "--frames", "20", "--linger-ms", "300" });
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();
	std::string found;
	for (auto &e : std::filesystem::directory_iterator(dir_))
	{
		std::string n = e.path().filename().string();
		if (n.rfind("PresentMon-", 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".csv")
			found = n;
	}
	ASSERT_FALSE(found.empty()) << "no PresentMon-<time>.csv in " << dir_;
	EXPECT_EQ(found.size(), std::string("PresentMon-2025-01-01T000000.csv").size());
	EXPECT_EQ(readFile(dir_ + "/" + found).substr(0, 3), "\xEF\xBB\xBF");
	std::filesystem::remove(dir_ + "/" + found);

	producers_.clear();
	Proc old;
	std::swap(cli_, old);
	startCli({ "--no_console_stats", "--multi_csv", "--output_file", "m.csv", "--terminate_on_proc_exit",
	           "--timed", "30", "--terminate_after_timed" });
	startProducer({ "--frames", "20", "--linger-ms", "300" });
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();
	std::string expected = dir_ + "/m-" + procName_ + ".exe-" + std::to_string(producerPid()) + ".csv";
	EXPECT_TRUE(std::filesystem::exists(expected)) << expected;
}

TEST_F(CliTest, StdoutCsvHasNoBomAndNoConsoleStats)
{
	startCli({ "--output_stdout", "--terminate_on_proc_exit", "--timed", "30", "--terminate_after_timed" });
	startProducer({ "--frames", "30", "--linger-ms", "300" });
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();
	std::string out = cli_.out();
	ASSERT_FALSE(out.empty());
	EXPECT_EQ(out.rfind("Application,ProcessID,", 0), 0u) << out.substr(0, 40);   // no BOM on stdout
	Csv csv = parseCsv(out);
	EXPECT_EQ(csv.rows.size(), 28u);   // 30 presents - seed - held tail
}

TEST_F(CliTest, NoCsvWritesNothingButStillShowsConsoleStats)
{
	startCli({ "--no_csv", "--stats_interval_ms", "200", "--terminate_on_proc_exit", "--timed", "30",
	           "--terminate_after_timed" });
	startProducer({ "--frames", "100", "--linger-ms", "1200" });
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();
	for (auto &e : std::filesystem::directory_iterator(dir_))
		EXPECT_NE(e.path().extension(), ".csv") << e.path();
	EXPECT_NE(cli_.out().find("0x00000000ABC0DE12"), std::string::npos) << cli_.out();
}

TEST_F(CliTest, ConsoleStatsLine)
{
	startCli({ "--no_csv", "--stats_interval_ms", "200", "--terminate_on_proc_exit", "--timed", "30",
	           "--terminate_after_timed" });
	startProducer({ "--frames", "100", "--mode", "drop3", "--linger-ms", "1200" });
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();
	std::string out = cli_.out();
	// Group line, then one line per swapchain
	EXPECT_NE(out.find(procName_ + ".exe[" + std::to_string(producerPid()) + "]:"), std::string::npos) << out;
	std::string line;
	for (auto &l : splitLines(out))
		if (l.find("0x00000000ABC0DE12 (DXVK) FIFO:") != std::string::npos && l.find("Presented 0.0 fps") == std::string::npos)
			line = l;
	ASSERT_FALSE(line.empty()) << out;
	for (const char *field : { "Displayed ", " fps", "Presented ", "Frame 5.000 ms", "CPU busy 4.800 ms", "Display latency 12.800 ms",
	                           "Dropped " })
		EXPECT_NE(line.find(field), std::string::npos) << field << " in: " << line;
	EXPECT_EQ(line.find("Dropped 0.0%"), std::string::npos) << "a third of the frames is dropped: " << line;
}

TEST_F(CliTest, NoConsoleStatsOption)
{
	startCli({ "--no_csv", "--no_console_stats", "--stats_interval_ms", "100", "--terminate_on_proc_exit",
	           "--timed", "30", "--terminate_after_timed" });
	startProducer({ "--frames", "50", "--linger-ms", "600" });
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();
	EXPECT_TRUE(cli_.out().empty()) << cli_.out();
}

TEST_F(CliTest, LostRecordsAreReported)
{
	startCli({ "--no_csv", "--stats_interval_ms", "200", "--terminate_on_proc_exit", "--timed", "30",
	           "--terminate_after_timed", "--discovery_ms", "50" });
	// Segment exists with a 64-slot ring for 400 ms (the collector attaches), then 300 frames land at once.
	startProducer({ "--frames", "300", "--ring", "64", "--start-delay-ms", "400", "--linger-ms", "1000" });
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();
	EXPECT_NE(cli_.out().find("lost "), std::string::npos) << cli_.out();
	EXPECT_NE(cli_.out().find(" records)"), std::string::npos) << cli_.out();
}

TEST_F(CliTest, FiltersExcludeAndProcessId)
{
	// Excluded by name: nothing is captured.
	startCli({ "--exclude", procName_, "--no_csv", "--stats_interval_ms", "200", "--terminate_after_timed",
	           "--timed", "1" }, /*filterByName=*/false);
	startProducer({ "--frames", "30", "--linger-ms", "600" });
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();
	EXPECT_EQ(cli_.out().find("ABC0DE12"), std::string::npos) << cli_.out();
	EXPECT_EQ(cli_.err().find("capturing " + procName_), std::string::npos) << cli_.err();

	// --process_id of a process that is not there: nothing either; of the right one: captured.
	producers_.clear();
	Proc old;
	std::swap(cli_, old);
	startCli({ "--process_id", "4000000000", "--process_name", "nosuchgame", "--no_csv", "--terminate_after_timed", "--timed", "1" },
	         false);
	startProducer({ "--frames", "30", "--linger-ms", "600" });
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();
	EXPECT_EQ(cli_.err().find("capturing " + procName_), std::string::npos) << cli_.err();
}

TEST_F(CliTest, TimedExitsAfterTheTimer)
{
	startCli({ "--no_csv", "--timed", "1", "--terminate_after_timed" });
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	bool timedOut = false;
	int code = cli_.wait(10000, &timedOut);
	clock_gettime(CLOCK_MONOTONIC, &b);
	EXPECT_FALSE(timedOut);
	EXPECT_EQ(code, 0) << cli_.err();
	double secs = double(b.tv_sec - a.tv_sec) + double(b.tv_nsec - a.tv_nsec) * 1e-9;
	EXPECT_LT(secs, 3.0);
	EXPECT_NE(cli_.err().find("timed capture finished"), std::string::npos) << cli_.err();
}

TEST_F(CliTest, TimedWithoutTerminateKeepsRunningUntilSigint)
{
	startCli({ "--no_csv", "--timed", "1" });
	sleepMs(2000);
	ASSERT_TRUE(cli_.running()) << "must keep running without --terminate_after_timed";
	cli_.signal(SIGINT);
	bool timedOut = false;
	EXPECT_EQ(cli_.wait(10000, &timedOut), 0) << cli_.err();
	EXPECT_FALSE(timedOut);
}

TEST_F(CliTest, SigintFlushesAndClosesTheCsv)
{
	startCli({ "--output_file", "int.csv", "--no_console_stats" });
	startProducer({ "--frames", "200", "--linger-ms", "20000" });
	sleepMs(500);
	cli_.signal(SIGINT);
	bool timedOut = false;
	EXPECT_EQ(cli_.wait(10000, &timedOut), 0) << cli_.err();
	EXPECT_FALSE(timedOut);
	std::string text = readFile(dir_ + "/int.csv");
	ASSERT_FALSE(text.empty());
	EXPECT_EQ(text.back(), '\n') << "the last row must be complete";
	EXPECT_EQ(parseCsv(text).rows.size(), 198u);
	EXPECT_NE(cli_.err().find("stopping"), std::string::npos);

	// SIGTERM behaves the same
	producers_.clear();
	Proc old;
	std::swap(cli_, old);
	startCli({ "--output_file", "term.csv", "--no_console_stats" });
	startProducer({ "--frames", "50", "--linger-ms", "20000" });
	sleepMs(300);
	cli_.signal(SIGTERM);
	EXPECT_EQ(cli_.wait(10000, &timedOut), 0);
	EXPECT_EQ(parseCsv(readFile(dir_ + "/term.csv")).rows.size(), 48u);
}

TEST_F(CliTest, TerminateOnProcessExitAlsoWhenTheProcessIsKilled)
{
	startCli({ "--no_csv", "--terminate_on_proc_exit" });
	startProducer({ "--frames", "10", "--linger-ms", "300", "--crash" });
	bool timedOut = false;
	EXPECT_EQ(cli_.wait(10000, &timedOut), 0) << cli_.err();
	EXPECT_FALSE(timedOut) << "terminate_on_proc_exit must notice a SIGKILLed producer";
	EXPECT_NE(cli_.err().find("all target processes have exited"), std::string::npos) << cli_.err();
	// The producer left its segment behind; it becomes stale after the 10 s heartbeat window.
	::unlink(("/dev/shm/" + pml::segmentNameForPid(producerPid())).c_str());
}

TEST_F(CliTest, DelayAndTimedSelectTheRecordingWindow)
{
	// Frames published at ~0 s are inside the delay and not recorded; the second burst at ~1.4 s is.
	startCli({ "--output_file", "w.csv", "--no_console_stats", "--delay", "1", "--timed", "2",
	           "--terminate_after_timed" });
	startProducer({ "--frames", "30", "--linger-ms", "300" }, "early");
	startProducer({ "--frames", "50", "--start-delay-ms", "1000", "--linger-ms", "300" }, "late");
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();
	Csv csv = parseCsv(readFile(dir_ + "/w.csv"));
	// Only the late producer's frames (48 rows), none of the early ones.
	ASSERT_EQ(csv.rows.size(), 48u);
	for (size_t i = 0; i < csv.rows.size(); ++i)
		EXPECT_EQ(csv.str(i, "ProcessID"), std::to_string(producerPid(1)));
}

TEST_F(CliTest, UnknownAndUnsupportedFlagsExitWithAnError)
{
	Proc p;
	ASSERT_TRUE(spawn(p, kCli, { "--bogus_flag" }, dir_, "bad1"));
	EXPECT_EQ(p.wait(5000), 1);
	EXPECT_NE(p.err().find("unrecognized option '--bogus_flag'"), std::string::npos) << p.err();

	Proc q;
	ASSERT_TRUE(spawn(q, kCli, { "--hotkey", "F11" }, dir_, "bad2"));
	EXPECT_EQ(q.wait(5000), 1);
	EXPECT_NE(q.err().find("not supported"), std::string::npos) << q.err();

	Proc h;
	ASSERT_TRUE(spawn(h, kCli, { "--help" }, dir_, "help"));
	EXPECT_EQ(h.wait(5000), 0);
	EXPECT_NE(h.out().find("--process_name"), std::string::npos);
}

// ---- stale segments ----------------------------------------------------------------------------

TEST_F(CliTest, StaleSegmentsAreUnlinkedLiveOnesAreNot)
{
	// A segment whose process died long ago: fork a child, let it exit, reuse its pid for the name.
	pid_t child = ::fork();
	if (child == 0)
		::_exit(0);
	int st = 0;
	::waitpid(child, &st, 0);

	// A live one: the fake producer. It goes first: creating a segment already removes dead
	// leftovers, and this test is about the collector doing it.
	startProducer({ "--frames", "10", "--linger-ms", "4000" });
	const std::string liveName = pml::segmentNameForPid(producerPid());

	pml::ProcessIdentity id = pml::currentProcessIdentity();
	id.pid = child;
	id.processStartTicks = 12345;
	id.processName = procName_ + "_dead.exe";
	const std::string staleName = pml::segmentNameForPid(child);
	pml::OverlayControl ov = {};
	std::string error;
	auto stale = pml::Segment::create(staleName, id, 64, ov, &error);
	ASSERT_TRUE(stale) << error;
	// Silent for a minute: older than the collector's stale age (10 s), younger than the age at which
	// Segment::create reaps (5 minutes), so no other process creating a segment meanwhile removes it first.
	pml::touchHeartbeat(*stale, pml::monotonicNs() - 60 * 1'000'000'000ull);
	ASSERT_TRUE(std::filesystem::exists("/dev/shm/" + staleName));

	startCli({ "--no_csv", "--no_console_stats", "--timed", "1", "--terminate_after_timed" }, /*filterByName=*/false);
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();

	EXPECT_FALSE(std::filesystem::exists("/dev/shm/" + staleName)) << "stale segment should be unlinked";
	EXPECT_NE(cli_.err().find("removed stale segment " + staleName), std::string::npos) << cli_.err();
	EXPECT_TRUE(std::filesystem::exists("/dev/shm/" + liveName)) << "live segment must stay";
	EXPECT_EQ(cli_.err().find("removed stale segment " + liveName), std::string::npos);
	stale->unlink();   // no-op if already gone
}

// ---- list ----------------------------------------------------------------------------------------

TEST_F(CliTest, ListShowsProcessSwapchainAndOverlay)
{
	startProducer({ "--frames", "10", "--linger-ms", "6000" });
	Proc list;
	ASSERT_TRUE(spawn(list, kCli, { "list", "--process_id", std::to_string(producerPid()) }, dir_, "list"));
	ASSERT_EQ(list.wait(10000), 0) << list.err();
	std::string out = list.out();
	for (const std::string &needle :
	     { procName_ + ".exe[" + std::to_string(producerPid()) + "]", std::string("engine=DXVK"), std::string("state=live"),
	       std::string("heartbeat="), std::string("s ago"), std::string("overlay: visible, top-left, scale auto, available"),
	       std::string("swapchain #1 0x00000000ABC0DE12"), std::string("2560x1440"), std::string("B8G8R8A8_UNORM"),
	       std::string("SRGB_NONLINEAR"), std::string("FIFO"), std::string("Wayland"),
	       std::string("supported=QUEUE_OPERATIONS_END+IMAGE_FIRST_PIXEL_OUT"), std::string("flags=layer-timing"),
	       std::string("written 10") })
		EXPECT_NE(out.find(needle), std::string::npos) << needle << "\n" << out;

	// Filtering by name finds it too; a name that does not exist reports nothing.
	Proc byName;
	ASSERT_TRUE(spawn(byName, kCli, { "list", "--process_name", procName_ }, dir_, "list2"));
	ASSERT_EQ(byName.wait(10000), 0);
	EXPECT_NE(byName.out().find("swapchain #1"), std::string::npos);
	Proc none;
	ASSERT_TRUE(spawn(none, kCli, { "list", "--process_name", "definitely_not_running" }, dir_, "list3"));
	ASSERT_EQ(none.wait(10000), 0);
	EXPECT_NE(none.out().find("no layer-enabled processes"), std::string::npos) << none.out();
}

TEST_F(CliTest, ListSaysWhenTheOverlayIsNotAvailable)
{
	startProducer({ "--frames", "5", "--linger-ms", "4000", "--no-overlay" });
	Proc list;
	ASSERT_TRUE(spawn(list, kCli, { "list", "--process_id", std::to_string(producerPid()) }, dir_, "list"));
	ASSERT_EQ(list.wait(10000), 0);
	EXPECT_NE(list.out().find("not available"), std::string::npos) << list.out();
}

// ---- overlay -------------------------------------------------------------------------------------

TEST_F(CliTest, OverlayControlRoundTripThroughTheSegment)
{
	startProducer({ "--frames", "5", "--linger-ms", "15000" });
	const std::string pid = std::to_string(producerPid());
	std::string error;
	auto seg = pml::Segment::open(pml::segmentNameForPid(producerPid()), &error);
	ASSERT_TRUE(seg) << error;

	auto run = [&](std::vector<std::string> args, int *code = nullptr) {
		static int n = 0;
		Proc p;
		args.push_back("--process_id");
		args.push_back(pid);
		std::vector<std::string> full = { "overlay" };
		full.insert(full.end(), args.begin(), args.end());
		EXPECT_TRUE(spawn(p, kCli, full, dir_, "ov" + std::to_string(n++)));
		int c = p.wait(10000);
		if (code)
			*code = c;
		return p.out() + p.err();
	};

	pml::OverlayControl c0 = pml::readOverlayControl(*seg);
	EXPECT_EQ(c0.visible, 1u);
	EXPECT_EQ(c0.available, 1u);

	std::string out = run({ "status" });
	EXPECT_NE(out.find("overlay visible, top-left, scale auto, available"), std::string::npos) << out;
	EXPECT_EQ(pml::readOverlayControl(*seg).generation, c0.generation) << "status must not write";

	out = run({ "toggle" });
	pml::OverlayControl c1 = pml::readOverlayControl(*seg);
	EXPECT_EQ(c1.visible, 0u);
	EXPECT_GT(c1.generation, c0.generation);
	EXPECT_NE(out.find("hidden"), std::string::npos) << out;

	run({ "toggle" });
	pml::OverlayControl c2 = pml::readOverlayControl(*seg);
	EXPECT_EQ(c2.visible, 1u);
	EXPECT_GT(c2.generation, c1.generation);

	run({ "hide" });
	EXPECT_EQ(pml::readOverlayControl(*seg).visible, 0u);
	out = run({ "show", "--position", "bottom-right", "--scale", "1.5" });
	pml::OverlayControl c3 = pml::readOverlayControl(*seg);
	EXPECT_EQ(c3.visible, 1u);
	EXPECT_EQ(c3.position, uint32_t(pml::OverlayPosition::BottomRight));
	EXPECT_EQ(c3.scalePermille, 1500u);
	EXPECT_EQ(c3.available, 1u) << "the CLI must not touch `available`";
	EXPECT_NE(out.find("visible, bottom-right, scale 1.50, available"), std::string::npos) << out;

	// position/scale are kept by a plain toggle; scale can go back to auto
	run({ "toggle" });
	EXPECT_EQ(pml::readOverlayControl(*seg).position, uint32_t(pml::OverlayPosition::BottomRight));
	run({ "show", "--scale", "auto", "--position", "top-left" });
	c3 = pml::readOverlayControl(*seg);
	EXPECT_EQ(c3.scalePermille, 0u);
	EXPECT_EQ(c3.position, uint32_t(pml::OverlayPosition::TopLeft));

	// Errors
	int code = 0;
	out = run({ "show", "--position", "middle" }, &code);
	EXPECT_EQ(code, 1) << out;
	out = run({ "show", "--scale", "99" }, &code);
	EXPECT_EQ(code, 1) << out;
	out = run({ "status", "--scale", "2" }, &code);
	EXPECT_EQ(code, 1) << out;
	out = run({ "explode" }, &code);
	EXPECT_EQ(code, 1) << out;

	// No matching process: exit 1
	Proc none;
	ASSERT_TRUE(spawn(none, kCli, { "overlay", "toggle", "--process_name", "definitely_not_running" }, dir_, "ovnone"));
	EXPECT_EQ(none.wait(10000), 1);
	EXPECT_NE(none.err().find("no matching live"), std::string::npos) << none.err();
}

TEST_F(CliTest, OverlayWarnsWhenNotLoaded)
{
	startProducer({ "--frames", "5", "--linger-ms", "5000", "--no-overlay" });
	Proc p;
	ASSERT_TRUE(spawn(p, kCli, { "overlay", "show", "--process_id", std::to_string(producerPid()) }, dir_, "ov"));
	ASSERT_EQ(p.wait(10000), 0);
	EXPECT_NE(p.out().find("not available"), std::string::npos) << p.out();
	EXPECT_NE(p.err().find("overlay is not loaded"), std::string::npos) << p.err();
}

// ---- review fixes ---------------------------------------------------------------------------------

namespace {

pid_t deadPid()
{
	pid_t child = ::fork();
	if (child == 0)
		::_exit(0);
	int st = 0;
	::waitpid(child, &st, 0);
	return child;
}

// The file a layer leaves when it dies between shm_open/ftruncate and publishing the magic.
std::string makeHalfCreated(pid_t pid, int ageSeconds)
{
	std::string name = pml::segmentNameForPid(pid);
	int fd = ::shm_open(("/" + name).c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
	EXPECT_GE(fd, 0);
	if (fd >= 0)
	{
		EXPECT_EQ(::ftruncate(fd, off_t(pml::segmentSize(8192))), 0);
		::close(fd);
		timeval tv[2];
		tv[0].tv_sec = tv[1].tv_sec = ::time(nullptr) - ageSeconds;
		tv[0].tv_usec = tv[1].tv_usec = 0;
		::utimes(("/dev/shm/" + name).c_str(), tv);
	}
	return name;
}

} // namespace

// M1 / K3: a segment that never got its magic cannot be opened, so it was never judged stale.
TEST_F(CliTest, AbandonedHalfCreatedSegmentIsRemovedYoungOnesAreKept)
{
	pid_t deadOld = deadPid();
	pid_t deadYoung = deadPid();
	while (deadYoung == deadOld)
		deadYoung = deadPid();
	const std::string oldName = makeHalfCreated(deadOld, 60);
	const std::string youngName = makeHalfCreated(deadYoung, 0);

	// `list` neither shows nor trips over them
	Proc list;
	ASSERT_TRUE(spawn(list, kCli, { "list", "--process_id", std::to_string(deadOld) }, dir_, "list"));
	ASSERT_EQ(list.wait(10000), 0) << list.err();
	EXPECT_NE(list.out().find("hidden"), std::string::npos) << list.out();
	EXPECT_EQ(list.out().find("still initializing"), std::string::npos) << list.out();

	startCli({ "--no_csv", "--no_console_stats", "--timed", "1", "--terminate_after_timed" });
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();
	EXPECT_FALSE(std::filesystem::exists("/dev/shm/" + oldName));
	EXPECT_NE(cli_.err().find("removed abandoned segment " + oldName), std::string::npos) << cli_.err();
	EXPECT_TRUE(std::filesystem::exists("/dev/shm/" + youngName)) << "within the grace period";
	::shm_unlink(("/" + youngName).c_str());
}

namespace {

// Creates this test process's segment the way the layer does, for tests that drive the ring directly.
std::unique_ptr<pml::Segment> makeOwnSegment(const std::string &processName, std::string *error)
{
	pml::ProcessIdentity id = pml::currentProcessIdentity();
	id.processName = processName;
	id.engineName = "DXVK";
	id.layerVersion = "test";
	pml::OverlayControl initial = {};
	return pml::Segment::create(pml::segmentNameForPid(id.pid), id, 8192, initial, error);
}

void publishFrames(pml::Segment &segment, uint32_t frames, uint64_t baseNs)
{
	faketl::Params params;
	params.frames = frames;
	params.baseNs = baseNs;
	pml::publishSwapchain(segment, faketl::makeSwapchainInfo(baseNs));
	pml::RingWriter writer(segment);
	for (uint32_t i = 1; i <= frames; ++i)
		writer.publish(faketl::makeFrame(params, i));
	pml::touchHeartbeat(segment, pml::monotonicNs());
}

} // namespace

// M2: the same pid (and start time) with a new segment under the same name, e.g. after execve.
TEST_F(CliTest, ReCreatedSegmentOfALiveProcessIsFollowed)
{
	startCli({ "--output_file", "re.csv", "--no_console_stats", "--discovery_ms", "50" });
	std::string error;
	auto first = makeOwnSegment(procName_ + ".exe", &error);
	ASSERT_TRUE(first) << error;
	publishFrames(*first, 50, pml::monotonicNs());
	sleepMs(500);

	auto second = makeOwnSegment(procName_ + ".exe", &error);   // unlinks and re-creates the name
	ASSERT_TRUE(second) << error;
	EXPECT_TRUE(first->replacedOnDisk());
	publishFrames(*second, 30, pml::monotonicNs());
	sleepMs(500);

	cli_.signal(SIGINT);
	ASSERT_EQ(cli_.wait(10000), 0) << cli_.err();
	EXPECT_NE(cli_.err().find("re-created its segment"), std::string::npos) << cli_.err();
	// 48 rows of the first image, 28 of the second (the first present of a chain only seeds it)
	EXPECT_EQ(parseCsv(readFile(dir_ + "/re.csv")).rows.size(), 76u);
	first->unlink();    // must leave the replacement alone
	EXPECT_TRUE(std::filesystem::exists("/dev/shm/" + second->name()));
	second->unlink();
}

// M4: --delay/--timed select frames by when they were presented, not when their record was processed.
TEST_F(CliTest, RecordingWindowUsesThePresentTime)
{
	const uint64_t t0 = pml::monotonicNs();
	startCli({ "--output_file", "win.csv", "--no_console_stats", "--delay", "1", "--timed", "1", "--qpc_time",
	           "--terminate_after_timed", "--discovery_ms", "50" });
	std::string error;
	auto segment = makeOwnSegment(procName_ + ".exe", &error);
	ASSERT_TRUE(segment) << error;
	auto waitUntil = [&](double seconds) {
		while (pml::monotonicNs() < t0 + uint64_t(seconds * 1e9))
			sleepMs(5);
	};

	// Presented at 0.3 s, published at 1.3 s (inside the window by the clock): must not be recorded.
	waitUntil(1.3);
	publishFrames(*segment, 20, t0 + 300 * 1000000ull);
	// Presented at 1.5 s (inside the window), published at 2.05 s, after the window ended: recorded.
	waitUntil(2.05);
	publishFrames(*segment, 20, t0 + 1500 * 1000000ull);

	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();
	Csv csv = parseCsv(readFile(dir_ + "/win.csv"));
	segment->unlink();
	ASSERT_GE(csv.rows.size(), 15u) << "frames presented inside the window were dropped";
	EXPECT_LE(csv.rows.size(), 20u);
	// The window selects by present time (TimeInQPC). CPUStartQPC is not checked: the first recorded row's
	// CPU start is the end of the previous present, which lies before the window (as in upstream).
	for (size_t i = 0; i < csv.rows.size(); ++i)
		EXPECT_GE(csv.num(i, "TimeInQPC"), double(t0 + 1400 * 1000000ull)) << "row " << i << " is from before the delay";
}

// M5: losses are reported without console stats, also those that happened before the first sweep.
TEST_F(CliTest, LossIsWarnedAboutWithoutConsoleStats)
{
	startCli({ "--no_csv", "--no_console_stats", "--terminate_on_proc_exit", "--discovery_ms", "800" });
	startProducer({ "--frames", "500", "--ring", "64", "--linger-ms", "2500" });
	ASSERT_EQ(cli_.wait(20000), 0) << cli_.err();
	EXPECT_NE(cli_.err().find("records lost so far"), std::string::npos) << cli_.err();
	EXPECT_NE(cli_.err().find("records were lost"), std::string::npos) << cli_.err();
}

// The final summary also counts the losses of a process that exited, and whose source was dropped, before the stop.
TEST_F(CliTest, LossOfAnExitedProcessIsInTheFinalSummary)
{
	startCli({ "--no_csv", "--no_console_stats", "--discovery_ms", "100" });
	startProducer({ "--frames", "500", "--ring", "64", "--linger-ms", "300" });
	ASSERT_EQ(producers_.back().wait(10000), 0);
	sleepMs(800);   // several discovery sweeps: the source of the exited process is gone by now
	cli_.signal(SIGINT);
	ASSERT_EQ(cli_.wait(10000), 0) << cli_.err();
	EXPECT_NE(cli_.err().find("records lost so far"), std::string::npos) << cli_.err();
	EXPECT_NE(cli_.err().find("records were lost"), std::string::npos) << cli_.err();
}

// L1: closing the terminal (SIGHUP) flushes the CSV like Ctrl+C.
TEST_F(CliTest, SighupFlushesAndClosesTheCsv)
{
	startCli({ "--output_file", "hup.csv", "--no_console_stats" });
	startProducer({ "--frames", "200", "--linger-ms", "20000" });
	sleepMs(500);
	cli_.signal(SIGHUP);
	bool timedOut = false;
	EXPECT_EQ(cli_.wait(10000, &timedOut), 0) << cli_.err();
	EXPECT_FALSE(timedOut);
	std::string text = readFile(dir_ + "/hup.csv");
	ASSERT_FALSE(text.empty());
	EXPECT_EQ(text.back(), '\n');
	EXPECT_EQ(parseCsv(text).rows.size(), 198u);
}

// A SIGHUP the parent left ignored (nohup) stays ignored: the capture survives a hangup and still stops on
// Ctrl+C with a complete CSV.
TEST_F(CliTest, InheritedIgnoredSighupStaysIgnored)
{
	struct sigaction ignore = {}, previous = {};
	ignore.sa_handler = SIG_IGN;
	sigemptyset(&ignore.sa_mask);
	ASSERT_EQ(sigaction(SIGHUP, &ignore, &previous), 0);
	gSpawnKeepsSignal = SIGHUP;   // posix_spawn then passes SIG_IGN on, as nohup's exec does
	startCli({ "--output_file", "nohup.csv", "--no_console_stats" });
	gSpawnKeepsSignal = 0;
	sigaction(SIGHUP, &previous, nullptr);
	startProducer({ "--frames", "200", "--linger-ms", "20000" });
	sleepMs(500);
	cli_.signal(SIGHUP);
	sleepMs(700);
	ASSERT_TRUE(cli_.running()) << "an inherited-ignored SIGHUP stopped the capture\n" << cli_.err();
	cli_.signal(SIGINT);
	bool timedOut = false;
	EXPECT_EQ(cli_.wait(10000, &timedOut), 0) << cli_.err();
	EXPECT_FALSE(timedOut);
	EXPECT_EQ(parseCsv(readFile(dir_ + "/nohup.csv")).rows.size(), 198u);
}

// NF9: a live swapchain that pauses (a loading screen, a paused game) keeps its engine however long the pause:
// its first present after the pause is a row like any other, not a new seed. (Only destroyed swapchains are
// dropped, see the next test.)
TEST_F(CliTest, LiveSwapchainKeepsItsEngineAcrossALongPause)
{
	startCli({ "--output_file", "pause.csv", "--no_console_stats", "--discovery_ms", "100", "--v1_metrics" }, true,
	         "debug");
	std::string error;
	auto segment = makeOwnSegment(procName_ + ".exe", &error);
	ASSERT_TRUE(segment) << error;
	publishFrames(*segment, 20, pml::monotonicNs());
	sleepMs(5500);   // longer than the 4 s after which a destroyed swapchain's engine is dropped
	publishFrames(*segment, 20, pml::monotonicNs());
	sleepMs(600);
	cli_.signal(SIGINT);
	ASSERT_EQ(cli_.wait(10000), 0) << cli_.err();
	segment->unlink();
	EXPECT_EQ(cli_.err().find("dropped idle swapchain"), std::string::npos) << cli_.err();
	EXPECT_EQ(parseCsv(readFile(dir_ + "/pause.csv")).rows.size(), 39u) << "40 presents: only the very first one seeds";
}

// L4: the engine of a destroyed swapchain is dropped once it has been idle for a few seconds.
TEST_F(CliTest, IdleDestroyedSwapchainsAreDropped)
{
	startCli({ "--no_csv", "--no_console_stats", "--discovery_ms", "100" }, true, "debug");
	std::string error;
	auto segment = makeOwnSegment(procName_ + ".exe", &error);
	ASSERT_TRUE(segment) << error;
	publishFrames(*segment, 20, pml::monotonicNs());
	pml::SwapchainInfo info = faketl::makeSwapchainInfo(pml::monotonicNs());
	info.destroyedNs = pml::monotonicNs();
	pml::updateSwapchain(*segment, 0, info);
	EXPECT_TRUE(waitForText(cli_.errPath, "dropped idle swapchain #1", 9000)) << cli_.err();
	segment->unlink();
}

// E2: a process that died is not listed (its segment is only waiting to be cleaned up).
TEST_F(CliTest, ListDoesNotShowADeadProcess)
{
	startProducer({ "--frames", "10", "--linger-ms", "300", "--crash" });
	const pid_t pid = producerPid();
	ASSERT_EQ(producers_[0].wait(10000), 128 + SIGKILL);
	ASSERT_TRUE(std::filesystem::exists("/dev/shm/" + pml::segmentNameForPid(pid)));

	Proc list;
	ASSERT_TRUE(spawn(list, kCli, { "list", "--process_id", std::to_string(pid) }, dir_, "list"));
	ASSERT_EQ(list.wait(10000), 0) << list.err();
	EXPECT_EQ(list.out().find("state=dead"), std::string::npos) << list.out();
	EXPECT_EQ(list.out().find(procName_), std::string::npos) << list.out();
	EXPECT_NE(list.out().find("no layer-enabled processes"), std::string::npos) << list.out();
	EXPECT_NE(list.out().find("hidden"), std::string::npos) << list.out();
	::unlink(("/dev/shm/" + pml::segmentNameForPid(pid)).c_str());
}

// L5: header strings are never read past their field, even if a foreign segment does not terminate them.
TEST_F(CliTest, ListReadsUnterminatedHeaderStringsWithABound)
{
	std::string error;
	auto segment = makeOwnSegment(procName_ + ".exe", &error);
	ASSERT_TRUE(segment) << error;
	auto &h = segment->header();
	std::memset(h.processName, 'A', sizeof(h.processName));
	std::memset(h.engineName, 'E', sizeof(h.engineName));
	std::memset(h.layerVersion, 'V', sizeof(h.layerVersion));
	Proc list;
	ASSERT_TRUE(spawn(list, kCli, { "list", "--process_id", std::to_string(::getpid()) }, dir_, "list"));
	ASSERT_EQ(list.wait(10000), 0) << list.err();
	const std::string out = list.out();
	EXPECT_NE(out.find(std::string(sizeof(h.processName), 'A') + "[" + std::to_string(::getpid()) + "]"),
	          std::string::npos) << out.substr(0, 400);
	EXPECT_NE(out.find("engine=" + std::string(sizeof(h.engineName), 'E') + "  layer=" +
	                   std::string(sizeof(h.layerVersion), 'V') + " "),
	          std::string::npos) << out.substr(0, 600);
	segment->unlink();
}
