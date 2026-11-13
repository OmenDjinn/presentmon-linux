// SPDX-License-Identifier: MIT
// Frame cap settings and status inside the layer (one per process):
//   * settings: the environment (PRESENTMON_FPS_CAP, PRESENTMON_CAP_PLACEMENT, PRESENTMON_CAP_REFLEX) until a reader
//     writes the segment's CapControl (CLI "presentmon cap", the GUI); then the segment's, polled with one atomic
//     load per present (a seqlock snapshot when it changed), kept as one packed atomic so a decision never mixes
//     two settings;
//   * the outputs' VRR state: DRM (pml::readDrmOutputs) read on a background thread every 500 ms while Auto is
//     selected or the overlay's VRR readout asked for it in the last 2 s (a few ioctls, kept off the present path;
//     the thread idles without DRM calls otherwise);
//   * the status written back to the segment: at most every 250 ms, and on the first present after a settings
//     change with that change's generation (appliedGeneration), which `presentmon cap` waits for.
// Thread-safe: presents of several swapchains may run on several threads. The process's runtime survives fork():
// its locks are taken around it and the child restarts the output thread when it needs it.
#pragma once

#include "frame_limiter.hpp"

#include <pml/drm_outputs.hpp>
#include <pml/protocol.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace pml {
class Segment;
}

namespace pml::layer {

struct CapDecision
{
	double fps = 0;            // cap in force (0 = none)
	uint32_t mode = 0;         // pml::CapMode
	uint32_t placement = 0;    // pml::CapPlacement
	bool allowReflex = true;   // not kCapNoReflex
	AutoCapInfo autoInfo;      // Auto: what the outputs said
	uint32_t generation = 0;   // the control block generation of these settings (the status acknowledges it)
};

class CapRuntime
{
public:
	using OutputSource = std::function<std::vector<pml::DrmOutput>()>;

	// The process's runtime (leaked: its thread may outlive static destruction).
	static CapRuntime &get();

	// Tests: an own instance; `source` replaces DRM, and no background thread is started (call pollOutputs()).
	explicit CapRuntime(OutputSource source, bool backgroundThread = true);
	CapRuntime(const CapRuntime &) = delete;
	CapRuntime &operator=(const CapRuntime &) = delete;

	// The initial control from the environment (also what applies while there is no segment):
	//   PRESENTMON_FPS_CAP=<fps>|auto|off   PRESENTMON_CAP_PLACEMENT=late|early   PRESENTMON_CAP_REFLEX=0
	static pml::CapControl controlFromEnvironment();
	// Parses one PRESENTMON_FPS_CAP value into mode + fpsMilli; false if it is not one.
	static bool parseCapValue(const char *text, uint32_t *mode, uint32_t *fpsMilli);

	// Per present. `segment` may be null. width/height and refreshDurationNs (the driver's refresh cycle for the
	// swapchain's surface, 0 = unknown): the swapchain's, which find its output for Auto (pml::pickOutput).
	CapDecision decide(pml::Segment *segment, uint32_t width, uint32_t height, uint64_t refreshDurationNs = 0);

	// True once any cap was ever in force: until then presents skip the cap code entirely.
	bool everActive() const { return everActive_.load(std::memory_order_relaxed); }

	// Status for readers: at most every 250 ms, at once after a settings change, or always with force.
	void report(pml::Segment *segment, const CapDecision &decision, pml::CapMethod method, const LimiterStats *stats,
	            uint64_t nowNs, bool force = false);
	// A present that capped nothing: reports "no cap" only to acknowledge a settings change.
	void reportIdle(pml::Segment *segment, uint64_t nowNs);

	// Reads the outputs now (the background thread calls this every 500 ms).
	void pollOutputs();

	// Forgets the outputs (Auto deselected: the next Auto starts from a fresh read, never from a stale VRR state).
	void forgetOutputs();

	// The VRR state of the output a `width` x `height` swapchain with that refresh cycle is on, for the overlay's
	// VRR readout (pml::pickOutput, as Auto): also keeps the output thread reading DRM for the next 2 s. vrr 0
	// (unknown) until the first read and when DRM is unreadable. A short lock; call it a few times per second,
	// not per present.
	AutoCapInfo vrrInfo(uint32_t width, uint32_t height, uint64_t refreshDurationNs, uint64_t nowNs);

private:
	void ensureThread();
	AutoCapInfo autoInfoFor(uint32_t width, uint32_t height, uint64_t refreshDurationNs);
	uint32_t currentMode() const { return uint32_t(settings_.load(std::memory_order_relaxed) >> 32) & 0xffu; }
	static void atforkPrepare();
	static void atforkParent();
	static void atforkChild();

	OutputSource source_;
	bool backgroundThread_;
	std::atomic<bool> threadStarted_{false};
	std::atomic<bool> everActive_{false};

	std::mutex settingsMutex_;   // serializes reloading the settings from the segment
	std::atomic<uint32_t> seenGeneration_{0};
	// fpsMilli | mode << 32 | placement << 40 | flags << 48: one load gives one consistent setting.
	std::atomic<uint64_t> settings_{0};

	std::mutex outputsMutex_;
	std::vector<pml::DrmOutput> outputs_;
	bool outputsRead_ = false;
	std::atomic<uint64_t> outputsWantedUntilNs_{0};   // vrrInfo() keeps the output thread reading until then

	std::atomic<uint64_t> lastReportNs_{0};
	std::atomic<uint32_t> reportedGeneration_{0};
};

} // namespace pml::layer
