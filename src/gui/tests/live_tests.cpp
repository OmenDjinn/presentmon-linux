// SPDX-License-Identifier: MIT
// ProcessModel and LiveSession against a fake producer (test_support.hpp) that owns the segment of
// the test's own pid. Headless, no GPU, no other process involved.
#include "test_support.hpp"

#include "format.hpp"
#include "livesession.hpp"
#include "processmodel.hpp"

#include <stats.hpp>

#include <QSignalSpy>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace pmlguitest;
using pml::layer::overlay::OverlayStats;

namespace {

constexpr uint64_t kMs = 1'000'000ull;

// Runs the event loop for ms while the producer keeps its cadence.
void runFor(FakeProducer &p, int ms)
{
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < ms)
	{
		p.pump();
		QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
	}
}

} // namespace

// ---- ProcessModel -----------------------------------------------------------------------------

TEST(ProcessModel, ListsTheFakeProcessWithItsDescription)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	p.addSwapchain(makeSwapchainInfo(1, 1920, 1080, pml::monotonicNs()));
	p.startCadence(1, 100);

	ProcessModel model;
	model.setPollIntervalMs(100);
	int row = -1;
	ASSERT_TRUE(waitUntil([&] { p.pump(); row = model.indexOfPid(p.pid()); return row >= 0; }));

	const QVariantMap m = model.get(row);
	EXPECT_EQ(m["pid"].toInt(), p.pid());
	EXPECT_EQ(m["name"].toString(), "FakeGame.exe");
	EXPECT_EQ(m["exePath"].toString(), "/opt/games/FakeGame/FakeGame.exe");
	EXPECT_EQ(m["application"].toString(), "Fake Game");
	EXPECT_EQ(m["engine"].toString(), "DXVK");
	EXPECT_EQ(m["layerVersion"].toString(), "fake-0");
	EXPECT_EQ(m["runtime"].toString(), "DXVK");
	EXPECT_EQ(m["swapchain"].toString(), pml::gui::swapchainSummary(makeSwapchainInfo(1, 1920, 1080, 0)));
	EXPECT_EQ(m["swapchainCount"].toInt(), 1);
	EXPECT_EQ(m["width"].toInt(), 1920);
	EXPECT_EQ(m["height"].toInt(), 1080);
	EXPECT_EQ(m["presentMode"].toString(), "FIFO");
	EXPECT_FALSE(m["hdr"].toBool());
	EXPECT_TRUE(m["overlayAvailable"].toBool());
	EXPECT_TRUE(m["overlayVisible"].toBool());
	EXPECT_EQ(m["overlayPosition"].toInt(), 0);
	EXPECT_EQ(m["overlayScale"].toDouble(), 0.0);
	EXPECT_EQ(m["overlayPanels"].toInt(), 0);
	EXPECT_EQ(model.count(), model.rowCount());
	EXPECT_EQ(model.data(model.index(row), ProcessModel::NameRole).toString(), "FakeGame.exe");
}

TEST(ProcessModel, PresentRateFollowsTheCadenceAndIdleTheHeartbeat)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	p.addSwapchain(makeSwapchainInfo(1, 1920, 1080, pml::monotonicNs()));
	p.startCadence(1, 100);

	ProcessModel model;
	model.setPollIntervalMs(100);
	// Roles that did not change must not be reported: the stable fields are never in a dataChanged.
	QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
	QSignalSpy reset(&model, &QAbstractItemModel::modelReset);

	auto rate = [&] { return model.get(model.indexOfPid(p.pid()))["presentRate"].toDouble(); };
	ASSERT_TRUE(waitUntil([&] { p.pump(); return model.indexOfPid(p.pid()) >= 0 && std::abs(rate() - 100.0) < 15.0; }));
	EXPECT_FALSE(model.get(model.indexOfPid(p.pid()))["idle"].toBool());
	EXPECT_NE(model.busiestPid(), 0);   // (another live game may be busier than the fake one)

	runFor(p, 300);
	EXPECT_NEAR(rate(), 100.0, 15.0);
	ASSERT_GT(changed.size(), 0);
	for (const QList<QVariant> &args : changed)
	{
		const QList<int> roles = args.at(2).value<QList<int>>();
		EXPECT_FALSE(roles.isEmpty());
		EXPECT_FALSE(roles.contains(ProcessModel::NameRole));
		EXPECT_FALSE(roles.contains(ProcessModel::SwapchainRole));
	}
	EXPECT_EQ(reset.size(), 0);

	// The game stops presenting: no heartbeat for 1.5 s.
	p.stopCadence();
	ASSERT_TRUE(waitUntil([&] { return model.get(model.indexOfPid(p.pid()))["idle"].toBool(); }, 6000));
	EXPECT_EQ(rate(), 0.0);
	EXPECT_NE(model.busiestPid(), p.pid());
}

