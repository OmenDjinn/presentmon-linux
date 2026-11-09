// SPDX-License-Identifier: MIT
#include "frame_limiter.hpp"

#include "pml/clock.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <sys/prctl.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace pml::layer {

namespace {

class SystemWaitClock final : public WaitClock
{
public:
	uint64_t now() override { return pml::monotonicNs(); }

	bool sleepUntil(uint64_t deadlineNs) override
	{
		// The calling thread is the application's (or DXVK's presenter): lower its timer slack only for this
		// sleep. PR_GET_TIMERSLACK returns the slack; it is ignored (and stays) for real-time threads.
		const int old = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
		const bool lowered = old > 1 && prctl(PR_SET_TIMERSLACK, 1, 0, 0, 0) == 0;
		timespec ts;
		ts.tv_sec = time_t(deadlineNs / 1'000'000'000ull);
		ts.tv_nsec = long(deadlineNs % 1'000'000'000ull);
		int r;
		while ((r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr)) == EINTR)
		{
		}
		bool slept = r == 0;
		if (!slept)
		{
			// Refused (a seccomp sandbox: ENOSYS/EPERM): the relative sleep, which Wine itself needs.
			const uint64_t now = pml::monotonicNs();
			if (now >= deadlineNs)
				slept = true;
			else
			{
				const uint64_t rest = deadlineNs - now;
				timespec rel;
				rel.tv_sec = time_t(rest / 1'000'000'000ull);
				rel.tv_nsec = long(rest % 1'000'000'000ull);
				while ((r = nanosleep(&rel, &rel)) != 0 && errno == EINTR)
				{
				}
				slept = r == 0;
			}
		}
		if (lowered)
			prctl(PR_SET_TIMERSLACK, old, 0, 0, 0);
		return slept;
	}

	void relax() override
	{
#if defined(__x86_64__) || defined(__i386__)
		_mm_pause();
#endif
	}
};

uint32_t percentile99(const uint32_t *samples, size_t count)
{
	if (count == 0)
		return 0;
	uint32_t tmp[128];
	const size_t n = std::min<size_t>(count, 128);
	std::copy(samples, samples + n, tmp);
	const size_t k = (n * 99) / 100 < n ? (n * 99) / 100 : n - 1;
	std::nth_element(tmp, tmp + k, tmp + n);
	return tmp[k];
}

uint32_t clampU32(uint64_t v) { return v > 0xffffffffull ? 0xffffffffu : uint32_t(v); }

} // namespace

WaitClock &systemWaitClock()
{
	static SystemWaitClock clock;
	return clock;
}

FrameLimiter::FrameLimiter(WaitClock &clock) : clock_(clock)
{
	stats_.marginNs = uint32_t(marginNs_);
}

void FrameLimiter::setInterval(uint64_t intervalNs)
{
	intervalNs = std::min(intervalNs, kMaxIntervalNs);
	if (intervalNs == intervalNs_)
		return;
	intervalNs_ = intervalNs;
	anchored_ = false;
}

void FrameLimiter::reset()
{
	anchored_ = false;
}

void FrameLimiter::noteWake(uint64_t sleptUntil, uint64_t wokeAt)
{
	wakeLate_[wakeCount_++ % kSamples] = clampU32(wokeAt > sleptUntil ? wokeAt - sleptUntil : 0);
	// Re-derive the margin every 32 sleeps from the recent wake-up lateness.
	if (wakeCount_ % 32 == 0)
	{
		const uint64_t p99 = percentile99(wakeLate_, std::min(wakeCount_, kSamples));
		const uint64_t hi = std::min<uint64_t>(kMaxMarginNs, std::max<uint64_t>(kMinMarginNs, intervalNs_ / 4));
		marginNs_ = std::clamp<uint64_t>(p99 + p99 / 2 + 20'000, kMinMarginNs, hi);
		stats_.marginNs = uint32_t(marginNs_);
	}
}

void FrameLimiter::noteRelease(uint64_t deadline, uint64_t releasedAt)
{
	releaseLate_[releaseCount_++ % kSamples] = clampU32(releasedAt > deadline ? releasedAt - deadline : 0);
	if (releaseCount_ % 32 == 0)
		stats_.releaseP99Ns = percentile99(releaseLate_, std::min(releaseCount_, kSamples));
}

uint64_t FrameLimiter::wait()
{
	if (intervalNs_ == 0)
		return 0;
	stats_.frames++;
	const uint64_t T = intervalNs_;
	uint64_t now = clock_.now();
	if (!anchored_)
	{
		anchored_ = true;
		nextNs_ = now + T;
		stats_.lastWaitNs = 0;
		stats_.meanWaitNs += (0.0 - stats_.meanWaitNs) / 64.0;
		return 0;
	}

	uint64_t waited = 0;
	const uint64_t deadline = nextNs_;
	if (now < deadline)
	{
		const uint64_t start = now;
		const uint64_t margin = std::min<uint64_t>(marginNs_, T / 2);
		if (deadline - now > margin)
		{
			const uint64_t sleepTo = deadline - margin;
			const bool slept = clock_.sleepUntil(sleepTo);
			now = clock_.now();
			if (slept)
				noteWake(sleepTo, now);   // a failed sleep says nothing about wake-up lateness
			else
				stats_.sleepFailures++;
		}
		while (now < deadline)
		{
			clock_.relax();
			now = clock_.now();
		}
		noteRelease(deadline, now);
		waited = now - start;
		stats_.waits++;
	}

	// Stay on the grid when on time (or late by wake-up noise only); a frame that came really late
	// re-anchors instead of catching up with a short frame.
	const uint64_t tolerance = std::min<uint64_t>(kMaxCatchUpNs, T / 8);
	if (now <= deadline + tolerance)
	{
		nextNs_ = deadline + T;
	}
	else
	{
		nextNs_ = now + T;
		stats_.reanchors++;
	}
	stats_.lastWaitNs = waited;
	// ~1 s at typical rates (alpha 1/64).
	stats_.meanWaitNs += (double(waited) - stats_.meanWaitNs) / 64.0;
	return waited;
}

// ---- Policy ---------------------------------------------------------------------------------

double vrrAutoCap(double refreshHz)
{
	if (!(refreshHz >= 30.0) || !std::isfinite(refreshHz))
		return 0.0;
	return std::floor(refreshHz - refreshHz * refreshHz / 3600.0);
}

double effectiveCapFps(uint32_t mode, double fps, const AutoCapInfo &info)
{
	if (!(fps >= kMinCapFps && fps <= kMaxCapFps))
		fps = 0.0;   // also NaN
	switch (mode)
	{
	case 1:   // Fixed
		return fps;
	case 2:   // Auto
	{
		if (info.vrr != 1)
			return 0.0;
		const double cap = vrrAutoCap(info.refreshHz);
		if (cap <= 0.0)
			return 0.0;
		return fps > 0.0 ? std::min(fps, cap) : cap;
	}
	default:
		return 0.0;
	}
}

uint32_t capIntervalUs(double fps)
{
	if (!(fps > 0.0) || !std::isfinite(fps))
		return 0;
	const double us = std::ceil(1.0e6 / fps);
	return us >= 4.0e9 ? 0xffffffffu : uint32_t(us);
}

} // namespace pml::layer
