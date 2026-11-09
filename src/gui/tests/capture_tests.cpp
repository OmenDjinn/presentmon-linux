// SPDX-License-Identifier: MIT
// Tests for the capture side of the GUI backend: the CSV reader and its statistics, CaptureAnalysis,
// CaptureLibrary, Recorder (end to end with the real presentmon CLI and the fake producer),
// LayerStatus and Hotkeys.
#include "test_support.hpp"

#include "captureanalysis.hpp"
#include "capturelibrary.hpp"
#include "csvreader.hpp"
#include "format.hpp"
#include "hotkeys.hpp"
#include "layerstatus.hpp"
#include "recorder.hpp"

#include <gtest/gtest.h>

#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>

#include <cmath>
#include <cstdarg>
#include <filesystem>
#include <numeric>
#include <string>

using namespace pml::gui;

namespace {

// ---- Synthetic CSV text ----

// 990 frames of 10 ms and 10 of 50 ms (the 50 ms ones every 100th frame): avg 10.4 ms, 1 % low = 20 fps.
std::vector<double> sampleFrames()
{
	std::vector<double> f(1000, 10.0);
	for (size_t i = 99; i < f.size(); i += 100)
		f[i] = 50.0;
	return f;
}

struct Layout
{
	const char *header;
	// One row: index, frame time (ms), cumulative start (ms).
	std::string (*row)(size_t i, double frameMs, double startMs, bool displayed);
};

std::string fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char *f, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, f);
	std::vsnprintf(buf, sizeof(buf), f, ap);
	va_end(ap);
	return buf;
}

// Our default layout: TimeInMs, MsBetweenPresents, MsBetweenDisplayChange (NA when not displayed).
const Layout kDefault{
	"Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,PresentFlags,AllowsTearing,PresentMode,TimeInMs,"
	"MsBetweenPresents,MsBetweenDisplayChange,MsInPresentAPI",
	[](size_t, double ft, double start, bool shown) {
		return fmt("Game.exe,100,0x1A,DXVK,1,0,0,Hardware: Independent Flip,%.4f,%.14f,%s,0.1", start, ft,
		           shown ? fmt("%.14f", ft).c_str() : "NA");
	}};

// --v1_metrics: TimeInSeconds, Dropped, msBetweenPresents, msBetweenDisplayChange (0 when dropped).
const Layout kV1{
	"Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags,Dropped,TimeInSeconds,msInPresentAPI,msBetweenPresents,"
	"AllowsTearing,PresentMode,msUntilRenderComplete,msUntilDisplayed,msBetweenDisplayChange",
	[](size_t, double ft, double start, bool shown) {
		return fmt("Game.exe,100,0x000000000000001A,DXVK,1,0,%d,%.14f,0.1,%.14f,0,Composed: Flip,0.1,1.0,%.14f", shown ? 0 : 1,
		           start / 1000.0, ft, shown ? ft : 0.0);
	}};

// --v2_metrics: CPUStartTime (ms), FrameTime, DisplayedTime (NA when not displayed).
const Layout kV2{
	"Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,PresentFlags,AllowsTearing,PresentMode,FrameType,CPUStartTime,"
	"FrameTime,CPUBusy,CPUWait,DisplayLatency,DisplayedTime",
	[](size_t, double ft, double start, bool shown) {
		return fmt("Game.exe,100,0x1A,DXVK,1,0,0,Hardware: Independent Flip,Application,%.4f,%.4f,%.4f,0.0000,%s", start, ft, ft,
		           shown ? fmt("20.0000,%.4f", ft).c_str() : "NA,NA");
	}};

// Time in QPC ticks, with the tick rate as a parameter (ours: ns, Intel's: 10 MHz).
std::string qpcRow(double ft, double start, double ticksPerMs, bool shown)
{
	return fmt("Game.exe,100,0x1A,DXVK,1,0,0,Hardware: Independent Flip,%llu,%.4f,%s",
	           static_cast<unsigned long long>(5e12 + start * ticksPerMs), ft, shown ? fmt("%.4f", ft).c_str() : "NA");
}
const char *kQpcHeader =
	"Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,PresentFlags,AllowsTearing,PresentMode,TimeInQPC,"
	"MsBetweenPresents,MsBetweenDisplayChange";

std::string makeCsv(const Layout &l, const std::vector<double> &frames, const char *eol = "\n", size_t dropEvery = 0)
{
	std::string s = std::string(l.header) + eol;
	double t = 0;
	for (size_t i = 0; i < frames.size(); ++i)
	{
		s += l.row(i, frames[i], t, !(dropEvery && i % dropEvery == dropEvery - 1)) + eol;
		t += frames[i];
	}
	return s;
}

CaptureData parseOk(const std::string &text)
{
	CaptureData d;
	std::string err;
	EXPECT_TRUE(parseCaptureCsv(text, &d, &err)) << err;
	return d;
}

std::string writeFile(const QString &path, const std::string &text)
{
	QFile f(path);
	EXPECT_TRUE(f.open(QIODevice::WriteOnly));
	f.write(text.data(), qint64(text.size()));
	return path.toStdString();
}

} // namespace

// ---- csvreader ----

TEST(CsvReader, DefaultLayoutKnownNumbers)
{
	const CaptureData d = parseOk(makeCsv(kDefault, sampleFrames()));
	EXPECT_EQ(d.frameMs.size(), 1000u);
	EXPECT_EQ(d.frameColumn, "MsBetweenPresents");
	EXPECT_EQ(d.timeColumn, "TimeInMs");
	EXPECT_EQ(d.application, "Game.exe");
	EXPECT_EQ(d.runtime, "DXVK");
	EXPECT_EQ(d.presentMode, "Hardware: Independent Flip");
	EXPECT_TRUE(d.hasDisplay);
	EXPECT_EQ(d.droppedFrames, 0u);
	EXPECT_TRUE(d.warnings.empty());
	EXPECT_NEAR(d.timeSec[1], 0.010, 1e-6);

	const CaptureStats s = computeCaptureStats(d);
	EXPECT_EQ(s.frames, 1000u);
	EXPECT_NEAR(s.times.avgMs, 10.4, 1e-6);
	EXPECT_NEAR(s.avgFps, 1000.0 / 10.4, 1e-6);
	EXPECT_NEAR(s.times.low1Fps, 20.0, 1e-6);
	EXPECT_NEAR(s.times.p50Ms, 10.0, 1e-9);
	EXPECT_NEAR(s.times.p99Ms, 10.0, 1e-9);   // nearest rank: exactly 1 % are slow, so the 99th is still 10 ms
	EXPECT_NEAR(s.times.maxMs, 50.0, 1e-9);
	EXPECT_NEAR(s.durationSec, 10.4 - 0.05, 1e-3);   // first start to last start
	EXPECT_NEAR(s.displayFps, 1000.0 / 10.4, 0.5);
	EXPECT_DOUBLE_EQ(s.droppedPct, 0.0);
}