TEST(ProcessModel, KeepsTheNewestLiveSwapchainAndCountsLiveOnes)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	const uint64_t now = pml::monotonicNs();
	p.addSwapchain(makeSwapchainInfo(1, 1280, 720, now));
	p.addSwapchain(makeSwapchainInfo(2, 2560, 1440, now + kMs, 0, "vkd3d"));
	ProcessModel model;
	const QVariantMap m = model.get(model.indexOfPid(p.pid()));
	ASSERT_FALSE(m.isEmpty());
	EXPECT_EQ(m["swapchainCount"].toInt(), 2);
	EXPECT_EQ(m["width"].toInt(), 2560);
	EXPECT_EQ(m["runtime"].toString(), pml::gui::runtimeName(makeSwapchainInfo(2, 0, 0, 0, 0, "vkd3d")));

	// The newest one is destroyed: the older live one is shown.
	pml::SwapchainInfo gone = makeSwapchainInfo(2, 2560, 1440, now + kMs, 0, "vkd3d");
	gone.destroyedNs = now + 2 * kMs;
	pml::updateSwapchain(p.segment(), 1, gone);
	model.refresh();
	const QVariantMap m2 = model.get(model.indexOfPid(p.pid()));
	EXPECT_EQ(m2["swapchainCount"].toInt(), 1);
	EXPECT_EQ(m2["width"].toInt(), 1280);
	EXPECT_EQ(m2["runtime"].toString(), "DXVK");
}

TEST(ProcessModel, RemovesTheRowWhenTheSegmentGoesAway)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	p.addSwapchain(makeSwapchainInfo(1, 1920, 1080, pml::monotonicNs()));
	ProcessModel model;
	ASSERT_GE(model.indexOfPid(p.pid()), 0);
	QSignalSpy removed(&model, &QAbstractItemModel::rowsAboutToBeRemoved);
	QSignalSpy countChanged(&model, &ProcessModel::countChanged);
	QSignalSpy reset(&model, &QAbstractItemModel::modelReset);

	p.unlink();
	model.refresh();
	EXPECT_EQ(model.indexOfPid(p.pid()), -1);
	EXPECT_EQ(removed.size(), 1);
	EXPECT_EQ(countChanged.size(), 1);
	EXPECT_EQ(reset.size(), 0);
	EXPECT_EQ(model.count(), model.rowCount());

	// It can come back (a new process image re-creating the name) as a new row.
	p.remove();
	FakeProducer again;
	ASSERT_TRUE(again.ok()) << again.error();
	again.addSwapchain(makeSwapchainInfo(1, 800, 600, pml::monotonicNs()));
	model.refresh();
	ASSERT_GE(model.indexOfPid(again.pid()), 0);
	EXPECT_EQ(model.get(model.indexOfPid(again.pid()))["width"].toInt(), 800);
}

TEST(ProcessModel, DropsAnExitedProducerEvenWhileTheNameExists)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	ProcessModel model;
	ASSERT_GE(model.indexOfPid(p.pid()), 0);
	p.exit();
	model.refresh();
	EXPECT_EQ(model.indexOfPid(p.pid()), -1);
}

TEST(ProcessModel, AppendsInsteadOfResetting)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	ProcessModel model;
	QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
	QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
	const int row = model.indexOfPid(p.pid());
	ASSERT_GE(row, 0);
	model.refresh();
	model.refresh();
	EXPECT_EQ(model.indexOfPid(p.pid()), row);   // a second poll keeps the order and the row
	EXPECT_EQ(inserted.size(), 0);
	EXPECT_EQ(reset.size(), 0);
}

TEST(ProcessModel, ZeroesTheRateOfAnIdleRowAtOnce)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	p.addSwapchain(makeSwapchainInfo(1, 1920, 1080, pml::monotonicNs()));
	ProcessModel model;
	model.setPollIntervalMs(100000);   // only the refresh() calls below sample
	const int pid = p.pid();
	ASSERT_GE(model.indexOfPid(pid), 0);

	// 200 presents arrive, but the heartbeat is older than 1.5 s: the interval's rate would be
	// positive, yet the row is idle and must show 0 on this very refresh.
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < 150)
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
	p.publish(makeStream(1, 1, 200, pml::monotonicNs(), kMs));
	pml::touchHeartbeat(p.segment(), pml::monotonicNs() - 3000 * kMs);
	model.refresh();
	const QVariantMap m = model.get(model.indexOfPid(pid));
	EXPECT_TRUE(m["idle"].toBool());
	EXPECT_EQ(m["presentRate"].toDouble(), 0.0);
}

