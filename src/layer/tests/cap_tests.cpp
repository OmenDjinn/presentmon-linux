// SPDX-License-Identifier: MIT
// CapRuntime (frame_cap.hpp): settings from the environment and the segment, the auto cap from the outputs.
#include "frame_cap.hpp"

#include <pml/clock.hpp>
#include <pml/shm.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace pml::layer;

namespace {

pml::DrmOutput output(const char *name, int enabled, double hz, uint32_t w, uint32_t h)
{
	pml::DrmOutput o;
	o.name = name;
	o.capable = true;
	o.enabled = enabled;
	o.refreshHz = hz;
	o.edidMinHz = 48;
	o.edidMaxHz = 500;
	o.width = w;
	o.height = h;
	return o;
}

struct EnvGuard
{
	explicit EnvGuard(std::initializer_list<std::pair<const char *, const char *>> vars)
	{
		for (const char *n : {"PRESENTMON_FPS_CAP", "PRESENTMON_CAP_PLACEMENT", "PRESENTMON_CAP_REFLEX"})
			unsetenv(n);
		for (auto &[n, v] : vars)
			setenv(n, v, 1);
	}
	~EnvGuard()
	{
		for (const char *n : {"PRESENTMON_FPS_CAP", "PRESENTMON_CAP_PLACEMENT", "PRESENTMON_CAP_REFLEX"})
			unsetenv(n);
	}
};

} // namespace

TEST(CapRuntime, ParseCapValue)
{
	uint32_t mode = 9, milli = 9;
	EXPECT_TRUE(CapRuntime::parseCapValue("141", &mode, &milli));
	EXPECT_EQ(mode, uint32_t(pml::CapMode::Fixed));
	EXPECT_EQ(milli, 141000u);
	EXPECT_TRUE(CapRuntime::parseCapValue("59.94", &mode, &milli));
	EXPECT_EQ(milli, 59940u);
	EXPECT_TRUE(CapRuntime::parseCapValue("auto", &mode, &milli));
	EXPECT_EQ(mode, uint32_t(pml::CapMode::Auto));
	EXPECT_TRUE(CapRuntime::parseCapValue("off", &mode, &milli));
	EXPECT_EQ(mode, uint32_t(pml::CapMode::Off));
	for (const char *bad : {"", "fast", "0.5", "141fps", "1e9", "-5"})
		EXPECT_FALSE(CapRuntime::parseCapValue(bad, &mode, &milli)) << bad;
}

TEST(CapRuntime, Environment)
{
	{
		EnvGuard env({{"PRESENTMON_FPS_CAP", "144"}, {"PRESENTMON_CAP_PLACEMENT", "early"}, {"PRESENTMON_CAP_REFLEX", "0"}});
		const pml::CapControl c = CapRuntime::controlFromEnvironment();
		EXPECT_EQ(c.mode, uint32_t(pml::CapMode::Fixed));
		EXPECT_EQ(c.fpsMilli, 144000u);
		EXPECT_EQ(c.placement, uint32_t(pml::CapPlacement::Early));
		EXPECT_TRUE(c.flags & pml::kCapNoReflex);
		CapRuntime rt(nullptr, false);
		const CapDecision d = rt.decide(nullptr, 0, 0);
		EXPECT_EQ(d.fps, 144.0) << "no segment: the environment applies";
		EXPECT_FALSE(d.allowReflex);
		EXPECT_TRUE(rt.everActive());
	}
	{
		EnvGuard env({{"PRESENTMON_FPS_CAP", "nonsense"}});
		EXPECT_EQ(CapRuntime::controlFromEnvironment().mode, uint32_t(pml::CapMode::Off));
		CapRuntime rt(nullptr, false);
		EXPECT_EQ(rt.decide(nullptr, 0, 0).fps, 0.0);
		EXPECT_FALSE(rt.everActive()) << "never capped: presents skip the cap code";
	}
}

TEST(CapRuntime, AutoFollowsTheVrrOutput)
{
	EnvGuard env({{"PRESENTMON_FPS_CAP", "auto"}});
	std::vector<pml::DrmOutput> outputs = {output("DP-1", 0, 120.0, 1440, 2560), output("DP-2", 1, 239.991, 2560, 1440)};
	CapRuntime rt([&] { return outputs; }, false);
	CapDecision d = rt.decide(nullptr, 2560, 1440);
	EXPECT_EQ(d.fps, 0.0) << "outputs not read yet";
	EXPECT_EQ(d.autoInfo.vrr, 0);
	rt.pollOutputs();
	d = rt.decide(nullptr, 2560, 1440);
	EXPECT_EQ(d.autoInfo.vrr, 1);
	EXPECT_NEAR(d.autoInfo.refreshHz, 239.991, 1e-9);
	EXPECT_EQ(d.fps, 223.0);
	// KWin turned VRR off (the game lost focus): no cap, and it says why.
	outputs[1].enabled = 0;
	rt.pollOutputs();
	d = rt.decide(nullptr, 2560, 1440);
	EXPECT_EQ(d.fps, 0.0);
	EXPECT_EQ(d.autoInfo.vrr, 2);
	// No readable output (no libdrm in the game's container): unknown.
	outputs.clear();
	rt.pollOutputs();
	EXPECT_EQ(rt.decide(nullptr, 2560, 1440).autoInfo.vrr, 0);
}