TEST(CsvReader, DroppedFramesDefaultLayoutNA)
{
	// Every 10th frame has NA in MsBetweenDisplayChange: not displayed, so dropped.
	const CaptureData d = parseOk(makeCsv(kDefault, std::vector<double>(100, 10.0), "\n", 10));
	EXPECT_EQ(d.droppedFrames, 10u);
	EXPECT_EQ(std::count_if(d.displayMs.begin(), d.displayMs.end(), [](float v) { return std::isnan(v); }), 10);
	const CaptureStats s = computeCaptureStats(d);
	EXPECT_NEAR(s.droppedPct, 10.0, 1e-9);
	EXPECT_NEAR(s.displayFps, 90.0 / s.durationSec, 1e-6);
}

TEST(CsvReader, V1Layout)
{
	const CaptureData d = parseOk(makeCsv(kV1, sampleFrames(), "\n", 20));
	EXPECT_EQ(d.frameColumn, "msBetweenPresents");
	EXPECT_EQ(d.timeColumn, "TimeInSeconds");
	EXPECT_EQ(d.runtime, "DXVK");
	EXPECT_EQ(d.droppedFrames, 50u);   // the Dropped column
	EXPECT_TRUE(d.hasDisplay);
	EXPECT_NEAR(d.timeSec[10], 0.100, 1e-9);
	const CaptureStats s = computeCaptureStats(d);
	EXPECT_NEAR(s.times.low1Fps, 20.0, 1e-6);
	EXPECT_NEAR(s.droppedPct, 5.0, 1e-9);
}

TEST(CsvReader, V2LayoutUsesFrameTimeAndDisplayedTime)
{
	const CaptureData d = parseOk(makeCsv(kV2, sampleFrames(), "\n", 50));
	EXPECT_EQ(d.frameColumn, "FrameTime");
	EXPECT_EQ(d.timeColumn, "CPUStartTime");
	EXPECT_TRUE(d.hasDisplay);
	EXPECT_EQ(d.droppedFrames, 20u);
	// MsBetweenDisplayChange(i) = DisplayedTime of the previous displayed frame: the first has none,
	// and a frame that follows a dropped one still gets its predecessor's value.
	EXPECT_TRUE(std::isnan(d.displayMs[0]));
	EXPECT_NEAR(d.displayMs[1], 10.0, 1e-4);
	EXPECT_TRUE(std::isnan(d.displayMs[49]));   // dropped
	EXPECT_NEAR(d.displayMs[50], 10.0, 1e-4);   // from frame 48 (frame 49 was dropped)
	EXPECT_NEAR(computeCaptureStats(d).times.low1Fps, 20.0, 1e-6);
}

TEST(CsvReader, MsBetweenAppStartIsTheLastResort)
{
	const std::string text = "Application,ProcessID,SwapChainAddress,CPUStartTimeInMs,MsBetweenAppStart\n"
	                         "A.exe,1,0x1,0.0,16.5\nA.exe,1,0x1,16.5,16.5\nA.exe,1,0x1,33.0,16.5\n";
	const CaptureData d = parseOk(text);
	EXPECT_EQ(d.frameColumn, "MsBetweenAppStart");
	EXPECT_EQ(d.timeColumn, "CPUStartTimeInMs");
	EXPECT_FALSE(d.hasDisplay);
	EXPECT_NEAR(d.timeSec[2], 0.033, 1e-9);
	EXPECT_EQ(computeCaptureStats(d).droppedPct, 0.0);
}

TEST(CsvReader, QpcNanosecondsAndTenMegahertzTicks)
{
	for (double ticksPerMs : {1e6, 1e4})   // our writer (ns) and Intel's (10 MHz)
	{
		std::string s = std::string(kQpcHeader) + "\n";
		double t = 0;
		for (int i = 0; i < 200; ++i)
		{
			s += qpcRow(10.0, t, ticksPerMs, true) + "\n";
			t += 10.0;
		}
		const CaptureData d = parseOk(s);
		EXPECT_EQ(d.timeColumn, "TimeInQPC");
		EXPECT_NEAR(d.timeSec.back(), 1.99, 1e-6) << "ticks per ms " << ticksPerMs;
		EXPECT_NEAR(computeCaptureStats(d).durationSec, 1.99, 1e-6);
	}
}

TEST(CsvReader, MislabelledTimeUnitIsCorrectedFromFrameTimes)
{
	// A header that says seconds over millisecond values (what a --qpc_time_ms file carries).
	std::string s = "Application,ProcessID,SwapChainAddress,TimeInSeconds,MsBetweenPresents\n";
	for (int i = 0; i < 100; ++i)
		s += fmt("G.exe,1,0x1,%.4f,10.0\n", i * 10.0);
	EXPECT_NEAR(parseOk(s).timeSec.back(), 0.99, 1e-6);
}

TEST(CsvReader, DateTimeColumn)
{
	std::string s = "Application,ProcessID,SwapChainAddress,CPUStartDateTime,FrameTime\n";
	for (int i = 0; i < 50; ++i)
	{
		const double secs = 59.0 + i * 0.020;   // crosses a minute boundary
		const int m = 4 + int(secs / 60.0);
		s += fmt("G.exe,1,0x1,2024-5-3 14:%d:%012.9f,20.0\n", m, std::fmod(secs, 60.0));
	}
	const CaptureData d = parseOk(s);
	EXPECT_EQ(d.timeColumn, "CPUStartDateTime");
	EXPECT_NEAR(d.timeSec.back(), 0.98, 1e-6);
}

TEST(CsvReader, NoTimeColumnUsesCumulativeFrameTimes)
{
	const CaptureData d = parseOk("Application,ProcessID,SwapChainAddress,MsBetweenPresents\nG,1,0x1,10\nG,1,0x1,20\nG,1,0x1,30\n");
	EXPECT_EQ(d.timeColumn, "");
	EXPECT_NEAR(d.timeSec[2], 0.030, 1e-9);
	EXPECT_NEAR(computeCaptureStats(d).durationSec, 0.060, 1e-9);
}