TEST(ProcessModel, OverlaySettersReachTheProducerSide)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	ProcessModel model;
	const int pid = p.pid();
	ASSERT_GE(model.indexOfPid(pid), 0);
	QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);

	const uint32_t gen0 = pml::readOverlayControl(p.segment()).generation;
	EXPECT_TRUE(model.setOverlayVisible(pid, false));
	pml::OverlayControl c = pml::readOverlayControl(p.segment());
	EXPECT_EQ(c.visible, 0u);
	EXPECT_GT(c.generation, gen0);
	EXPECT_FALSE(model.get(model.indexOfPid(pid))["overlayVisible"].toBool());   // row updated at once
	ASSERT_GT(changed.size(), 0);
	EXPECT_TRUE(changed.last().at(2).value<QList<int>>().contains(ProcessModel::OverlayVisibleRole));

	EXPECT_TRUE(model.setOverlayPosition(pid, int(pml::OverlayPosition::BottomRight)));
	c = pml::readOverlayControl(p.segment());
	EXPECT_EQ(c.position, uint32_t(pml::OverlayPosition::BottomRight));
	EXPECT_EQ(c.visible, 0u);   // read-modify-write: the other fields stay
	EXPECT_EQ(model.get(model.indexOfPid(pid))["overlayPosition"].toInt(), 3);
	EXPECT_FALSE(model.setOverlayPosition(pid, 4));
	EXPECT_FALSE(model.setOverlayPosition(pid, -1));

	EXPECT_TRUE(model.setOverlayScale(pid, 2.5));
	EXPECT_EQ(pml::readOverlayControl(p.segment()).scalePermille, 2500u);
	EXPECT_DOUBLE_EQ(model.get(model.indexOfPid(pid))["overlayScale"].toDouble(), 2.5);
	EXPECT_TRUE(model.setOverlayScale(pid, 0.1));
	EXPECT_EQ(pml::readOverlayControl(p.segment()).scalePermille, 500u);
	EXPECT_TRUE(model.setOverlayScale(pid, 9));
	EXPECT_EQ(pml::readOverlayControl(p.segment()).scalePermille, 4000u);
	EXPECT_TRUE(model.setOverlayScale(pid, 0));
	EXPECT_EQ(pml::readOverlayControl(p.segment()).scalePermille, 0u);
	EXPECT_FALSE(model.setOverlayScale(pid, -1));
	EXPECT_FALSE(model.setOverlayScale(pid, std::nan("")));

	const int panels = int(pml::kPanelFps | pml::kPanelCpu | pml::kPanelDropped);
	EXPECT_TRUE(model.setOverlayPanels(pid, panels));
	EXPECT_EQ(pml::readOverlayControl(p.segment()).panels, uint32_t(panels));
	EXPECT_EQ(model.get(model.indexOfPid(pid))["overlayPanels"].toInt(), panels);
	EXPECT_TRUE(model.setOverlayPanels(pid, 0));
	EXPECT_EQ(pml::readOverlayControl(p.segment()).panels, 0u);
	// Unknown bits are masked off: only the panels the overlay knows reach the shared control block.
	const uint32_t known = pml::kPanelFps | pml::kPanelFrameTimeGraph | pml::kPanelCpu | pml::kPanelGpu |
	                       pml::kPanelLatency | pml::kPanelSwapchain | pml::kPanelDropped | pml::kPanelVrr | pml::kPanelCap;
	EXPECT_TRUE(model.setOverlayPanels(pid, 0x7fffffff));
	EXPECT_EQ(pml::readOverlayControl(p.segment()).panels, known);
	EXPECT_TRUE(model.setOverlayPanels(pid, 0));

	// What the producer wrote itself stays untouched.
	EXPECT_EQ(pml::readOverlayControl(p.segment()).available, 1u);

	// Unknown pid.
	EXPECT_FALSE(model.setOverlayVisible(pid + 1000000, true));
	EXPECT_FALSE(model.setOverlayScale(pid + 1000000, 1));
}

// ---- LiveSession ------------------------------------------------------------------------------

namespace {

// The numbers an OverlayStats gives for the same records, the reference of the session's numbers.
struct Reference
{
	pml::layer::overlay::Snapshot snap;
};

Reference referenceFor(const pml::SwapchainInfo &info, uint64_t attachNs, const std::vector<pml::FrameRecord> &records,
                       uint64_t nowNs)
{
	OverlayStats stats(info, uint32_t(getpid()), attachNs);
	for (const pml::FrameRecord &r : records)
		stats.push(r);
	stats.update(nowNs);
	return Reference{stats.snapshot()};
}

// 120 fps with a slow frame every 25th (a hitch of 30 ms), displayed 8 ms after the present.
std::vector<pml::FrameRecord> jitteryStream(uint64_t serial, uint64_t count, uint64_t startNs)
{
	std::vector<pml::FrameRecord> out;
	uint64_t t = startNs;
	for (uint64_t n = 1; n <= count; ++n)
	{
		t += (n % 25 == 0) ? 30 * kMs : 8'333'333;
		pml::FrameRecord r = makeRecord(serial, n, t, t + 8 * kMs);
		r.gpuStartNs = t - 2 * kMs;           // GPU work from 2 ms before the present call
		r.gpuBusyNs = (n % 3 + 1) * kMs / 2;  // 0.5 to 1.5 ms
		out.push_back(r);
	}
	return out;
}

} // namespace

