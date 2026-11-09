// SPDX-License-Identifier: MIT
// Shared helpers for pml_gui_tests.
#pragma once

#include <pml/clock.hpp>
#include <pml/protocol.hpp>
#include <pml/shm.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>

#include <unistd.h>

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pmlguitest {

// Runs the Qt event loop until pred() is true or timeoutMs passes; returns pred().
inline bool waitUntil(const std::function<bool()> &pred, int timeoutMs = 5000)
{
	QElapsedTimer t;
	t.start();
	while (!pred() && t.elapsed() < timeoutMs)
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
	return pred();
}

// ---- Fake producer ----------------------------------------------------------------------------
//
// Stands in for VkLayer_presentmon inside the test process: creates presentmon-linux.<getpid()> the
// way the layer does and publishes swapchains and records. It only ever touches the segment of the
// test's own pid (a game of the user's may be running with the layer, its segment must stay alone),
// and removes it again in the destructor, so a failing test cleans up as well.

inline pml::SwapchainInfo makeSwapchainInfo(uint64_t serial, uint32_t width, uint32_t height, uint64_t createdNs,
                                            uint64_t refreshNs = 0, const char *engine = "DXVK")
{
	pml::SwapchainInfo s = {};
	s.serial = serial;
	s.handle = 0xA000 + serial;
	s.createdNs = createdNs;
	s.refreshDurationNs = refreshNs;
	s.width = width;
	s.height = height;
	s.format = 50;      // VK_FORMAT_B8G8R8A8_SRGB
	s.colorSpace = 0;   // VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
	s.presentMode = 2;  // VK_PRESENT_MODE_FIFO_KHR
	s.minImageCount = 2;
	s.imageCount = 3;
	s.surfaceKind = uint32_t(pml::SurfaceKind::Wayland);
	s.timingStagesSupported = 0x5;
	s.timingStagesUsed = 0x5;
	s.timingFlags = pml::kSwapchainLayerTiming;
	std::strncpy(s.engineName, engine, sizeof(s.engineName) - 1);
	return s;
}

// A record like the layer's: presented at presentStartNs, ready 1 ms later, on screen screenNs (0 = no display
// information for this present).
inline pml::FrameRecord makeRecord(uint64_t serial, uint64_t index, uint64_t presentStartNs, uint64_t screenNs)
{
	pml::FrameRecord r = {};
	r.swapchainSerial = serial;
	r.swapchainHandle = 0xA000 + serial;
	r.frameIndex = index;
	r.presentId = index;
	r.presentStartNs = presentStartNs;
	r.presentEndNs = presentStartNs + 200'000;
	r.threadId = 1;
	r.presentMode = 2;
	r.readyNs = presentStartNs + 1'000'000;
	r.flags = pml::kFrameTimingRequested | pml::kFrameTimingComplete;
	if (screenNs)
	{
		r.screenNs = screenNs;
		r.screenStage = 4;
		r.flags |= pml::kFrameDisplayed;
	}
	return r;
}

// `count` records of one swapchain at a fixed cadence: frame n (first, first+1, ...) is presented at
// startNs + n * periodNs and shown latencyNs later.
inline std::vector<pml::FrameRecord> makeStream(uint64_t serial, uint64_t first, uint64_t count, uint64_t startNs,
                                                uint64_t periodNs, uint64_t latencyNs = 8'000'000)
{
	std::vector<pml::FrameRecord> out;
	out.reserve(count);
	for (uint64_t n = first; n < first + count; ++n)
	{
		const uint64_t t = startNs + n * periodNs;
		out.push_back(makeRecord(serial, n, t, t + latencyNs));
	}
	return out;
}

class FakeProducer
{
public:
	explicit FakeProducer(uint32_t ringCapacity = 8192, bool overlayAvailable = true)
	{
		pml::ProcessIdentity id = pml::currentProcessIdentity();
		id.processName = "FakeGame.exe";
		id.exePath = "/opt/games/FakeGame/FakeGame.exe";
		id.engineName = "DXVK";
		id.applicationName = "Fake Game";
		id.layerVersion = "fake-0";
		pml::OverlayControl initial = {};
		initial.visible = 1;
		initial.position = uint32_t(pml::OverlayPosition::TopLeft);
		std::string error;
		segment_ = pml::Segment::create(pml::segmentNameForPid(getpid()), id, ringCapacity, initial, &error);
		if (segment_)
		{
			pml::setOverlayAvailable(*segment_, overlayAvailable);
			writer_ = std::make_unique<pml::RingWriter>(*segment_);
		}
		else
			error_ = error;
	}

	~FakeProducer() { remove(); }
	FakeProducer(const FakeProducer &) = delete;
	FakeProducer &operator=(const FakeProducer &) = delete;

	bool ok() const { return segment_ != nullptr; }
	const std::string &error() const { return error_; }
	int pid() const { return int(getpid()); }
	pml::Segment &segment() { return *segment_; }

	// Returns the table slot (for destroySwapchain).
	uint32_t addSwapchain(const pml::SwapchainInfo &info) { return pml::publishSwapchain(*segment_, info); }
	// Marks the swapchain in `slot` destroyed at destroyedNs, like the layer does on vkDestroySwapchainKHR.
	void destroySwapchain(uint32_t slot, pml::SwapchainInfo info, uint64_t destroyedNs)
	{
		info.destroyedNs = destroyedNs;
		pml::updateSwapchain(*segment_, slot, info);
	}

	void publish(const pml::FrameRecord &record)
	{
		writer_->publish(record);
		pml::touchHeartbeat(*segment_, pml::monotonicNs());
	}
	void publish(const std::vector<pml::FrameRecord> &records)
	{
		for (const pml::FrameRecord &r : records)
			writer_->publish(r);
		pml::touchHeartbeat(*segment_, pml::monotonicNs());
	}

	// Real-time cadence: pump() publishes the records that are due since startCadence() (one swapchain),
	// timestamped on the real clock, and keeps the heartbeat fresh. Call it from the wait loop.
	void startCadence(uint64_t serial, double fps)
	{
		cadenceSerial_ = serial;
		cadencePeriodNs_ = uint64_t(1e9 / fps);
		cadenceStartNs_ = pml::monotonicNs();
		cadenceSent_ = 0;
	}
	void pump()
	{
		if (!cadencePeriodNs_)
			return;
		const uint64_t now = pml::monotonicNs();
		const uint64_t due = (now - cadenceStartNs_) / cadencePeriodNs_;
		while (cadenceSent_ < due)
		{
			++cadenceSent_;
			const uint64_t t = cadenceStartNs_ + cadenceSent_ * cadencePeriodNs_;
			writer_->publish(makeRecord(cadenceSerial_, cadenceSent_, t, t + 8'000'000));
		}
		if (due)
			pml::touchHeartbeat(*segment_, now);
	}
	void stopCadence() { cadencePeriodNs_ = 0; }

	void exit()
	{
		if (segment_)
			pml::setState(*segment_, pml::SegmentState::Exited);
	}
	// Removes the name from /dev/shm (keeps the mapping).
	void unlink()
	{
		if (segment_)
			segment_->unlink();
	}
	void remove()
	{
		writer_.reset();
		if (segment_)
			segment_->unlink();
		segment_.reset();
	}

private:
	std::unique_ptr<pml::Segment> segment_;
	std::unique_ptr<pml::RingWriter> writer_;
	std::string error_;
	uint64_t cadenceSerial_ = 0, cadencePeriodNs_ = 0, cadenceStartNs_ = 0, cadenceSent_ = 0;
};

} // namespace pmlguitest