TEST(CsvReader, BomCrlfQuotesAndNA)
{
	const std::string text = "\xEF\xBB\xBF"
	                         "Application,ProcessID,SwapChainAddress,PresentRuntime,TimeInSeconds,MsBetweenPresents,MsBetweenDisplayChange\r\n"
	                         "\"My, \"\"Quoted\"\" Game.exe\",7,0x1,DXGI,0.0,10.0,10.0\r\n"
	                         "\"My, \"\"Quoted\"\" Game.exe\",7,0x1,DXGI,0.01,NA,NA\r\n"
	                         "\r\n"
	                         "\"My, \"\"Quoted\"\" Game.exe\",7,0x1,DXGI,0.02,20.0,20.0\r\n";
	const CaptureData d = parseOk(text);
	EXPECT_EQ(d.application, "My, \"Quoted\" Game.exe");
	ASSERT_EQ(d.frameMs.size(), 3u);
	EXPECT_TRUE(std::isnan(d.frameMs[1]));
	EXPECT_FLOAT_EQ(d.frameMs[2], 20.0f);
	EXPECT_TRUE(d.hasDisplay);
	EXPECT_EQ(d.droppedFrames, 1u);
	const CaptureStats s = computeCaptureStats(d);
	EXPECT_NEAR(s.times.avgMs, 15.0, 1e-9);   // NA is ignored
}

TEST(CsvReader, SeveralSwapchainsKeepsTheBiggestAndWarns)
{
	std::string s = "Application,ProcessID,SwapChainAddress,PresentRuntime,MsBetweenPresents\n";
	for (int i = 0; i < 50; ++i)
		s += "Game.exe,100,0x1,DXVK,10.0\n";
	for (int i = 0; i < 20; ++i)
		s += "dwm.exe,200,0x2,DXGI,16.6\nGame.exe,100,0x3,DXVK,33.0\n";
	const CaptureData d = parseOk(s);
	EXPECT_EQ(d.frameMs.size(), 50u);
	EXPECT_EQ(d.swapchainCount, 3u);
	EXPECT_EQ(d.processCount, 2u);
	EXPECT_EQ(d.application, "Game.exe");
	ASSERT_EQ(d.warnings.size(), 1u);
	EXPECT_NE(d.warnings[0].find("dwm.exe"), std::string::npos) << d.warnings[0];
	EXPECT_NE(d.warnings[0].find("PID 200"), std::string::npos) << d.warnings[0];
}

TEST(CsvReader, BlankLinesAndShortRowsAreNotRows)
{
	std::string base = "Application,ProcessID,SwapChainAddress,MsBetweenPresents\n";
	for (int i = 0; i < 20; ++i)
		base += "a.exe,1,0x1,10.0\n";
	for (const char *tail : {"\n\n", "\r\n\r\n", "a.exe\n"})
	{
		const CaptureData d = parseOk(base + tail);
		EXPECT_EQ(d.frameMs.size(), 20u) << tail;
		EXPECT_EQ(d.swapchainCount, 1u) << tail;
		EXPECT_TRUE(d.warnings.empty()) << tail;
	}
	// Two rows with a blank line between: the blank line must not form a competing group.
	const CaptureData t = parseOk("Application,ProcessID,SwapChainAddress,MsBetweenPresents\na.exe,1,0x1,10\n\na.exe,1,0x1,12\n");
	EXPECT_EQ(t.frameMs.size(), 2u);
	EXPECT_EQ(t.swapchainCount, 1u);
}

TEST(CsvReader, OutOfRangeNumbersAreNA)
{
	const CaptureData d = parseOk("Application,ProcessID,SwapChainAddress,MsBetweenPresents,MsBetweenDisplayChange\n"
	                              "a.exe,1,0x1,1e50,1e300\na.exe,1,0x1,10,10\n");
	ASSERT_EQ(d.frameMs.size(), 2u);
	EXPECT_TRUE(std::isnan(d.frameMs[0]));
	const CaptureStats s = computeCaptureStats(d);
	EXPECT_EQ(s.times.count, 1u);
	EXPECT_NEAR(s.times.avgMs, 10.0, 1e-9);
}

TEST(CsvReader, OverAnHourIsNotAFrameTime)
{
	const CaptureData d = parseOk("Application,ProcessID,SwapChainAddress,MsBetweenPresents,MsBetweenDisplayChange\n"
	                              "a.exe,1,0x1,1e30,10\na.exe,1,0x1,3600001,3e9\na.exe,1,0x1,3599999,10\na.exe,1,0x1,10,10\n");
	ASSERT_EQ(d.frameMs.size(), 4u);
	EXPECT_TRUE(std::isnan(d.frameMs[0]));
	EXPECT_TRUE(std::isnan(d.frameMs[1]));
	EXPECT_FLOAT_EQ(d.frameMs[2], 3599999.0f);   // a one-hour freeze is still a frame
	EXPECT_TRUE(std::isnan(d.displayMs[1]));
	ASSERT_EQ(d.warnings.size(), 1u);
	EXPECT_NE(d.warnings[0].find("3 frame or display times over one hour were ignored"), std::string::npos) << d.warnings[0];
	const CaptureStats s = computeCaptureStats(d);
	EXPECT_EQ(s.times.count, 2u);
}

TEST(CsvReader, Errors)
{
	CaptureData d;
	std::string err;
	EXPECT_FALSE(parseCaptureCsv("", &d, &err));
	EXPECT_FALSE(err.empty());
	err.clear();
	EXPECT_FALSE(parseCaptureCsv("Application,ProcessID\nA,1\n", &d, &err));
	EXPECT_NE(err.find("frame time"), std::string::npos) << err;
	err.clear();
	EXPECT_FALSE(parseCaptureCsv("Application,MsBetweenPresents\n", &d, &err));   // header only
	EXPECT_FALSE(err.empty());
	err.clear();
	EXPECT_FALSE(readCaptureCsv("/nonexistent/dir/none.csv", &d, &err));
	EXPECT_FALSE(err.empty());
}