TEST(LiveSession, NumbersEqualAnOverlayStatsFedTheSameRecords)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	const pml::SwapchainInfo info = makeSwapchainInfo(1, 1920, 1080, pml::monotonicNs(), 4'166'667, "DXVK");
	p.addSwapchain(info);

	LiveSession s;
	s.setRunning(false);   // no timer: the test drives the ticks
	s.setPid(p.pid());
	ASSERT_TRUE(s.attached());
	EXPECT_EQ(s.processName(), "FakeGame.exe");
	EXPECT_TRUE(s.attachError().isEmpty());

	const auto records = jitteryStream(1, 400, s.attachNs());
	p.publish(records);
	const uint64_t now = records.back().presentStartNs + 20 * kMs;
	s.tickForTest(now);

	const Reference ref = referenceFor(info, s.attachNs(), records, now);
	ASSERT_TRUE(ref.snap.valid);
	ASSERT_TRUE(s.valid());
	EXPECT_FALSE(s.idle());
	EXPECT_EQ(s.hasDisplayInfo(), ref.snap.hasDisplayInfo);
	EXPECT_TRUE(s.hasDisplayInfo());
	EXPECT_DOUBLE_EQ(s.presentFps(), ref.snap.presentFps);
	EXPECT_DOUBLE_EQ(s.displayFps(), ref.snap.displayFps);
	EXPECT_DOUBLE_EQ(s.avgMs(), ref.snap.avgMs);
	EXPECT_DOUBLE_EQ(s.p99Ms(), ref.snap.p99Ms);
	EXPECT_DOUBLE_EQ(s.low1Fps(), ref.snap.low1Fps);
	EXPECT_DOUBLE_EQ(s.low01Fps(), ref.snap.low01Fps);
	EXPECT_DOUBLE_EQ(s.cpuBusyMs(), ref.snap.cpuBusyMs);
	EXPECT_DOUBLE_EQ(s.cpuWaitMs(), ref.snap.cpuWaitMs);
	EXPECT_FALSE(std::isnan(s.gpuBusyMs()));
	EXPECT_DOUBLE_EQ(s.gpuBusyMs(), ref.snap.gpuBusyMs);
	EXPECT_DOUBLE_EQ(s.gpuWaitMs(), ref.snap.gpuWaitMs);
	EXPECT_DOUBLE_EQ(s.gpuLatencyMs(), ref.snap.gpuLatencyMs);
	EXPECT_DOUBLE_EQ(s.displayLatencyMs(), ref.snap.displayLatencyMs);
	EXPECT_DOUBLE_EQ(s.untilDisplayedMs(), ref.snap.untilDisplayedMs);
	EXPECT_DOUBLE_EQ(s.readyToScreenMs(), ref.snap.readyToScreenMs);
	EXPECT_DOUBLE_EQ(s.droppedPct(), ref.snap.droppedPct);
	EXPECT_EQ(s.lostRecords(), 0u);

	// And the numbers are sane for that stream (a 30 ms hitch every 25th frame).
	EXPECT_GT(s.presentFps(), 80.0);
	EXPECT_LT(s.presentFps(), 125.0);
	EXPECT_GT(s.p99Ms(), 29.0);

	EXPECT_EQ(s.swapchain(), pml::gui::swapchainSummary(info));
	EXPECT_EQ(s.swapchainCount(), 1);
	EXPECT_NEAR(s.refreshHz(), 240.0, 0.01);
}

TEST(LiveSession, FillsTheSeriesWithFrameAndDisplayTimes)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	p.addSwapchain(makeSwapchainInfo(1, 1920, 1080, pml::monotonicNs()));
	LiveSession s;
	s.setRunning(false);
	s.setHistorySeconds(60);
	s.setPid(p.pid());
	QSignalSpy frameChanged(s.frameTimes(), &FrameSeries::changed);
	QSignalSpy displayChanged(s.displayTimes(), &FrameSeries::changed);

	const uint64_t period = 10 * kMs;
	const uint64_t count = 200;
	const auto records = makeStream(1, 1, count, s.attachNs(), period);
	p.publish(records);
	s.tickForTest(records.back().presentStartNs + 20 * kMs);

	// One notify per series for the whole batch.
	EXPECT_EQ(frameChanged.size(), 1);
	EXPECT_EQ(displayChanged.size(), 1);

	// frameTimes: one point per present with a previous one (the first present has no frame time).
	const QList<QPointF> &ft = s.frameTimes()->points();
	ASSERT_EQ(ft.size(), qsizetype(count - 1));
	for (qsizetype i = 0; i < ft.size(); ++i)
	{
		const double expectedX = double(records[size_t(i) + 1].presentStartNs - s.attachNs()) * 1e-9;
		EXPECT_NEAR(ft[i].x(), expectedX, 1e-9) << i;
		EXPECT_NEAR(ft[i].y(), 10.0, 1e-6) << i;
	}

	// displayTimes: displayed frames, MsBetweenDisplayChange; the newest displayed frame is held back
	// until the next one reaches the screen, and the first has no previous screen time.
	const QList<QPointF> &dt = s.displayTimes()->points();
	ASSERT_GE(dt.size(), qsizetype(count - 3));
	ASSERT_LE(dt.size(), qsizetype(count - 1));
	for (qsizetype i = 1; i < dt.size(); ++i)
	{
		EXPECT_NEAR(dt[i].y(), 10.0, 1e-6) << i;
		EXPECT_GT(dt[i].x(), dt[i - 1].x());
	}
	EXPECT_GE(dt.front().x(), double(records.front().presentStartNs - s.attachNs()) * 1e-9 - 1e-9);
	EXPECT_LE(dt.back().x(), ft.back().x() + 1e-9);

	// A second tick without news changes nothing and notifies nothing.
	s.tickForTest(records.back().presentStartNs + 30 * kMs);
	EXPECT_EQ(frameChanged.size(), 1);
	EXPECT_EQ(displayChanged.size(), 1);
}

TEST(LiveSession, TrimsTheSeriesToHistorySeconds)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	p.addSwapchain(makeSwapchainInfo(1, 1920, 1080, pml::monotonicNs()));
	LiveSession s;
	s.setRunning(false);
	s.setHistorySeconds(1.0);
	s.setPid(p.pid());

	const uint64_t period = 10 * kMs;
	const auto records = makeStream(1, 1, 300, s.attachNs(), period);   // 3 s at 100 fps
	p.publish(records);
	s.tickForTest(records.back().presentStartNs + 20 * kMs);

	const QList<QPointF> &ft = s.frameTimes()->points();
	ASSERT_GT(ft.size(), 90);
	EXPECT_LE(ft.size(), 102);
	EXPECT_LE(ft.back().x() - ft.front().x(), 1.0 + 1e-9);
	EXPECT_NEAR(ft.back().x(), double(records.back().presentStartNs - s.attachNs()) * 1e-9, 1e-9);
	const QList<QPointF> &dt = s.displayTimes()->points();
	ASSERT_GT(dt.size(), 90);
	EXPECT_GE(dt.front().x(), ft.back().x() - 1.0 - 1e-9);

	// A shorter history trims on the next tick that brings data.
	s.setHistorySeconds(0.25);
	const auto more = makeStream(1, 301, 10, s.attachNs(), period);
	p.publish(more);
	s.tickForTest(more.back().presentStartNs + 20 * kMs);
	EXPECT_LE(s.frameTimes()->points().size(), 27);
	EXPECT_LE(s.frameTimes()->maxX() - s.frameTimes()->minX(), 0.25 + 1e-9);
}