TEST(CapRuntime, VrrInfoForTheOverlay)
{
	// The overlay's VRR readout asks without any cap selected: the same pick as Auto, plus the EDID floor.
	std::vector<pml::DrmOutput> outputs = {output("DP-1", 0, 120.0, 1440, 2560), output("DP-2", 1, 239.991, 2560, 1440)};
	outputs[1].edidMinHz = 48;
	CapRuntime rt([&] { return outputs; }, false);
	EXPECT_EQ(rt.vrrInfo(2560, 1440, 1).vrr, 0) << "outputs not read yet";
	rt.pollOutputs();
	AutoCapInfo info = rt.vrrInfo(2560, 1440, 1);
	EXPECT_EQ(info.vrr, 1);
	EXPECT_NEAR(info.refreshHz, 239.991, 1e-9);
	EXPECT_EQ(info.vrrMinHz, 48.0);
	EXPECT_EQ(rt.decide(nullptr, 2560, 1440).fps, 0.0) << "asking caps nothing";
	outputs[1].enabled = 0;
	rt.pollOutputs();
	EXPECT_EQ(rt.vrrInfo(2560, 1440, 1).vrr, 2);
}

TEST(CapRuntime, PickVrrOutputBySize)
{
	// VRR "Always" on two outputs: the one matching the swapchain wins, then the orientation, then the refresh.
	std::vector<pml::DrmOutput> outputs = {output("DP-1", 1, 120.0, 1440, 2560), output("DP-2", 1, 239.991, 2560, 1440)};
	EXPECT_EQ(pml::pickVrrOutput(outputs, 2560, 1440)->name, "DP-2");
	EXPECT_EQ(pml::pickVrrOutput(outputs, 1440, 2560)->name, "DP-1");
	EXPECT_EQ(pml::pickVrrOutput(outputs, 1920, 1080)->name, "DP-2") << "landscape";
	EXPECT_EQ(pml::pickVrrOutput(outputs, 0, 0)->name, "DP-2") << "unknown size: highest refresh";
	outputs[0].enabled = 0;
	outputs[1].enabled = -1;
	EXPECT_EQ(pml::pickVrrOutput(outputs, 2560, 1440), nullptr);
}