TEST(CsvReader, Histogram)
{
	const std::vector<float> f = {1.f, 1.5f, 2.5f, 9.f, 100.f, NAN, 0.f};
	const auto h = frameTimeHistogram(f, 2.0, 10.0);   // bins 0-2, 2-4, ..., 8-10
	ASSERT_EQ(h.size(), 5u);
	EXPECT_NEAR(h[0], 40.0, 1e-9);   // 1, 1.5 (of 5 valid values)
	EXPECT_NEAR(h[1], 20.0, 1e-9);   // 2.5
	EXPECT_NEAR(h[4], 40.0, 1e-9);   // 9 and the 100 that is above the range
	EXPECT_NEAR(std::accumulate(h.begin(), h.end(), 0.0), 100.0, 1e-9);
	EXPECT_TRUE(frameTimeHistogram(f, 0, 10).empty());
	EXPECT_TRUE(frameTimeHistogram({}, 1, 10).size() == 10);
}

TEST(CsvReader, UpstreamGoldFilesParse)
{
	namespace fs = std::filesystem;
	int files = 0;
	for (const auto &e : fs::directory_iterator(PML_GOLD_DIR))
	{
		if (e.path().extension() != ".csv")
			continue;
		++files;
		SCOPED_TRACE(e.path().filename().string());
		CaptureData d;
		std::string err;
		ASSERT_TRUE(readCaptureCsv(e.path().string(), &d, &err)) << err;
		const CaptureStats s = computeCaptureStats(d);
		EXPECT_GT(s.frames, 10u);
		EXPECT_GT(s.avgFps, 1.0);
		EXPECT_LT(s.avgFps, 20000.0);
		EXPECT_GT(s.times.minMs, 0.0);
		EXPECT_LE(s.times.p50Ms, s.times.p99Ms);
		EXPECT_LE(s.times.p99Ms, s.times.maxMs);
		EXPECT_GT(s.durationSec, 0.0);
		// The span of the time axis must agree with the frame times (checks the QPC tick rate).
		EXPECT_NEAR(s.durationSec, double(s.frames) * s.times.avgMs / 1000.0, s.durationSec * 0.25 + 0.1);
		const auto h = frameTimeHistogram(d.frameMs, 1.0, std::max(10.0, s.times.p99Ms * 1.5));
		EXPECT_NEAR(std::accumulate(h.begin(), h.end(), 0.0), 100.0, 1e-6);
		EXPECT_FALSE(d.application.empty());
	}
	EXPECT_GE(files, 18);
}

// ---- CaptureAnalysis ----

TEST(CaptureAnalysis, LoadsAsynchronouslyAndExposesNumbers)
{
	QTemporaryDir dir;
	ASSERT_TRUE(dir.isValid());
	const QString path = dir.filePath("a.csv");
	writeFile(path, makeCsv(kDefault, sampleFrames()));

	CaptureAnalysis a;
	EXPECT_FALSE(a.loaded());
	EXPECT_TRUE(std::isnan(a.avgMs()));
	QSignalSpy loadedSpy(&a, &CaptureAnalysis::loadedChanged);
	a.setPath(path);
	EXPECT_TRUE(a.loading());
	EXPECT_FALSE(a.loaded());
	ASSERT_TRUE(a.waitForLoaded());
	EXPECT_FALSE(a.loading());
	EXPECT_TRUE(a.error().isEmpty());
	EXPECT_GE(loadedSpy.count(), 1);

	EXPECT_EQ(a.frames(), 1000);
	EXPECT_NEAR(a.avgMs(), 10.4, 1e-6);
	EXPECT_NEAR(a.avgFps(), 1000.0 / 10.4, 1e-6);
	EXPECT_NEAR(a.low1Fps(), 20.0, 1e-6);
	EXPECT_NEAR(a.p99Ms(), 10.0, 1e-9);
	EXPECT_EQ(a.application(), "Game.exe");
	EXPECT_EQ(a.runtime(), "DXVK");
	EXPECT_EQ(a.frameColumn(), "MsBetweenPresents");
	EXPECT_TRUE(a.hasDisplay());
	EXPECT_EQ(a.frameTimes()->count(), 1000);
	EXPECT_EQ(a.displayTimes()->count(), 1000);
	EXPECT_NEAR(a.frameTimes()->maxY(), 50.0, 1e-4);

	// Histogram: 0 .. max(p99 * 1.5, p50 * 2) = 20 ms in round 0.25 ms bins (80 of them); the
	// 50 ms frames land in the last bin.
	const QList<double> h = a.histogram();
	EXPECT_EQ(h.size(), 80);   // p50 * 2 = 20 ms > p99 * 1.5 = 15 ms: the wider range wins
	EXPECT_NEAR(a.histogramBinMs(), 0.25, 1e-9);
	EXPECT_NEAR(std::accumulate(h.begin(), h.end(), 0.0), 100.0, 1e-6);
	EXPECT_NEAR(h[40], 99.0, 1e-6);   // the 10 ms frames
	EXPECT_NEAR(h.last(), 1.0, 1e-6);
}

TEST(CaptureAnalysis, ErrorAndClearing)
{
	QTemporaryDir dir;
	ASSERT_TRUE(dir.isValid());
	CaptureAnalysis a;
	a.setPath(dir.filePath("missing.csv"));
	EXPECT_FALSE(a.waitForLoaded());
	EXPECT_FALSE(a.loaded());
	EXPECT_FALSE(a.error().isEmpty());
	EXPECT_TRUE(std::isnan(a.avgFps()));
	EXPECT_EQ(a.frames(), 0);

	const QString good = dir.filePath("good.csv");
	writeFile(good, makeCsv(kV1, std::vector<double>(100, 10.0)));
	a.setPath(good);
	ASSERT_TRUE(a.waitForLoaded());
	EXPECT_TRUE(a.error().isEmpty());
	a.setPath(QString());
	EXPECT_FALSE(a.loaded());
	EXPECT_FALSE(a.loading());
	EXPECT_EQ(a.frameTimes()->count(), 0);
}

TEST(CaptureAnalysis, NewerPathWins)
{
	QTemporaryDir dir;
	ASSERT_TRUE(dir.isValid());
	// A big file that takes a while, then a small one set right after: only the second may show.
	const QString big = dir.filePath("big.csv"), small = dir.filePath("small.csv");
	writeFile(big, makeCsv(kDefault, std::vector<double>(200000, 10.0)));
	writeFile(small, makeCsv(kDefault, std::vector<double>(50, 20.0)));

	CaptureAnalysis a;
	a.setPath(big);
	a.setPath(small);
	ASSERT_TRUE(a.waitForLoaded());
	EXPECT_EQ(a.frames(), 50);
	// Let the older load finish too, and check it did not overwrite the newer result.
	pmlguitest::waitUntil([] { return false; }, 400);
	EXPECT_EQ(a.frames(), 50);
	EXPECT_EQ(a.path(), small);
	EXPECT_NEAR(a.avgMs(), 20.0, 1e-6);
}