TEST(LiveSession, FollowsTheBusierSwapchainWithHysteresis)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	const uint64_t now0 = pml::monotonicNs();
	const pml::SwapchainInfo a = makeSwapchainInfo(1, 1280, 720, now0);
	const pml::SwapchainInfo b = makeSwapchainInfo(2, 1920, 1080, now0 + kMs);
	p.addSwapchain(a);
	p.addSwapchain(b);
	LiveSession s;
	s.setRunning(false);
	s.setHistorySeconds(60);
	s.setPid(p.pid());
	const uint64_t t0 = s.attachNs();

	// Merged in time order, as the layer's ring would be, in 100 ms steps. Phase 1 (2 s): swapchain 1
	// at 100 fps, 2 at 10 fps. Phase 2 (4 s): 2 at 200 fps, 1 at 10 fps.
	uint64_t idx1 = 0, idx2 = 0;
	auto step = [&](uint64_t stepNo, double fps1, double fps2) {
		const uint64_t from = t0 + stepNo * 100 * kMs;
		std::vector<pml::FrameRecord> recs;
		for (uint64_t k = 0; k < uint64_t(fps1 / 10); ++k)
		{
			const uint64_t t = from + k * uint64_t(1e9 / fps1);
			recs.push_back(makeRecord(1, ++idx1, t, t + 8 * kMs));
		}
		for (uint64_t k = 0; k < uint64_t(fps2 / 10); ++k)
		{
			const uint64_t t = from + 1 + k * uint64_t(1e9 / fps2);
			recs.push_back(makeRecord(2, ++idx2, t, t + 8 * kMs));
		}
		std::sort(recs.begin(), recs.end(), [](const auto &x, const auto &y) { return x.presentStartNs < y.presentStartNs; });
		p.publish(recs);
		s.tickForTest(from + 100 * kMs);
	};

	const QString sumA = pml::gui::swapchainSummary(a), sumB = pml::gui::swapchainSummary(b);
	for (uint64_t i = 0; i < 20; ++i)
		step(i, 100, 10);
	EXPECT_EQ(s.swapchain(), sumA);
	EXPECT_EQ(s.swapchainCount(), 2);
	ASSERT_GT(s.frameTimes()->count(), 100);
	EXPECT_NEAR(s.presentFps(), 100.0, 3.0);

	// 0.5 s into phase 2 swapchain 2 leads, but not for long enough yet: no switch, no jump.
	for (uint64_t i = 20; i < 25; ++i)
		step(i, 10, 200);
	EXPECT_EQ(s.swapchain(), sumA);
	EXPECT_GT(s.frameTimes()->count(), 100);

	// After a few seconds it has taken over and the series was cleared at the switch.
	int countAtSwitch = -1;
	for (uint64_t i = 25; i < 60; ++i)
	{
		step(i, 10, 200);
		if (countAtSwitch < 0 && s.swapchain() == sumB)
			countAtSwitch = s.frameTimes()->count();
	}
	EXPECT_EQ(s.swapchain(), sumB);
	ASSERT_GE(countAtSwitch, 0);
	EXPECT_LT(countAtSwitch, 60);   // cleared, then only what came in during the tick of the switch
	EXPECT_NEAR(s.presentFps(), 200.0, 5.0);
	for (const QPointF &pt : s.frameTimes()->points())
		EXPECT_NEAR(pt.y(), 5.0, 0.01);   // only swapchain 2's frame times remain
}

TEST(LiveSession, DoesNotFlipForARivalWithOnlyAFewTimesMore)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	const uint64_t now0 = pml::monotonicNs();
	const pml::SwapchainInfo a = makeSwapchainInfo(1, 1280, 720, now0);
	const pml::SwapchainInfo b = makeSwapchainInfo(2, 1920, 1080, now0 + kMs);
	p.addSwapchain(a);
	p.addSwapchain(b);
	LiveSession s;
	s.setRunning(false);
	s.setPid(p.pid());
	const uint64_t t0 = s.attachNs();

	// 100 fps against 120 fps (1.2x) for 5 s: below 1.5x + 2, so swapchain 1 stays.
	uint64_t i1 = 0, i2 = 0;
	for (uint64_t stepNo = 0; stepNo < 50; ++stepNo)
	{
		const uint64_t from = t0 + stepNo * 100 * kMs;
		std::vector<pml::FrameRecord> recs;
		for (uint64_t k = 0; k < 10; ++k)
		{
			const uint64_t t = from + k * 10 * kMs;
			recs.push_back(makeRecord(1, ++i1, t, t + 8 * kMs));
		}
		for (uint64_t k = 0; k < (stepNo < 10 ? 1u : 12u); ++k)   // first a quiet rival, so 1 is followed
		{
			const uint64_t t = from + 1 + k * 8'333'333;
			recs.push_back(makeRecord(2, ++i2, t, t + 8 * kMs));
		}
		std::sort(recs.begin(), recs.end(), [](const auto &x, const auto &y) { return x.presentStartNs < y.presentStartNs; });
		p.publish(recs);
		s.tickForTest(from + 100 * kMs);
		EXPECT_EQ(s.swapchain(), pml::gui::swapchainSummary(a)) << "step " << stepNo;
	}
}

