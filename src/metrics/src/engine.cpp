// SPDX-License-Identifier: MIT
#include <pml/metrics/engine.hpp>

#include <CommonUtilities/mc/MetricsCalculator.h>

namespace pml::metrics {

using pmon::util::metrics::ComputeMetricsForPresent;
using pmon::util::metrics::UnifiedSwapChain;

namespace {
// frameId of the synthetic present finish() uses to push out held rows; never reported.
constexpr uint32_t kFlushFrameId = 0xFFFFFFFFu;
// V2 holds not-displayed presents until the next displayed one. When presents without a screen time
// have kept arriving for this long (in present time) and nothing was released meanwhile, display
// information has stopped (the layer backs off timing requests when its queue is full, e.g. at 19000
// fps in mailbox mode), and the held rows are released with NA display columns.
constexpr uint64_t kStallNs = 500'000'000ull;
// ...and at least this many presents have been held since the last release. Together with the rule
// that a displayed present never counts as a stall, this keeps upstream's results for long gaps
// between presents (a hitch, a 1 fps menu, a pause): there the next present releases the held rows
// itself, with its real lookahead.
constexpr uint64_t kStallMinHeld = 2;
}

SwapchainEngine::SwapchainEngine(SwapchainContext context, MetricsMode mode, uint64_t sessionStartNs)
	: ctx_(std::move(context))
	, mode_(mode)
	, qpc_(makeQpcConverter(sessionStartNs))
	, unified_(std::make_unique<UnifiedSwapChain>())
{
}

SwapchainEngine::~SwapchainEngine() = default;
SwapchainEngine::SwapchainEngine(SwapchainEngine &&) noexcept = default;
SwapchainEngine &SwapchainEngine::operator=(SwapchainEngine &&) noexcept = default;

void SwapchainEngine::takePending(uint32_t frameId, Pending &out)
{
	auto it = pending_.find(frameId);
	if (it == pending_.end())
		return;
	out = it->second;
	pending_.erase(it);
}

Row SwapchainEngine::makeRow(const FrameData &frame, const FrameMetrics &metrics)
{
	Row row;
	row.frame = frame;
	row.metrics = metrics;
	row.frameType = metrics.frameType;
	row.isAppFrame = metrics.frameType == FrameType::Application || metrics.frameType == FrameType::NotSet;

	Pending p{ DisplayState::Unknown, 0, 0, 0 };
	takePending(frame.frameId, p);
	row.display = p.display;
	row.vkPresentMode = p.vkPresentMode;
	row.swapchainSerial = p.serial;
	row.frameIndex = p.frameIndex;
	row.dropped = p.display == DisplayState::NotDisplayed;
	row.hasDisplayMetrics = p.display == DisplayState::Displayed;

	// A present with no display information is neither displayed nor dropped. PresentMon's math
	// sees "no display entry" and calls it dropped; undo that. The display doubles are already
	// 0 (the CSV prints 0 as NA) or NaN, so nothing else needs hiding.
	if (p.display == DisplayState::Unknown)
		row.metrics.isDroppedFrame = false;

	row.runtime = runtimeName(ctx_.engineName);
	row.presentMode = presentModeName(p.vkPresentMode);
	row.syncInterval = frame.syncInterval;
	row.allowsTearing = frame.supportsTearing;
	return row;
}

std::vector<Row> SwapchainEngine::process(const pml::FrameRecord &record)
{
	std::vector<Row> out;
	if (isFailedPresent(record) || finished_) {
		++ignored_;
		return out;
	}
	++processed_;

	if (mode_ == MetricsMode::V2) {
		// A stall is an unbroken stream of presents without screen times, not a gap. The run starts at the
		// first present held after the last release, and a gap of kStallNs or more between two presents (a
		// hitch, a pause, fewer than 2 fps) starts it again, so time inside a gap never counts: the rows held
		// across it wait for the next displayed present, as in upstream.
		const uint64_t start = record.presentStartNs;
		if (lastIngestedNs_ != 0 && start >= lastIngestedNs_ + kStallNs)
			heldSinceRelease_ = 0;
		if (lastReleaseNs_ == 0)
			lastReleaseNs_ = start;
		else if (classifyDisplay(record) != DisplayState::Displayed && heldSinceRelease_ >= kStallMinHeld &&
		         start > firstHeldNs_ + kStallNs && start > lastReleaseNs_ + kStallNs)
			out = recoverFromStall(start);
		if (heldSinceRelease_++ == 0)
			firstHeldNs_ = start;
		lastIngestedNs_ = start;
	}

	FrameData frame = toFrameData(record, ctx_);
	if (frame.frameId == 0)
		frame.frameId = ++nextAutoFrameId_;
	if (frame.frameId == kFlushFrameId)
		frame.frameId = kFlushFrameId - 1;

	// Window the "pipeline compile busy %" is relative to: time since the previous present.
	if (frame.interFrameEventStats[0].busyQpc != 0 && lastPresentStartNs_ != 0 &&
	    frame.presentStartTime > lastPresentStartNs_)
		frame.interFrameEventWindowQpc = frame.presentStartTime - lastPresentStartNs_;
	lastPresentStartNs_ = frame.presentStartTime;

	const DisplayState display = classifyDisplay(record);
	if (display == DisplayState::Displayed)
		lastScreenNs_ = record.screenNs;

	const bool seedOnly = !unified_->swapChain.lastPresent.has_value();
	const uint32_t frameId = frame.frameId;
	pending_[frameId] = Pending{ display, record.presentMode, record.swapchainSerial, record.frameIndex };

	if (mode_ == MetricsMode::V1) {
		// OutputThread.cpp: sanitize, ComputeMetricsForPresent on the chain state, and the
		// first present is computed (so state advances) but not reported.
		UnifiedSwapChain::SanitizeDisplayedRepeatedPresents(frame);
		auto computed = ComputeMetricsForPresent(qpc_, frame, unified_->swapChain);
		if (seedOnly) {
			pending_.erase(frameId);
			return out;
		}
		for (auto &cm : computed)
			out.push_back(makeRow(frame, cm.metrics));
		return out;
	}

	// V2: ProcessPresent seeds on the first present and returns no rows.
	auto processed = unified_->ProcessPresent(qpc_, std::move(frame));
	if (seedOnly)
		pending_.erase(frameId);
	for (auto &r : processed)
		out.push_back(makeRow(r.present, r.computed.metrics));
	if (!processed.empty()) {
		lastReleaseNs_ = record.presentStartNs;
		heldSinceRelease_ = 0;
	}
	return out;
}

std::vector<Row> SwapchainEngine::recoverFromStall(uint64_t nowNs)
{
	lastReleaseNs_ = nowNs;
	heldSinceRelease_ = 0;
	if (!unified_->swapChain.lastPresent.has_value() || lastScreenNs_ == 0)
		return {};   // no displayed present was ever seen: nothing is held back
	// Release what is held (display columns NA, as at the end of a capture), then restart the chain
	// from the next present; as at the very start, the first present only seeds the state.
	std::vector<Row> out = flushHeld();
	unified_ = std::make_unique<UnifiedSwapChain>();
	pending_.clear();
	lastScreenNs_ = 0;
	return out;
}

std::vector<Row> SwapchainEngine::finish()
{
	if (finished_ || mode_ == MetricsMode::V1)
		return {};
	finished_ = true;
	return flushHeld();
}

std::vector<Row> SwapchainEngine::flushHeld()
{
	std::vector<Row> out;
	if (!unified_->swapChain.lastPresent.has_value() || lastScreenNs_ == 0)
		return out;   // nothing is waiting for a lookahead

	// Rows wait for the next displayed screen time. Hand the queue a synthetic displayed present
	// at the last known screen time (so the held frames get DisplayedTime 0 = "NA" rather than a
	// made-up duration) and drop whatever it releases for itself.
	FrameData flush{};
	flush.frameId = kFlushFrameId;
	flush.processId = ctx_.processId;
	flush.swapChainAddress = unified_->swapChain.lastPresent->swapChainAddress;
	flush.presentStartTime = lastPresentStartNs_ + 1;
	flush.finalState = PresentResult::Presented;
	flush.displayed.PushBack({ FrameType::Application, lastScreenNs_ });
	for (auto &r : unified_->ProcessPresent(qpc_, std::move(flush))) {
		if (r.present.frameId == kFlushFrameId)
			continue;
		out.push_back(makeRow(r.present, r.computed.metrics));
	}
	return out;
}

} // namespace pml::metrics