// ---- CaptureLibrary ----

TEST(CaptureLibrary, SortsNewestFirstAndPicksUpNewFiles)
{
	QTemporaryDir dir;
	ASSERT_TRUE(dir.isValid());
	const QDateTime base(QDate(2025, 3, 4), QTime(12, 0, 0));
	auto name = [&](const QString &exe, int minutes) { return captureFileName(exe, base.addSecs(minutes * 60)); };
	writeFile(dir.filePath(name("old.exe", 0)), "x\n");
	writeFile(dir.filePath(name("new.exe", 30)), "xx\n");
	writeFile(dir.filePath("custom.csv"), "xxx\n");   // not a pmcap name: sorted by mtime (now: newest)
	writeFile(dir.filePath("notes.txt"), "no");
	QDir(dir.path()).mkdir("sub.csv");                  // a folder, not a file

	CaptureLibrary lib;
	QSignalSpy countSpy(&lib, &CaptureLibrary::countChanged);
	lib.setFolder(dir.path());
	ASSERT_EQ(lib.count(), 3);
	EXPECT_EQ(lib.get(0).value("fileName").toString(), "custom.csv");
	EXPECT_EQ(lib.get(0).value("processName").toString(), "");
	EXPECT_EQ(lib.get(1).value("processName").toString(), "new.exe");
	EXPECT_EQ(lib.get(1).value("dateTime").toDateTime(), base.addSecs(30 * 60));
	EXPECT_EQ(lib.get(2).value("processName").toString(), "old.exe");
	EXPECT_EQ(lib.get(2).value("size").toLongLong(), 2);
	EXPECT_EQ(lib.indexOfPath(dir.filePath(name("old.exe", 0))), 2);
	EXPECT_EQ(lib.indexOfPath(dir.filePath("nothing.csv")), -1);
	EXPECT_EQ(lib.data(lib.index(1), CaptureLibrary::PathRole).toString(), dir.filePath(name("new.exe", 30)));
	EXPECT_TRUE(lib.get(7).isEmpty());
	EXPECT_EQ(countSpy.count(), 1);

	// The watcher notices a new file (after its 300 ms debounce), inserting a row without a reset.
	QSignalSpy resetSpy(&lib, &QAbstractItemModel::modelReset);
	QSignalSpy insertSpy(&lib, &QAbstractItemModel::rowsInserted);
	writeFile(dir.filePath(name("newest.exe", 60)), "y\n");
	ASSERT_TRUE(pmlguitest::waitUntil([&] { return lib.count() == 4; }, 5000));
	EXPECT_EQ(lib.get(0).value("fileName").toString(), "custom.csv");   // 2025 names sort below today's mtime
	EXPECT_EQ(lib.get(1).value("processName").toString(), "newest.exe");
	EXPECT_EQ(resetSpy.count(), 0);
	EXPECT_GE(insertSpy.count(), 1);

	// A file that grows after the scan (a running capture) updates its size row.
	const int row = lib.indexOfPath(dir.filePath("custom.csv"));
	ASSERT_GE(row, 0);
	QSignalSpy changed(&lib, &QAbstractItemModel::dataChanged);
	{
		QFile f(dir.filePath("custom.csv"));
		ASSERT_TRUE(f.open(QIODevice::Append));
		f.write(std::string(500, 'z').c_str());
	}
	ASSERT_TRUE(pmlguitest::waitUntil([&] { return lib.get(row).value("size").toLongLong() == 504; }, 5000));
	EXPECT_GE(changed.count(), 1);

	// And a deleted one.
	QFile::remove(dir.filePath("custom.csv"));
	ASSERT_TRUE(pmlguitest::waitUntil([&] { return lib.count() == 3; }, 5000));
	EXPECT_EQ(lib.indexOfPath(dir.filePath("custom.csv")), -1);
}

TEST(CaptureLibrary, FolderThatAppearsLater)
{
	QTemporaryDir dir;
	ASSERT_TRUE(dir.isValid());
	CaptureLibrary lib;
	lib.setFolder(dir.filePath("later/PresentMon"));
	EXPECT_EQ(lib.count(), 0);
	QDir().mkpath(dir.filePath("later/PresentMon"));
	writeFile(dir.filePath("later/PresentMon/a.csv"), "x\n");
	lib.refresh();
	EXPECT_EQ(lib.count(), 1);
}

TEST(CaptureLibrary, MoveToTrashUsesOnlyTheTestTrash)
{
	QStandardPaths::setTestModeEnabled(true);   // the trash lives in ~/.qttest, never the user's real one
	struct Reset
	{
		~Reset() { QStandardPaths::setTestModeEnabled(false); }
	} reset;
	// Same file system as the test-mode trash, so Qt does not create a $topdir/.Trash-<uid> elsewhere.
	const QString dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
	ASSERT_TRUE(dataHome.contains(".qttest")) << dataHome.toStdString();
	QDir().mkpath(dataHome);
	QTemporaryDir dir(dataHome + "/pml-capture-test-XXXXXX");
	ASSERT_TRUE(dir.isValid());

	const QString unique = QString("trashme-%1.csv").arg(QCoreApplication::applicationPid());
	const QString path = dir.filePath(unique);
	writeFile(path, "x\n");
	writeFile(dir.filePath("keep.txt"), "x");

	CaptureLibrary lib;
	lib.setFolder(dir.path());
	ASSERT_EQ(lib.count(), 1);
	EXPECT_FALSE(lib.moveToTrash(dir.filePath("keep.txt")));   // not a capture
	EXPECT_FALSE(lib.moveToTrash(dir.filePath("nothing.csv")));
	EXPECT_TRUE(QFileInfo::exists(dir.filePath("keep.txt")));
	EXPECT_TRUE(lib.moveToTrash(path));
	EXPECT_FALSE(QFileInfo::exists(path));
	EXPECT_EQ(lib.count(), 0);

	// Clean up what went into the test-mode trash.
	QFile::remove(dataHome + "/Trash/files/" + unique);
	QFile::remove(dataHome + "/Trash/info/" + unique + ".trashinfo");
}