TEST(LiveSession, ReplacesADestroyedFollowedSwapchainAtOnce)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	const uint64_t now0 = pml::monotonicNs();
	const pml::SwapchainInfo a = makeSwapchainInfo(1, 1280, 720, now0);
	const pml::SwapchainInfo b = makeSwapchainInfo(2, 1920, 1080, now0 + 2000 * kMs);
	const uint32_t slotA = p.addSwapchain(a);
	LiveSession s;
	s.setRunning(false);
	s.setPid(p.pid());
	const uint64_t t0 = s.attachNs();

	// Swapchain 1 at 100 fps for 1 s.
	auto recs = makeStream(1, 1, 100, t0, 10 * kMs);
	p.publish(recs);
	s.tickForTest(t0 + 1000 * kMs);
	ASSERT_EQ(s.swapchain(), pml::gui::swapchainSummary(a));

	// A resize: 1 is destroyed, 2 is created and presents a few frames. The table is rescanned within 500 ms.
	p.destroySwapchain(slotA, a, t0 + 1000 * kMs);
	p.addSwapchain(b);
	std::vector<pml::FrameRecord> next;
	for (uint64_t n = 1; n <= 5; ++n)
	{
		const uint64_t t = t0 + 1000 * kMs + n * 10 * kMs;
		next.push_back(makeRecord(2, n, t, t + 8 * kMs));
	}
	p.publish(next);
	s.tickForTest(t0 + 1600 * kMs);   // past the next table scan (every 500 ms), far short of the 1 s dwell
	EXPECT_EQ(s.swapchain(), pml::gui::swapchainSummary(b));
	EXPECT_EQ(s.swapchainCount(), 1);
	EXPECT_EQ(s.frameTimes()->count(), 4);   // cleared at the switch, 5 presents give 4 intervals
}