TEST(CapRuntime, SegmentSettingsWinOnceWritten)
{
	EnvGuard env({{"PRESENTMON_FPS_CAP", "100"}});
	const std::string name = "pml-cap-test-" + std::to_string(::getpid());
	pml::ProcessIdentity id;
	id.processName = "cap_tests";
	const pml::CapControl initial = CapRuntime::controlFromEnvironment();
	std::string error;
	auto seg = pml::Segment::create(name, id, 64, {}, &error, &initial);
	ASSERT_TRUE(seg) << error;
	CapRuntime rt(nullptr, false);
	EXPECT_EQ(rt.decide(seg.get(), 0, 0).fps, 100.0) << "seeded from the environment";
	pml::CapControl c = pml::readCapControl(*seg);
	c.mode = uint32_t(pml::CapMode::Fixed);
	c.fpsMilli = 57500;
	c.placement = uint32_t(pml::CapPlacement::Early);
	pml::writeCapControl(*seg, c);
	CapDecision d = rt.decide(seg.get(), 0, 0);
	EXPECT_EQ(d.fps, 57.5);
	EXPECT_EQ(d.placement, uint32_t(pml::CapPlacement::Early));
	c.mode = uint32_t(pml::CapMode::Off);
	pml::writeCapControl(*seg, c);
	EXPECT_EQ(rt.decide(seg.get(), 0, 0).fps, 0.0);
	EXPECT_TRUE(rt.everActive()) << "stays on: a cap that was in force must be undone on later presents";

	// Status: written at most every 250 ms unless forced.
	LimiterStats st;
	st.meanWaitNs = 2'340'000;
	st.releaseP99Ns = 12'000;
	CapDecision on = d;
	rt.report(seg.get(), on, pml::CapMethod::PresentEarly, &st, 1'000'000'000, true);
	pml::CapControl s = pml::readCapControl(*seg);
	EXPECT_EQ(s.method, uint32_t(pml::CapMethod::PresentEarly));
	EXPECT_EQ(s.effectiveMilli, 57500u);
	EXPECT_EQ(s.meanWaitUs, 2340u);
	EXPECT_EQ(s.lateP99Us, 12u);
	EXPECT_EQ(s.appliedGeneration, d.generation) << "the status names the settings it was computed from";
	CapDecision none;                 // nothing in force, same settings
	none.generation = d.generation;
	rt.report(seg.get(), none, pml::CapMethod::None, nullptr, 1'100'000'000);
	EXPECT_EQ(pml::readCapControl(*seg).effectiveMilli, 57500u) << "too soon: not written";
	rt.report(seg.get(), none, pml::CapMethod::None, nullptr, 1'300'000'000);
	EXPECT_EQ(pml::readCapControl(*seg).effectiveMilli, 0u);

	// A settings change is acknowledged on the next report even inside the 250 ms window (`presentmon cap` waits
	// for appliedGeneration), also when nothing is capped (reportIdle).
	c.mode = uint32_t(pml::CapMode::Fixed);
	c.fpsMilli = 80000;
	const uint32_t g1 = pml::writeCapControl(*seg, c);
	// A decision made before the change (another swapchain's present, still on the old settings) does not
	// acknowledge it: its status would describe the old setting.
	rt.report(seg.get(), none, pml::CapMethod::None, nullptr, 1'340'000'000, true);
	d = rt.decide(seg.get(), 0, 0);
	EXPECT_EQ(d.generation, g1);
	EXPECT_NE(pml::readCapControl(*seg).appliedGeneration, g1);
	rt.report(seg.get(), d, pml::CapMethod::PresentLate, &st, 1'350'000'000);
	s = pml::readCapControl(*seg);
	EXPECT_EQ(s.appliedGeneration, g1);
	EXPECT_EQ(s.effectiveMilli, 80000u);
	c.mode = uint32_t(pml::CapMode::Off);
	const uint32_t g2 = pml::writeCapControl(*seg, c);
	rt.decide(seg.get(), 0, 0);
	rt.reportIdle(seg.get(), 1'360'000'000);
	s = pml::readCapControl(*seg);
	EXPECT_EQ(s.appliedGeneration, g2);
	EXPECT_EQ(s.effectiveMilli, 0u);
	EXPECT_EQ(s.method, uint32_t(pml::CapMethod::None));
	rt.reportIdle(seg.get(), 1'370'000'000);   // nothing new: no write
	EXPECT_EQ(pml::readCapControl(*seg).appliedGeneration, g2);

	// A write in progress (odd generation) is not read: the last settings stay until it is complete.
	c.mode = uint32_t(pml::CapMode::Fixed);
	c.fpsMilli = 70000;
	pml::writeCapControl(*seg, c);
	EXPECT_EQ(rt.decide(seg.get(), 0, 0).fps, 70.0);
	const uint32_t g = pml::atomicLoad(seg->header().cap.generation);
	pml::atomicStore(seg->header().cap.generation, g + 1);   // a writer between its stores
	pml::atomicStore(seg->header().cap.fpsMilli, 30000u);
	EXPECT_EQ(rt.decide(seg.get(), 0, 0).fps, 70.0);
	pml::atomicStore(seg->header().cap.generation, g + 2);   // ... done
	EXPECT_EQ(rt.decide(seg.get(), 0, 0).fps, 30.0);

	// Values only a foreign writer can store: below 1 fps or above 10000 is no cap, never an endless wait.
	c.fpsMilli = 10;
	pml::writeCapControl(*seg, c);
	EXPECT_EQ(rt.decide(seg.get(), 0, 0).fps, 0.0);
	c.fpsMilli = 20'000'000;
	pml::writeCapControl(*seg, c);
	EXPECT_EQ(rt.decide(seg.get(), 0, 0).fps, 0.0);
	seg->unlink();
}

TEST(CapRuntime, ForkedChildStartsClean)
{
	// The process's runtime registers fork handlers: a child forked while another thread holds the runtime's locks
	// (here: the parent keeps deciding in a loop) can still decide and poll.
	CapRuntime &rt = CapRuntime::get();
	std::atomic<bool> stop{false};
	std::thread busy([&] {
		while (!stop.load())
		{
			rt.decide(nullptr, 0, 0);
			rt.forgetOutputs();
		}
	});
	for (int i = 0; i < 20; i++)
	{
		const pid_t child = ::fork();
		if (child == 0)
		{
			rt.decide(nullptr, 0, 0);
			rt.forgetOutputs();
			::_exit(0);
		}
		int status = -1;
		ASSERT_EQ(::waitpid(child, &status, 0), child);
		EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
	stop = true;
	busy.join();
}
