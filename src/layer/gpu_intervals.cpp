// SPDX-License-Identifier: MIT
#include "gpu_intervals.hpp"

#include <algorithm>
#include <cmath>

namespace pml::layer {

void GpuFrameAttributor::add(GpuInterval interval)
{
	if (interval.endNs == 0)
		return;
	if (interval.startNs == 0 || interval.startNs > interval.endNs)
		interval.startNs = interval.endNs;
	if (pending_.size() >= kMaxPending)
	{
		// Drop the older-ending half at once (amortized O(1) per add): it belongs to frames long gone.
		const auto mid = pending_.begin() + std::ptrdiff_t(pending_.size() / 2);
		std::nth_element(pending_.begin(), mid, pending_.end(),
		                 [](const GpuInterval &a, const GpuInterval &b) { return a.endNs < b.endNs; });
		dropped_ += uint64_t(mid - pending_.begin());
		pending_.erase(pending_.begin(), mid);
	}
	pending_.push_back(interval);
}

GpuFrameWork GpuFrameAttributor::take(uint64_t readyNs, uint64_t presentStartNs)
{
	GpuFrameWork w;
	scratch_.clear();
	const uint64_t floorNs = std::max(lastReadyNs_, readyNs > kMaxFrameSpanNs ? readyNs - kMaxFrameSpanNs : 0);
	size_t keep = 0;
	for (size_t i = 0; i < pending_.size(); i++)
	{
		const GpuInterval &iv = pending_[i];
		if (iv.endNs <= floorNs)
			dropped_++;
		else if (iv.endNs <= readyNs || (iv.endNs <= readyNs + kReadySlackNs && iv.submitNs < presentStartNs))
			scratch_.push_back(iv);
		else
			pending_[keep++] = iv;
	}
	pending_.resize(keep);
	lastReadyNs_ = std::max(lastReadyNs_, readyNs);
	if (scratch_.empty())
		return w;

	std::sort(scratch_.begin(), scratch_.end(),
	          [](const GpuInterval &a, const GpuInterval &b) { return a.startNs < b.startNs; });
	uint64_t busy = 0;
	uint64_t curStart = scratch_[0].startNs, curEnd = scratch_[0].endNs;
	for (size_t i = 1; i < scratch_.size(); i++)
	{
		const GpuInterval &iv = scratch_[i];
		if (iv.startNs > curEnd)
		{
			busy += curEnd - curStart;
			curStart = iv.startNs;
			curEnd = iv.endNs;
		}
		else if (iv.endNs > curEnd)
		{
			curEnd = iv.endNs;
		}
	}
	busy += curEnd - curStart;

	w.valid = true;
	w.startNs = scratch_[0].startNs;
	w.busyNs = busy;
	w.intervals = uint32_t(scratch_.size());
	return w;
}

void GpuFrameAttributor::discardBefore(uint64_t cutNs)
{
	size_t keep = 0;
	for (size_t i = 0; i < pending_.size(); i++)
		if (pending_[i].endNs >= cutNs)
			pending_[keep++] = pending_[i];
	pending_.resize(keep);
}

uint64_t GpuClock::toMonotonic(uint64_t ticks) const
{
	if (!calibrated || ticks == 0)
		return 0;
	int64_t delta;
	if (validBits >= 64)
	{
		delta = int64_t(ticks - calTicks);
	}
	else
	{
		const uint64_t mask = (uint64_t(1) << validBits) - 1;
		uint64_t d = (ticks - calTicks) & mask;
		delta = d > (mask >> 1) ? -int64_t((mask - d) + 1) : int64_t(d);
	}
	const double ns = double(calMonoNs) + double(delta) * periodNs;
	if (!(ns > 0.0))
		return 0;
	return uint64_t(std::llround(ns));
}

} // namespace pml::layer