TEST(LiveSession, ForgetsOldDestroyedSwapchains)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	const uint64_t now0 = pml::monotonicNs();
	const pml::SwapchainInfo a = makeSwapchainInfo(1, 1280, 720, now0);
	const pml::SwapchainInfo b = makeSwapchainInfo(2, 1920, 1080, now0 + kMs);
	const uint32_t slotA = p.addSwapchain(a);
	p.addSwapchain(b);
	LiveSession s;
	s.setRunning(false);
	s.setHistorySeconds(60);
	s.setPid(p.pid());
	const uint64_t t0 = s.attachNs();

	p.publish(makeStream(1, 1, 50, t0, 10 * kMs));
	s.tickForTest(t0 + 600 * kMs);
	p.destroySwapchain(slotA, a, t0 + 600 * kMs);
	// 20 s later swapchain 2 is the only one that presents; nothing breaks while 1 is dropped.
	p.publish(makeStream(2, 1, 100, t0 + 20'000 * kMs, 10 * kMs));
	s.tickForTest(t0 + 21'500 * kMs);
	s.tickForTest(t0 + 22'000 * kMs);
	EXPECT_EQ(s.swapchain(), pml::gui::swapchainSummary(b));
	EXPECT_EQ(s.swapchainCount(), 1);
	EXPECT_TRUE(s.valid());
}

TEST(LiveSession, ReadsTheWholeBacklogWithoutLosingItToTheStatsQueue)
{
	FakeProducer p;   // 8192 slots
	ASSERT_TRUE(p.ok()) << p.error();
	p.addSwapchain(makeSwapchainInfo(1, 1920, 1080, pml::monotonicNs()));
	const uint64_t start = pml::monotonicNs() - 20'000 * kMs;
	const auto records = makeStream(1, 1, 8192, start, kMs);   // 1 kHz, the ring is exactly full
	p.publish(records);

	LiveSession s;
	s.setRunning(false);
	s.setHistorySeconds(60);
	s.setPid(p.pid());
	s.tickForTest(records.back().presentStartNs + 20 * kMs);
	EXPECT_GT(s.frameTimes()->count(), 8000);   // not cut at OverlayStats' 4096 queue
	EXPECT_EQ(s.lostRecords(), 0u);
	EXPECT_TRUE(s.valid());
	EXPECT_NEAR(s.presentFps(), 1000.0, 20.0);
}

TEST(LiveSession, ClearsOnPidChangeAndReportsMissingProcess)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	p.addSwapchain(makeSwapchainInfo(1, 1920, 1080, pml::monotonicNs()));
	LiveSession s;
	s.setRunning(false);
	s.setPid(p.pid());
	const auto records = makeStream(1, 1, 100, s.attachNs(), 10 * kMs);
	p.publish(records);
	s.tickForTest(records.back().presentStartNs + 20 * kMs);
	ASSERT_TRUE(s.valid());
	ASSERT_GT(s.frameTimes()->count(), 0);
	QSignalSpy pidSpy(&s, &LiveSession::pidChanged);
	QSignalSpy attachedSpy(&s, &LiveSession::attachedChanged);
	QSignalSpy statsSpy(&s, &LiveSession::statsChanged);

	// A pid nobody has a segment for.
	const int missing = 0x7ffffff0;
	s.setPid(missing);
	EXPECT_EQ(pidSpy.size(), 1);
	EXPECT_GE(attachedSpy.size(), 1);
	EXPECT_GE(statsSpy.size(), 1);
	EXPECT_FALSE(s.attached());
	EXPECT_FALSE(s.attachError().isEmpty());
	EXPECT_TRUE(s.processName().isEmpty());
	EXPECT_FALSE(s.valid());
	EXPECT_TRUE(std::isnan(s.presentFps()));
	EXPECT_TRUE(std::isnan(s.avgMs()));
	EXPECT_EQ(s.frameTimes()->count(), 0);
	EXPECT_EQ(s.displayTimes()->count(), 0);
	EXPECT_TRUE(s.swapchain().isEmpty());
	EXPECT_EQ(s.swapchainCount(), 0);
	EXPECT_TRUE(std::isnan(s.refreshHz()));

	// Back to the fake process: attaches again, reads the ring from the oldest record.
	s.setPid(p.pid());
	EXPECT_TRUE(s.attached());
	EXPECT_TRUE(s.attachError().isEmpty());
	s.tickForTest(records.back().presentStartNs + 20 * kMs);
	EXPECT_TRUE(s.valid());
	EXPECT_GT(s.frameTimes()->count(), 0);

	s.setPid(0);
	EXPECT_FALSE(s.attached());
	EXPECT_TRUE(s.attachError().isEmpty());
	EXPECT_EQ(s.frameTimes()->count(), 0);
	EXPECT_FALSE(s.valid());

	// A failed attach leaves an error text; clearing it must be announced, or QML keeps showing it.
	s.setPid(missing);
	ASSERT_FALSE(s.attachError().isEmpty());
	QSignalSpy errorSpy(&s, &LiveSession::attachedChanged);
	s.setPid(0);
	EXPECT_TRUE(s.attachError().isEmpty());
	EXPECT_GE(errorSpy.size(), 1);
}

TEST(LiveSession, RetriesAttachWhileThePidStaysSet)
{
	LiveSession s;
	s.setRunning(false);
	s.setPid(getpid());   // the fake process does not exist yet
	EXPECT_FALSE(s.attached());
	EXPECT_FALSE(s.attachError().isEmpty());

	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	p.addSwapchain(makeSwapchainInfo(1, 1920, 1080, pml::monotonicNs()));
	s.tickForTest(pml::monotonicNs());   // too early for the retry (500 ms)
	EXPECT_FALSE(s.attached());
	QSignalSpy attachedSpy(&s, &LiveSession::attachedChanged);
	s.tickForTest(pml::monotonicNs() + 600 * kMs);
	EXPECT_TRUE(s.attached());
	EXPECT_TRUE(s.attachError().isEmpty());
	EXPECT_GE(attachedSpy.size(), 1);
	EXPECT_EQ(s.processName(), "FakeGame.exe");
}

TEST(LiveSession, ReportsLostRecordsAndExit)
{
	FakeProducer p(256);
	ASSERT_TRUE(p.ok()) << p.error();
	p.addSwapchain(makeSwapchainInfo(1, 1920, 1080, pml::monotonicNs()));
	LiveSession s;
	s.setRunning(false);
	s.setPid(p.pid());

	// The reader is not called while 1000 records go through a 256 slot ring.
	const auto records = makeStream(1, 1, 1000, s.attachNs(), 5 * kMs);
	p.publish(records);
	const uint64_t now = records.back().presentStartNs + 20 * kMs;
	s.tickForTest(now);
	EXPECT_EQ(s.lostRecords(), 1000u - 256u);
	EXPECT_TRUE(s.valid());
	EXPECT_FALSE(s.idle());
	const double fps = s.presentFps();
	const double avg = s.avgMs();
	EXPECT_NEAR(fps, 200.0, 1.0);

	// The producer exits: idle, and the numbers stay.
	p.exit();
	s.tickForTest(now + 30 * kMs);
	EXPECT_TRUE(s.idle());
	EXPECT_DOUBLE_EQ(s.presentFps(), fps);
	EXPECT_DOUBLE_EQ(s.avgMs(), avg);
	EXPECT_TRUE(s.attached());
}

TEST(LiveSession, DoesNotCountTheHistoryBeforeTheAttachAsLost)
{
	FakeProducer p(256);
	ASSERT_TRUE(p.ok()) << p.error();
	p.addSwapchain(makeSwapchainInfo(1, 1920, 1080, pml::monotonicNs()));
	const uint64_t start = pml::monotonicNs() - 10'000 * kMs;
	p.publish(makeStream(1, 1, 1000, start, 5 * kMs));   // the ring wrapped long before the attach

	LiveSession s;
	s.setRunning(false);
	s.setPid(p.pid());
	s.tickForTest(start + 1000 * 5 * kMs + 20 * kMs);
	EXPECT_EQ(s.lostRecords(), 0u);

	// What is overwritten after the attach is lost.
	p.publish(makeStream(1, 1001, 1000, start, 5 * kMs));
	s.tickForTest(start + 2000 * 5 * kMs + 20 * kMs);
	EXPECT_EQ(s.lostRecords(), 1000u - 256u);
}

TEST(LiveSession, GoesIdleWhenThePresentsStop)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	p.addSwapchain(makeSwapchainInfo(1, 1920, 1080, pml::monotonicNs()));
	LiveSession s;
	s.setRunning(false);
	s.setPid(p.pid());
	const auto records = makeStream(1, 1, 100, s.attachNs(), 10 * kMs);
	p.publish(records);
	const uint64_t last = records.back().presentStartNs;
	s.tickForTest(last + 20 * kMs);
	EXPECT_FALSE(s.idle());
	s.tickForTest(last + 3000 * kMs);
	EXPECT_TRUE(s.idle());
	EXPECT_TRUE(s.valid());
}

TEST(LiveSession, TimerDrivenAttachFollowsARealtimeProducer)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	p.addSwapchain(makeSwapchainInfo(1, 1920, 1080, pml::monotonicNs()));
	p.startCadence(1, 120);

	LiveSession s;
	s.setHistorySeconds(5);
	s.setPid(p.pid());   // running by default: the 60 Hz timer does the ticks
	ASSERT_TRUE(waitUntil([&] { p.pump(); return s.valid() && s.frameTimes()->count() > 60; }));
	EXPECT_NEAR(s.presentFps(), 120.0, 15.0);
	EXPECT_NEAR(s.avgMs(), 1000.0 / 120.0, 1.5);

	// Stopped (page hidden): nothing is read any more.
	s.setRunning(false);
	runFor(p, 200);
	const int frozen = s.frameTimes()->count();
	runFor(p, 200);
	EXPECT_EQ(s.frameTimes()->count(), frozen);
	s.setRunning(true);
	ASSERT_TRUE(waitUntil([&] { p.pump(); return s.frameTimes()->count() != frozen; }));
}

