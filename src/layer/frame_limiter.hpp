// SPDX-License-Identifier: MIT
// Frame cap: a precise frame limiter for the layer's present hook, and the policy that decides how a cap is
// enforced (docs/ARCHITECTURE.md, 3.1.2; research summary in the same section).
//
// The limiter keeps an absolute deadline grid (DXVK / vkd3d-proton style): deadline k+1 = deadline k + T, so
// oversleeping never accumulates and the long-run rate is exact. It differs where the research pointed to:
//   * Timer slack: Linux delays a normal thread's timer by up to 50 us (its timer slack). The wait sets the
//     calling thread's slack to 1 ns for the sleep and restores it right after (the thread belongs to the app).
//   * Sleep, then spin: clock_nanosleep(TIMER_ABSTIME) until deadline - margin, then PAUSE until the deadline.
//     The margin adapts to the measured wake-up lateness: 1.5 x p99 + 20 us, within [30 us, min(1 ms, T/4)].
//   * No catch-up burst: a frame that comes later than the deadline by more than a small tolerance (wake-up
//     noise) re-anchors the grid at now + T instead of keeping the old grid. DXVK keeps up to one interval of
//     catch-up, which keeps the average rate but makes the next frame short: under VRR that short frame is a
//     refresh-rate jump (what the VRR verdict counts as uneven).
//   * The first frame after (re)anchoring passes without waiting.
// No locks are held while waiting; presents to one swapchain are externally synchronized (Vulkan), so one
// limiter per swapchain needs none.
#pragma once

#include <cstddef>
#include <cstdint>

namespace pml::layer {

// Time source and waiting primitives (tests use a fake).
class WaitClock
{
public:
	virtual ~WaitClock() = default;
	virtual uint64_t now() = 0;                    // CLOCK_MONOTONIC ns
	// May return late (or, for a fake, at any time >= now). False: the thread could not sleep at all (a sandbox
	// refused both clock_nanosleep and nanosleep); the limiter then spins the rest of the interval.
	virtual bool sleepUntil(uint64_t deadlineNs) = 0;
	virtual void relax() = 0;                      // one spin step
};

// CLOCK_MONOTONIC, clock_nanosleep(TIMER_ABSTIME) with the thread's timer slack at 1 ns during the sleep (relative
// nanosleep when the absolute sleep is refused), PAUSE.
WaitClock &systemWaitClock();

struct LimiterStats
{
	uint64_t frames = 0;          // wait() calls with a cap
	uint64_t waits = 0;           // ... that actually waited
	uint64_t reanchors = 0;       // late frames that moved the grid
	uint64_t lastWaitNs = 0;
	double meanWaitNs = 0;        // exponential average over ~1 s of frames
	uint32_t marginNs = 0;        // current spin margin
	uint32_t releaseP99Ns = 0;    // p99 of (release - deadline) over the last 128 waits: the limiter's precision
	uint64_t sleepFailures = 0;   // sleeps that could not happen (spun instead)
};

class FrameLimiter
{
public:
	static constexpr uint64_t kInitialMarginNs = 200'000;
	static constexpr uint64_t kMinMarginNs = 30'000;
	static constexpr uint64_t kMaxMarginNs = 1'000'000;
	static constexpr uint64_t kMaxCatchUpNs = 500'000;   // lateness kept on the grid (min with T/8)
	static constexpr uint64_t kMaxIntervalNs = 1'000'000'000;  // setInterval clamps: never hold a present > 1 s

	explicit FrameLimiter(WaitClock &clock);

	// 0 = no cap. A changed interval re-anchors the grid. Clamped to kMaxIntervalNs.
	void setInterval(uint64_t intervalNs);
	uint64_t interval() const { return intervalNs_; }
	// Re-anchor (swapchain recreated, method changed): the next wait() passes.
	void reset();

	// Holds the calling thread until this frame's deadline. Returns the time waited (ns).
	uint64_t wait();

	LimiterStats stats() const { return stats_; }

private:
	void noteWake(uint64_t sleptUntil, uint64_t wokeAt);
	void noteRelease(uint64_t deadline, uint64_t releasedAt);

	WaitClock &clock_;
	uint64_t intervalNs_ = 0;
	uint64_t nextNs_ = 0;
	bool anchored_ = false;
	uint64_t marginNs_ = kInitialMarginNs;
	static constexpr size_t kSamples = 128;
	uint32_t wakeLate_[kSamples] = {};     // sleep return - requested time
	uint32_t releaseLate_[kSamples] = {};  // release - deadline
	size_t wakeCount_ = 0;
	size_t releaseCount_ = 0;
	LimiterStats stats_;
};

// ---- Policy ---------------------------------------------------------------------------------

// Valid caps (the CLI, the GUI and PRESENTMON_FPS_CAP accept the same range); a control block value outside it
// counts as no cap (Fixed) or no upper bound (Auto).
inline constexpr double kMinCapFps = 1.0;
inline constexpr double kMaxCapFps = 10000.0;

// What the auto cap and the VRR readout know about the game's output (from DRM: CRTC VRR_ENABLED, mode refresh,
// EDID range, the card's driver; pml::pickOutput finds the output).
struct AutoCapInfo
{
	int vrr = 0;               // the game's output: 0 unknown, 1 VRR on, 2 VRR off (or off on every output)
	double refreshHz = 0;      // its mode refresh (the VRR maximum); 0 = unknown, also when the output is
	double vrrMinHz = 0;       // its EDID VRR floor
	bool floorRescan = false;  // pml::rescansAtVrrFloor: the display engine re-scans by itself at the floor
};

// Cap just under the VRR maximum: floor(hz - hz^2/3600) (224 at 240 Hz, 138 at 144, 116 at 120). Community
// estimate of NVIDIA's Reflex + G-SYNC cap (no NVIDIA source; one report of 224 at 240 Hz fits it); 0 below 30 Hz.
double vrrAutoCap(double refreshHz);

// The cap in force for a control setting: Fixed -> fps; Auto -> vrrAutoCap of the VRR output (min with fps when
// set), nothing while VRR is off or unknown. 0 = no cap. fps outside [kMinCapFps, kMaxCapFps] counts as unset.
double effectiveCapFps(uint32_t mode, double fps, const AutoCapInfo &info);

// NV_low_latency2 interval for a cap: ceil(1e6 / fps) us (never faster than the cap); 0 for no cap.
uint32_t capIntervalUs(double fps);
// What the layer sends down: the app's own interval raised to the cap's (a game's own cap stays if lower fps).
inline uint32_t mergedIntervalUs(uint32_t appUs, uint32_t capUs) { return appUs > capUs ? appUs : capUs; }

} // namespace pml::layer