// ---- Recorder ----

TEST(Recorder, CommandFor)
{
	Recorder r;
	EXPECT_TRUE(r.folder().endsWith("/PresentMon"));
	EXPECT_TRUE(r.stopWhenProcessExits());
	EXPECT_EQ(r.durationSeconds(), 0);
	EXPECT_FALSE(r.v1Metrics());

	EXPECT_EQ(r.commandFor(42, "/tmp/x.csv"),
	          (QStringList{"--process_id", "42", "--output_file", "/tmp/x.csv", "--no_console_stats", "--terminate_on_proc_exit"}));
	r.setStopWhenProcessExits(false);
	EXPECT_EQ(r.commandFor(42, "/tmp/x.csv"), (QStringList{"--process_id", "42", "--output_file", "/tmp/x.csv", "--no_console_stats"}));
	r.setDurationSeconds(30);
	r.setV1Metrics(true);
	EXPECT_EQ(r.commandFor(7, "f.csv"), (QStringList{"--process_id", "7", "--output_file", "f.csv", "--no_console_stats", "--v1_metrics",
	                                                 "--timed", "30", "--terminate_after_timed"}));
	r.setStopWhenProcessExits(true);
	r.setV1Metrics(false);
	EXPECT_EQ(r.commandFor(7, "f.csv").last(), "--terminate_on_proc_exit");
	EXPECT_TRUE(r.commandFor(7, "f.csv").contains("--timed"));
	EXPECT_FALSE(r.commandFor(7, "f.csv").contains("--v1_metrics"));
	r.setDurationSeconds(-5);
	EXPECT_EQ(r.durationSeconds(), 0);
}

TEST(Recorder, StartFailsWithoutCli)
{
	QTemporaryDir dir;
	Recorder r;
	r.setFolder(dir.path());
	r.setPresentmonPath(dir.filePath("no-such-presentmon"));
	EXPECT_FALSE(r.start(123, "x.exe"));
	EXPECT_FALSE(r.lastError().isEmpty());
	EXPECT_FALSE(r.recording());
}

namespace {

const QString kBinDir = QStringLiteral(PML_BIN_DIR);

// A fake game: pml_fake_producer as a child that publishes `frames` and lingers.
struct FakeGame
{
	QProcess proc;
	bool start(int frames, int lingerMs, int startDelayMs = 0)
	{
		const QString exe = kBinDir + "/pml_fake_producer";
		if (!QFileInfo(exe).isExecutable())
			return false;
		proc.setProgram(exe);
		proc.setArguments({"--frames", QString::number(frames), "--period-us", "2000", "--linger-ms", QString::number(lingerMs),
		                   "--start-delay-ms", QString::number(startDelayMs), "--process-name", "FakeGame.exe"});
		proc.start();
		return proc.waitForStarted(3000);
	}
	int pid() const { return int(proc.processId()); }
	~FakeGame()
	{
		if (proc.state() != QProcess::NotRunning)   // our own child only
		{
			proc.terminate();
			if (!proc.waitForFinished(3000))
			{
				proc.kill();
				proc.waitForFinished(1000);
			}
		}
	}
};

bool haveCli() { return QFileInfo(kBinDir + "/presentmon").isExecutable() && QFileInfo(kBinDir + "/pml_fake_producer").isExecutable(); }

} // namespace

TEST(Recorder, RecordsTheFakeProducerEndToEnd)
{
	if (!haveCli())
		GTEST_SKIP() << "build presentmon and pml_fake_producer first";
	QTemporaryDir dir;
	ASSERT_TRUE(dir.isValid());
	FakeGame game;
	ASSERT_TRUE(game.start(3000, 6000, 300));   // starts publishing after the recorder attached

	Recorder rec;
	rec.setPresentmonPath(kBinDir + "/presentmon");
	rec.setFolder(dir.filePath("caps/PresentMon"));   // created by start()
	rec.setDurationSeconds(1);
	QSignalSpy finished(&rec, &Recorder::finished);
	QSignalSpy progress(&rec, &Recorder::progress);
	ASSERT_TRUE(rec.start(game.pid(), "FakeGame.exe")) << rec.lastError().toStdString();
	EXPECT_TRUE(rec.recording());
	EXPECT_EQ(rec.pid(), game.pid());
	EXPECT_TRUE(rec.outputFile().startsWith(dir.filePath("caps/PresentMon/pmcap-FakeGame.exe-")));
	EXPECT_FALSE(rec.start(game.pid(), "FakeGame.exe"));   // already recording

	ASSERT_TRUE(pmlguitest::waitUntil([&] { return !rec.recording(); }, 15000)) << rec.lastError().toStdString();
	EXPECT_TRUE(rec.lastError().isEmpty()) << rec.lastError().toStdString();
	ASSERT_EQ(finished.count(), 1);
	const QString file = finished.at(0).at(0).toString();
	EXPECT_EQ(file, rec.outputFile());
	EXPECT_EQ(rec.lastFile(), file);
	EXPECT_GT(rec.framesWritten(), 0);
	EXPECT_GE(rec.elapsedSeconds(), 0.9);
	EXPECT_GE(progress.count(), 2);

	CaptureData d;
	std::string err;
	ASSERT_TRUE(readCaptureCsv(file.toStdString(), &d, &err)) << err;
	EXPECT_EQ(qint64(d.frameMs.size()), rec.framesWritten());
	EXPECT_EQ(d.application, "FakeGame.exe");
	EXPECT_EQ(d.frameColumn, "MsBetweenPresents");
	EXPECT_GT(computeCaptureStats(d).avgFps, 10.0);
}