// ---------------------------------------------------------------------------------------
// VRR verdict: the outputs' driver state (drmvrr.hpp) and the session's numbers
// ---------------------------------------------------------------------------------------

TEST(DrmVrr, EdidRangeDescriptor)
{
	std::vector<uint8_t> edid(128, 0);
	const uint8_t range[18] = {0, 0, 0, 0xFD, 0x02, 48, 245, 30, 255, 0};   // max 245 + 255 (flag bit 1)
	std::copy(std::begin(range), std::end(range), edid.begin() + 72);
	double lo = 0, hi = 0;
	ASSERT_TRUE(pml::gui::edidVerticalRange(edid.data(), edid.size(), &lo, &hi));
	EXPECT_EQ(lo, 48.0);
	EXPECT_EQ(hi, 500.0);
	EXPECT_FALSE(pml::gui::edidVerticalRange(edid.data(), 100, &lo, &hi)) << "truncated";
	std::vector<uint8_t> none(128, 0);
	EXPECT_FALSE(pml::gui::edidVerticalRange(none.data(), none.size(), &lo, &hi));
}

TEST(DrmVrr, TheOutputWithVrrOnIsTheGames)
{
	using pml::gui::OutputVrr;
	const OutputVrr dp1{"DP-1", true, 0, 120.0, 48, 180};
	OutputVrr dp2{"DP-2", true, 1, 239.991, 48, 500};
	std::string which;
	auto ctx = pml::gui::vrrContextFromOutputs({dp1, dp2}, &which);
	EXPECT_EQ(ctx.driver, pml::metrics::DriverVrr::On);
	EXPECT_EQ(which, "DP-2");
	EXPECT_NEAR(ctx.refreshHz, 239.991, 1e-9);
	EXPECT_EQ(ctx.vrrMinHz, 48.0);
	dp2.enabled = 0;
	ctx = pml::gui::vrrContextFromOutputs({dp1, dp2}, &which);
	EXPECT_EQ(ctx.driver, pml::metrics::DriverVrr::Off);
	EXPECT_EQ(which, "");
	dp2.enabled = -1;
	OutputVrr unreadable = dp1;
	unreadable.enabled = -1;
	EXPECT_EQ(pml::gui::vrrContextFromOutputs({unreadable, dp2}).driver, pml::metrics::DriverVrr::Unknown);
}

TEST(LiveSession, VrrVerdictWithTheDriversState)
{
	FakeProducer p;
	ASSERT_TRUE(p.ok()) << p.error();
	const pml::SwapchainInfo info = makeSwapchainInfo(1, 2560, 1440, pml::monotonicNs(), 0, "VKD3D");
	p.addSwapchain(info);
	LiveSession s;
	s.setRunning(false);
	s.setOutputSourceForTest([] {
		return std::vector<pml::gui::OutputVrr>{{"DP-1", true, 0, 120.0, 48, 180}, {"DP-2", true, 1, 240.0, 48, 500}};
	});
	s.setPid(p.pid());
	ASSERT_TRUE(s.attached());
	// Presents every 6 ms; every tenth frame reaches the screen 3 ms late (Lies of P, 2026-11-09).
	std::vector<pml::FrameRecord> recs;
	uint64_t t = s.attachNs();
	for (uint64_t n = 1; n <= 900; ++n)
	{
		t += 6 * kMs;
		recs.push_back(makeRecord(1, n, t, t + (n % 10 == 0 ? 7 : 4) * kMs));
	}
	p.publish(recs);
	s.tickForTest(recs.back().presentStartNs + 20 * kMs);
	EXPECT_EQ(s.vrrState(), "active");
	EXPECT_GE(s.vrrScore(), 0);
	EXPECT_LT(s.vrrScore(), 65);
	EXPECT_EQ(s.vrrCause(), "uneven on screen, game steady");
	EXPECT_TRUE(s.vrrSource().contains("VRR on, DP-2, 48–240 Hz")) << s.vrrSource().toStdString();
	EXPECT_TRUE(s.vrrDetail().contains("on screen only")) << s.vrrDetail().toStdString();
}
