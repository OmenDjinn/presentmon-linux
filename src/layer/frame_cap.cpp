// SPDX-License-Identifier: MIT
#include "frame_cap.hpp"

#include "log.hpp"

#include <pml/clock.hpp>
#include <pml/shm.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

namespace pml::layer {

namespace {

constexpr uint64_t kReportEveryNs = 250'000'000ull;
constexpr auto kPollEvery = std::chrono::milliseconds(500);
constexpr auto kIdleEvery = std::chrono::milliseconds(100);   // Auto not selected: only watch for it
constexpr uint64_t kWantedForNs = 2'000'000'000ull;           // vrrInfo() keeps the outputs read this long

CapRuntime *g_runtime = nullptr;   // the process's runtime, for the fork handlers

uint64_t packSettings(uint32_t mode, uint32_t fpsMilli, uint32_t placement, uint32_t flags)
{
	return uint64_t(fpsMilli) | uint64_t(mode & 0xffu) << 32 | uint64_t(placement & 0xffu) << 40 |
	       uint64_t(flags & 0xffffu) << 48;
}

uint32_t toMilli(double v)
{
	if (!(v > 0.0) || !std::isfinite(v))
		return 0;
	const double m = std::round(v * 1000.0);
	return m >= 4.0e9 ? 0xffffffffu : uint32_t(m);
}

} // namespace

CapRuntime &CapRuntime::get()
{
	static CapRuntime *runtime = [] {
		auto *rt = new CapRuntime([] { return pml::readDrmOutputs(); }, true);
		g_runtime = rt;
		pthread_atfork(&CapRuntime::atforkPrepare, &CapRuntime::atforkParent, &CapRuntime::atforkChild);
		return rt;
	}();
	return *runtime;
}

// fork(): no lock may be copied into the child held by a thread that does not exist there, and the child has no
// output thread (threadStarted_ would keep it from ever starting).
void CapRuntime::atforkPrepare()
{
	if (!g_runtime)
		return;
	g_runtime->settingsMutex_.lock();
	g_runtime->outputsMutex_.lock();
}

void CapRuntime::atforkParent()
{
	if (!g_runtime)
		return;
	g_runtime->outputsMutex_.unlock();
	g_runtime->settingsMutex_.unlock();
}

void CapRuntime::atforkChild()
{
	if (!g_runtime)
		return;
	g_runtime->outputs_.clear();
	g_runtime->outputsRead_ = false;
	g_runtime->threadStarted_.store(false, std::memory_order_relaxed);
	g_runtime->outputsMutex_.unlock();
	g_runtime->settingsMutex_.unlock();
}

CapRuntime::CapRuntime(OutputSource source, bool backgroundThread)
    : source_(std::move(source)), backgroundThread_(backgroundThread)
{
	const pml::CapControl env = controlFromEnvironment();
	settings_.store(packSettings(env.mode, env.fpsMilli, env.placement, env.flags), std::memory_order_relaxed);
}

bool CapRuntime::parseCapValue(const char *text, uint32_t *mode, uint32_t *fpsMilli)
{
	if (!text || !*text)
		return false;
	if (!std::strcmp(text, "off") || !std::strcmp(text, "0"))
	{
		*mode = uint32_t(pml::CapMode::Off);
		*fpsMilli = 0;
		return true;
	}
	if (!std::strcmp(text, "auto"))
	{
		*mode = uint32_t(pml::CapMode::Auto);
		*fpsMilli = 0;
		return true;
	}
	char *end = nullptr;
	const double fps = std::strtod(text, &end);
	if (end == text || *end != '\0' || !(fps >= 1.0) || !(fps <= 10000.0))
		return false;
	*mode = uint32_t(pml::CapMode::Fixed);
	*fpsMilli = toMilli(fps);
	return true;
}

pml::CapControl CapRuntime::controlFromEnvironment()
{
	pml::CapControl c = {};
	uint32_t mode = 0, milli = 0;
	if (const char *v = std::getenv("PRESENTMON_FPS_CAP"))
	{
		if (parseCapValue(v, &mode, &milli))
		{
			c.mode = mode;
			c.fpsMilli = milli;
		}
		else
		{
			PML_LOG(LogLevel::Warn, "PRESENTMON_FPS_CAP=%s is not a frame rate, 'auto' or 'off': no cap", v);
		}
	}
	if (const char *v = std::getenv("PRESENTMON_CAP_PLACEMENT"))
		c.placement = !std::strcmp(v, "early") ? uint32_t(pml::CapPlacement::Early) : uint32_t(pml::CapPlacement::Late);
	if (const char *v = std::getenv("PRESENTMON_CAP_REFLEX"))
		if (!std::strcmp(v, "0") || !std::strcmp(v, "off"))
			c.flags |= pml::kCapNoReflex;
	return c;
}

void CapRuntime::ensureThread()
{
	if (!backgroundThread_ || threadStarted_.exchange(true))
		return;
	try
	{
		std::thread([this] {
			pthread_setname_np(pthread_self(), "pml-cap-drm");
			for (;;)
			{
				const bool wanted = currentMode() == uint32_t(pml::CapMode::Auto) ||
				                    pml::monotonicNs() < outputsWantedUntilNs_.load(std::memory_order_relaxed);
				if (wanted)
					pollOutputs();
				else
					forgetOutputs();
				std::this_thread::sleep_for(wanted ? kPollEvery : kIdleEvery);
			}
		}).detach();
	}
	catch (...)
	{
		PML_LOG_ONCE(LogLevel::Warn, "frame cap: cannot start the output thread; auto cap and the overlay's VRR state unavailable");
	}
}

void CapRuntime::pollOutputs()
{
	std::vector<pml::DrmOutput> outputs;
	try
	{
		outputs = source_ ? source_() : std::vector<pml::DrmOutput>{};
	}
	catch (...)
	{
		outputs.clear();
	}
	std::lock_guard<std::mutex> lock(outputsMutex_);
	outputs_ = std::move(outputs);
	outputsRead_ = true;
}

void CapRuntime::forgetOutputs()
{
	std::lock_guard<std::mutex> lock(outputsMutex_);
	if (!outputsRead_)
		return;
	outputs_.clear();
	outputsRead_ = false;
}

AutoCapInfo CapRuntime::autoInfoFor(uint32_t width, uint32_t height, uint64_t refreshDurationNs)
{
	AutoCapInfo info;
	std::lock_guard<std::mutex> lock(outputsMutex_);
	if (!outputsRead_)
		return info;
	// The game's own output, with VRR on or off: an output of another screen with VRR on (KWin policy "Always")
	// says nothing about this game.
	if (const pml::DrmOutput *o = pml::pickOutput(outputs_, width, height, refreshDurationNs))
	{
		info.vrr = o->enabled == 1 ? 1 : o->enabled == 0 ? 2 : 0;
		info.refreshHz = o->refreshHz;
		info.vrrMinHz = o->edidMinHz;
		info.floorRescan = pml::rescansAtVrrFloor(*o);
		return info;
	}
	// Which output is unknown (several, no refresh cycle from the driver) and none has VRR on.
	bool readable = false;
	for (const pml::DrmOutput &o : outputs_)
		readable = readable || o.enabled >= 0;
	info.vrr = readable ? 2 : 0;
	return info;
}

AutoCapInfo CapRuntime::vrrInfo(uint32_t width, uint32_t height, uint64_t refreshDurationNs, uint64_t nowNs)
{
	outputsWantedUntilNs_.store(nowNs + kWantedForNs, std::memory_order_relaxed);
	ensureThread();
	return autoInfoFor(width, height, refreshDurationNs);
}

CapDecision CapRuntime::decide(pml::Segment *segment, uint32_t width, uint32_t height, uint64_t refreshDurationNs)
{
	if (segment)
	{
		// Odd: a reader is writing the settings right now; the next present picks them up.
		const uint32_t gen = pml::atomicLoad(segment->header().cap.generation, std::memory_order_acquire);
		if (gen != seenGeneration_.load(std::memory_order_acquire) && !(gen & 1u))
		{
			std::lock_guard<std::mutex> lock(settingsMutex_);
			pml::CapControl c = {};
			if (pml::tryReadCapSettings(*segment, &c) && c.generation != seenGeneration_.load(std::memory_order_relaxed))
			{
				const double want = double(c.fpsMilli) / 1000.0;
				const bool outOfRange = !(want >= kMinCapFps && want <= kMaxCapFps);
				if (c.mode == uint32_t(pml::CapMode::Fixed) && outOfRange)
					PML_LOG(LogLevel::Warn, "frame cap: %.3f fps in the control block is outside 1-10000 fps: no cap", want);
				else if (c.mode == uint32_t(pml::CapMode::Auto) && c.fpsMilli && outOfRange)
					PML_LOG(LogLevel::Warn, "frame cap: auto bound %.3f fps is outside 1-10000 fps: ignored", want);
				// Settings first, generation second: a reader that sees the new generation sees the new settings.
				settings_.store(packSettings(c.mode, c.fpsMilli, c.placement, c.flags), std::memory_order_release);
				seenGeneration_.store(c.generation, std::memory_order_release);
			}
		}
	}
	// Generation first: (old generation, new settings) only delays the acknowledgement; never the reverse.
	const uint32_t generation = seenGeneration_.load(std::memory_order_acquire);
	const uint64_t packed = settings_.load(std::memory_order_acquire);
	CapDecision d;
	d.generation = generation;
	d.mode = uint32_t(packed >> 32) & 0xffu;
	d.placement = uint32_t(packed >> 40) & 0xffu;
	d.allowReflex = !((uint32_t(packed >> 48) & 0xffffu) & pml::kCapNoReflex);
	const double fps = double(uint32_t(packed)) / 1000.0;
	if (d.mode == uint32_t(pml::CapMode::Auto))
	{
		ensureThread();
		d.autoInfo = autoInfoFor(width, height, refreshDurationNs);
	}
	d.fps = effectiveCapFps(d.mode, fps, d.autoInfo);
	if (d.fps > 0.0 && !everActive_.load(std::memory_order_relaxed))
		everActive_.store(true, std::memory_order_relaxed);
	// Auto selected: keep the cap code running even while VRR is off (it turns on again).
	if (d.mode != uint32_t(pml::CapMode::Off) && !everActive_.load(std::memory_order_relaxed))
		everActive_.store(true, std::memory_order_relaxed);
	return d;
}

void CapRuntime::report(pml::Segment *segment, const CapDecision &d, pml::CapMethod method, const LimiterStats *stats,
                        uint64_t nowNs, bool force)
{
	if (!segment)
		return;
	const uint32_t gen = d.generation;   // the settings this status was computed from
	const bool fresh = gen != reportedGeneration_.load(std::memory_order_relaxed);
	uint64_t last = lastReportNs_.load(std::memory_order_relaxed);
	if (!force && !fresh && nowNs < last + kReportEveryNs)
		return;
	if (!lastReportNs_.compare_exchange_strong(last, nowNs, std::memory_order_relaxed))
		return;   // another thread reports this round
	pml::CapControl s = {};
	s.appliedGeneration = gen;
	s.available = 1;
	s.method = uint32_t(d.fps > 0.0 ? method : pml::CapMethod::None);
	s.effectiveMilli = toMilli(d.fps);
	s.refreshMilli = d.mode == uint32_t(pml::CapMode::Auto) && d.autoInfo.vrr == 1 ? toMilli(d.autoInfo.refreshHz) : 0;
	s.vrr = d.mode == uint32_t(pml::CapMode::Auto) ? uint32_t(d.autoInfo.vrr) : 0;
	if (stats && method != pml::CapMethod::Reflex && d.fps > 0.0)
	{
		s.meanWaitUs = uint32_t(std::lround(stats->meanWaitNs / 1000.0));
		s.lateP99Us = stats->releaseP99Ns / 1000;
	}
	pml::writeCapStatus(*segment, s);
	reportedGeneration_.store(gen, std::memory_order_relaxed);
}

void CapRuntime::reportIdle(pml::Segment *segment, uint64_t nowNs)
{
	if (!segment || seenGeneration_.load(std::memory_order_acquire) == reportedGeneration_.load(std::memory_order_relaxed))
		return;
	CapDecision none;
	none.generation = seenGeneration_.load(std::memory_order_acquire);
	none.mode = currentMode();
	report(segment, none, pml::CapMethod::None, nullptr, nowNs, true);
}

} // namespace pml::layer