TEST(Recorder, StopBeforeTheEndGivesAValidCsvAndV1Layout)
{
	if (!haveCli())
		GTEST_SKIP() << "build presentmon and pml_fake_producer first";
	QTemporaryDir dir;
	ASSERT_TRUE(dir.isValid());
	FakeGame game;
	ASSERT_TRUE(game.start(5000, 8000, 200));

	Recorder rec;
	rec.setPresentmonPath(kBinDir + "/presentmon");
	rec.setFolder(dir.path());
	rec.setV1Metrics(true);
	QSignalSpy finished(&rec, &Recorder::finished);
	ASSERT_TRUE(rec.start(game.pid(), "FakeGame.exe"));
	ASSERT_TRUE(pmlguitest::waitUntil([&] { return rec.framesWritten() > 50; }, 10000)) << rec.lastError().toStdString();
	rec.stop();
	ASSERT_TRUE(pmlguitest::waitUntil([&] { return !rec.recording(); }, 10000));
	EXPECT_TRUE(rec.lastError().isEmpty()) << rec.lastError().toStdString();
	ASSERT_EQ(finished.count(), 1);

	CaptureData d;
	std::string err;
	ASSERT_TRUE(readCaptureCsv(rec.lastFile().toStdString(), &d, &err)) << err;
	EXPECT_GE(d.frameMs.size(), 50u);
	EXPECT_EQ(d.frameColumn, "msBetweenPresents");   // v1 header
	EXPECT_EQ(d.timeColumn, "TimeInSeconds");
	EXPECT_EQ(qint64(d.frameMs.size()), rec.framesWritten());
	EXPECT_TRUE(rec.stopWhenProcessExits());
}

TEST(Recorder, ProcessThatNeverPresentsGivesNoFramesError)
{
	if (!haveCli())
		GTEST_SKIP() << "build presentmon and pml_fake_producer first";
	QTemporaryDir dir;
	ASSERT_TRUE(dir.isValid());
	FakeGame game;
	ASSERT_TRUE(game.start(0, 6000));   // a segment exists, but no frame is ever published

	Recorder rec;
	rec.setPresentmonPath(kBinDir + "/presentmon");
	rec.setFolder(dir.path());
	rec.setDurationSeconds(1);
	QSignalSpy finished(&rec, &Recorder::finished);
	ASSERT_TRUE(rec.start(game.pid(), "FakeGame.exe"));
	const QString file = rec.outputFile();
	ASSERT_TRUE(pmlguitest::waitUntil([&] { return !rec.recording(); }, 15000));
	EXPECT_EQ(rec.lastError(), "No frames were recorded");
	EXPECT_EQ(finished.count(), 0);
	EXPECT_FALSE(QFileInfo::exists(file));
	EXPECT_TRUE(rec.lastFile().isEmpty());
	EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Files).isEmpty());
}

TEST(Recorder, HeaderOnlyCsvIsRemovedAndReportedAsNoFrames)
{
	QTemporaryDir dir;
	ASSERT_TRUE(dir.isValid());
	// A stand-in CLI that creates the CSV with only its header and exits cleanly.
	const QString script = dir.filePath("presentmon");
	writeFile(script, "#!/bin/sh\necho 'Application,MsBetweenPresents' > \"$4\"\nexit 0\n");
	QFile::setPermissions(script, QFile::permissions(script) | QFileDevice::ExeOwner);

	Recorder rec;
	rec.setPresentmonPath(script);
	rec.setFolder(dir.filePath("caps"));
	QSignalSpy finished(&rec, &Recorder::finished);
	ASSERT_TRUE(rec.start(1, "x.exe"));
	const QString file = rec.outputFile();
	ASSERT_TRUE(pmlguitest::waitUntil([&] { return !rec.recording(); }, 10000));
	EXPECT_EQ(rec.lastError(), "No frames were recorded");
	EXPECT_EQ(finished.count(), 0);
	EXPECT_FALSE(QFileInfo::exists(file));
}

TEST(Recorder, RequestedStopKilledBySigtermIsNotAFailure)
{
	QTemporaryDir dir;
	ASSERT_TRUE(dir.isValid());
	// Writes a row, then dies of SIGTERM like a CLI without a handler (CrashExit, exit code 15).
	const QString script = dir.filePath("presentmon");
	writeFile(script, "#!/bin/sh\nprintf 'Application,MsBetweenPresents\\na.exe,10\\n' > \"$4\"\nexec sleep 30\n");
	QFile::setPermissions(script, QFile::permissions(script) | QFileDevice::ExeOwner);

	Recorder rec;
	rec.setPresentmonPath(script);
	rec.setFolder(dir.filePath("caps"));
	QSignalSpy finished(&rec, &Recorder::finished);
	ASSERT_TRUE(rec.start(1, "x.exe"));
	ASSERT_TRUE(pmlguitest::waitUntil([&] { return rec.framesWritten() >= 1; }, 10000));
	rec.stop();
	ASSERT_TRUE(pmlguitest::waitUntil([&] { return !rec.recording(); }, 10000));
	EXPECT_TRUE(rec.lastError().isEmpty()) << rec.lastError().toStdString();
	EXPECT_EQ(finished.count(), 1);
	EXPECT_TRUE(QFileInfo::exists(rec.lastFile()));
}

TEST(Recorder, CliFailureReportsItsStderr)
{
	QTemporaryDir dir;
	ASSERT_TRUE(dir.isValid());
	// A stand-in CLI that logs like presentmon and fails.
	const QString script = dir.filePath("presentmon");
	writeFile(script, "#!/bin/sh\necho '[presentmon] error: cannot do that' >&2\necho '[presentmon] second line' >&2\nexit 3\n");
	QFile::setPermissions(script, QFile::permissions(script) | QFileDevice::ExeOwner);

	Recorder rec;
	rec.setPresentmonPath(script);
	rec.setFolder(dir.filePath("caps"));
	QSignalSpy finished(&rec, &Recorder::finished);
	ASSERT_TRUE(rec.start(1, "x.exe"));
	ASSERT_TRUE(pmlguitest::waitUntil([&] { return !rec.recording(); }, 10000));
	EXPECT_TRUE(rec.lastError().contains("[presentmon] error: cannot do that")) << rec.lastError().toStdString();
	EXPECT_TRUE(rec.lastError().contains("second line"));
	EXPECT_EQ(finished.count(), 0);
	EXPECT_TRUE(rec.lastFile().isEmpty());
	EXPECT_TRUE(QDir(dir.filePath("caps")).entryList(QDir::Files).isEmpty());

	// A folder that cannot be created is reported by start() itself.
	rec.setFolder("/proc/nope/PresentMon");
	EXPECT_FALSE(rec.start(1, "x.exe"));
	EXPECT_TRUE(rec.lastError().contains("/proc/nope"));
}

TEST(Recorder, DestructorStopsARunningCapture)
{
	if (!haveCli())
		GTEST_SKIP() << "build presentmon and pml_fake_producer first";
	QTemporaryDir dir;
	ASSERT_TRUE(dir.isValid());
	FakeGame game;
	ASSERT_TRUE(game.start(0, 8000));
	QString file;
	{
		Recorder rec;
		rec.setPresentmonPath(kBinDir + "/presentmon");
		rec.setFolder(dir.path());
		ASSERT_TRUE(rec.start(game.pid(), "FakeGame.exe"));
		file = rec.outputFile();
		pmlguitest::waitUntil([] { return false; }, 300);
	}   // ~Recorder: SIGTERM and wait
	EXPECT_FALSE(QFileInfo::exists(file));   // no frames: not kept
}

// ---- LayerStatus ----

namespace {

struct EnvGuard
{
	QByteArray name, old;
	bool had;
	explicit EnvGuard(const char *n, const QByteArray &value) : name(n), old(qgetenv(n)), had(qEnvironmentVariableIsSet(n))
	{
		qputenv(n, value);
	}
	~EnvGuard()
	{
		if (had)
			qputenv(name.constData(), old);
		else
			qunsetenv(name.constData());
	}
};

void writeManifest(const QString &dir, const QString &file, const QString &libraryPath, const QString &layerName = "VK_LAYER_presentmon_linux")
{
	QDir().mkpath(dir);
	QFile f(dir + "/" + file);
	ASSERT_TRUE(f.open(QIODevice::WriteOnly));
	f.write(QString(R"({"file_format_version":"1.2.0","layer":{"name":"%1","type":"GLOBAL","library_path":"%2",
	                     "api_version":"1.3.0","implementation_version":"1","description":"x"}})")
	            .arg(layerName, libraryPath)
	            .toUtf8());
}

} // namespace

TEST(LayerStatus, FindsTheManifestInXdgDataHome)
{
	QTemporaryDir tmp;
	ASSERT_TRUE(tmp.isValid());
	// Everything the loader would search is pointed into the temp dir (except /etc, which we cannot
	// change: the manifests below sit in the directories searched first).
	EnvGuard cfgHome("XDG_CONFIG_HOME", tmp.filePath("cfg").toUtf8());
	EnvGuard cfgDirs("XDG_CONFIG_DIRS", tmp.filePath("cfgdirs").toUtf8());
	EnvGuard dataHome("XDG_DATA_HOME", tmp.filePath("data").toUtf8());
	EnvGuard dataDirs("XDG_DATA_DIRS", tmp.filePath("datadirs").toUtf8());
	const QString dir = tmp.filePath("cfg/vulkan/implicit_layer.d");
	const QString lib = tmp.filePath("libVkLayer_presentmon.so");
	writeFile(lib, "elf");

	// Another layer's manifest and garbage in the same directory are skipped.
	writeManifest(dir, "00_other.json", lib, "VK_LAYER_other");
	writeFile(dir + "/01_broken.json", "{ not json");
	writeManifest(dir, "VkLayer_presentmon.json", lib);

	LayerStatus s;
	EXPECT_TRUE(s.installed());
	EXPECT_EQ(s.manifestPath(), dir + "/VkLayer_presentmon.json");
	EXPECT_EQ(s.libraryPath(), lib);
	EXPECT_TRUE(s.libraryFound());
	EXPECT_EQ(s.shellPrefix(), "PRESENTMON_LAYER=1");
	EXPECT_TRUE(s.steamLaunchOptions().contains("%command%"));
	EXPECT_TRUE(s.nativeLaunchOptions().contains("PRESENTMON_LAYER=1"));

	// Relative library_path: relative to the manifest's directory. A missing library is reported.
	writeManifest(dir, "VkLayer_presentmon.json", "../../../libMissing.so");
	QSignalSpy changed(&s, &LayerStatus::changed);
	s.refresh();
	EXPECT_TRUE(s.installed());
	EXPECT_EQ(s.libraryPath(), tmp.filePath("libMissing.so"));
	EXPECT_FALSE(s.libraryFound());
	EXPECT_EQ(changed.count(), 1);
	writeManifest(dir, "VkLayer_presentmon.json", "../../../libVkLayer_presentmon.so");
	s.refresh();
	EXPECT_TRUE(s.libraryFound());
	EXPECT_EQ(s.libraryPath(), lib);

	// The data home is searched as well.
	QFile::remove(dir + "/VkLayer_presentmon.json");
	writeManifest(tmp.filePath("data/vulkan/implicit_layer.d"), "p.json", lib);
	s.refresh();
	EXPECT_EQ(s.manifestPath(), tmp.filePath("data/vulkan/implicit_layer.d/p.json"));
	EXPECT_TRUE(s.libraryFound());
}

TEST(LayerStatus, NothingInstalledInTheSearchedDirs)
{
	QTemporaryDir tmp;
	ASSERT_TRUE(tmp.isValid());
	EnvGuard cfgHome("XDG_CONFIG_HOME", tmp.filePath("cfg").toUtf8());
	EnvGuard cfgDirs("XDG_CONFIG_DIRS", tmp.filePath("cfgdirs").toUtf8());
	EnvGuard dataHome("XDG_DATA_HOME", tmp.filePath("data").toUtf8());
	EnvGuard dataDirs("XDG_DATA_DIRS", tmp.filePath("datadirs").toUtf8());
	LayerStatus s;
	// /etc is outside the test's control; the temp dirs hold nothing.
	EXPECT_FALSE(s.manifestPath().startsWith(tmp.path()));
	if (!s.installed())
	{
		EXPECT_TRUE(s.manifestPath().isEmpty());
		EXPECT_FALSE(s.libraryFound());
	}
}

TEST(LayerStatus, CopyToClipboard)
{
	LayerStatus s;
	s.copyToClipboard("PRESENTMON_LAYER=1 %command%");
	// The offscreen platform has a (process-local) clipboard.
	EXPECT_EQ(QGuiApplication::clipboard()->text(), "PRESENTMON_LAYER=1 %command%");
}

// ---- Hotkeys ----

TEST(Hotkeys, StaysUnavailableUnderTest)
{
	ASSERT_TRUE(qEnvironmentVariableIsSet("PML_GUI_TEST"));
	Hotkeys h;
	QSignalSpy changed(&h, &Hotkeys::changed);
	EXPECT_FALSE(h.enabled());
	h.setEnabled(true);
	EXPECT_FALSE(h.enabled());
	EXPECT_FALSE(h.available());
	EXPECT_TRUE(h.recordShortcut().isEmpty());
	EXPECT_TRUE(h.overlayShortcut().isEmpty());
	h.setEnabled(false);
	EXPECT_EQ(changed.count(), 0);
}
