// SPDX-License-Identifier: MIT
#include <pml/metrics/adapter.hpp>

#include <cstring>

namespace pml::metrics {

namespace {

bool startsWithNoCase(std::string_view s, std::string_view prefix)
{
	if (s.size() < prefix.size())
		return false;
	for (size_t i = 0; i < prefix.size(); ++i) {
		char a = s[i], b = prefix[i];
		if (a >= 'A' && a <= 'Z')
			a = char(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z')
			b = char(b - 'A' + 'a');
		if (a != b)
			return false;
	}
	return true;
}

} // namespace

DisplayState classifyDisplay(const pml::FrameRecord &r)
{
	if ((r.flags & pml::kFrameDisplayed) && r.screenNs != 0)
		return DisplayState::Displayed;
	if (r.flags & pml::kFrameNotDisplayed)
		return DisplayState::NotDisplayed;
	return DisplayState::Unknown;
}

bool isFailedPresent(const pml::FrameRecord &r)
{
	return (r.flags & pml::kFramePresentFailed) != 0 || r.presentResult < 0;
}

SwapchainContext SwapchainContext::fromInfo(uint32_t pid, const pml::SwapchainInfo &info)
{
	SwapchainContext c;
	c.processId = pid;
	c.engineName.assign(info.engineName, strnlen(info.engineName, sizeof(info.engineName)));
	c.refreshDurationNs = info.refreshDurationNs;
	return c;
}

FrameData toFrameData(const pml::FrameRecord &r, const SwapchainContext &ctx)
{
	FrameData f{};

	// CPU. Note for DXVK: these are the entry and exit of vkQueuePresentKHR, which DXVK calls from its
	// presenter thread, so CPUBusy/CPUWait/MsInPresentAPI describe that thread, not the game thread
	// that called D3D Present (see docs/ARCHITECTURE.md; unvalidated against a real Proton capture).
	// The record times are CLOCK_MONOTONIC ns used as QPC ticks. A zero time is "missing"
	// in PresentMon's math, so nothing is substituted for it.
	f.presentStartTime = r.presentStartNs;
	f.timeInPresent = r.presentEndNs > r.presentStartNs ? r.presentEndNs - r.presentStartNs : 0;
	f.readyTime = r.readyNs;

	// GPU (M2). Video queues are out of scope: gpuVideoDuration stays 0.
	f.gpuStartTime = r.gpuStartNs;
	f.gpuDuration = r.gpuBusyNs;
	f.gpuVideoDuration = r.gpuVideoBusyNs;

	// Reflex markers (M3). PresentMon's "app provider" fields and its PC Latency sim start
	// both come from SIMULATION_START (docs/ARCHITECTURE.md 3.2).
	f.appSimStartTime = r.simStartNs;
	f.appSimEndTime = r.simEndNs;
	f.appSleepStartTime = r.sleepStartNs;
	f.appSleepEndTime = r.sleepEndNs;
	f.appRenderSubmitStartTime = r.renderSubmitStartNs;
	f.appRenderSubmitEndTime = r.renderSubmitEndNs;
	f.appPresentStartTime = r.markerPresentStartNs;
	f.appPresentEndTime = r.markerPresentEndNs;
	f.appInputSample = { r.inputSampleNs, r.inputSampleNs ? InputDeviceType::Unknown : InputDeviceType::None };
	f.pclSimStartTime = r.simStartNs;
	f.pclInputPingTime = 0;

	// Input (M4, synthetic only).
	// PresentMon computes MsSinceInput as presentStartTime - inputTime in unsigned arithmetic, so a
	// sample newer than the present start would wrap to ~1.8e13 ms. Treat it as "no input".
	f.inputTime = r.inputNs <= r.presentStartNs ? r.inputNs : 0;
	f.mouseClickTime = r.mouseClickNs <= r.presentStartNs ? r.mouseClickNs : 0;

	// Pipeline compile activity (M3) reuses PresentMon's D3D12 PSO compile columns.
	auto &pso = f.interFrameEventStats[size_t(InterPresentActivity::Kind::D3D12PsoCompile)];
	pso.activityCount = r.pipelineCompileCount;
	pso.busyQpc = r.pipelineCompileBusyNs;
	pso.summedBusyQpc = r.pipelineCompileSummedNs;

	// Present parameters.
	f.swapChainAddress = r.swapchainHandle;
	f.syncInterval = syncIntervalFor(r.presentMode);
	f.supportsTearing = allowsTearingFor(r.presentMode);
	f.presentFlags = 0;
	f.presentId = r.presentId;

	// Identity.
	f.processId = ctx.processId;
	f.threadId = r.threadId;
	f.frameId = uint32_t(r.frameIndex);
	f.appFrameId = uint32_t(r.latencyFrameId);
	f.pclFrameId = uint32_t(r.latencyFrameId);

	// Display. The three cases are described at DisplayState.
	switch (classifyDisplay(r)) {
	case DisplayState::Displayed:
		f.finalState = PresentResult::Presented;
		f.displayed.PushBack({ FrameType::Application, r.screenNs });
		break;
	case DisplayState::NotDisplayed:
		f.finalState = PresentResult::Discarded;
		break;
	case DisplayState::Unknown:
		f.finalState = PresentResult::Presented;
		break;
	}
	return f;
}

const char *runtimeName(std::string_view engineName)
{
	if (startsWithNoCase(engineName, "dxvk"))
		return "DXVK";
	if (startsWithNoCase(engineName, "vkd3d"))
		return "VKD3D";
	return "Vulkan";
}

const char *presentModeName(uint32_t mode)
{
	switch (mode) {
	case kVkPresentModeImmediate: return "IMMEDIATE";
	case kVkPresentModeMailbox: return "MAILBOX";
	case kVkPresentModeFifo: return "FIFO";
	case kVkPresentModeFifoRelaxed: return "FIFO_RELAXED";
	case kVkPresentModeFifoLatestReady: return "FIFO_LATEST_READY";
	case kVkPresentModeSharedDemandRefresh: return "SHARED_DEMAND_REFRESH";
	case kVkPresentModeSharedContinuousRefresh: return "SHARED_CONTINUOUS_REFRESH";
	default: return "Unknown";
	}
}

int32_t syncIntervalFor(uint32_t mode)
{
	return (mode == kVkPresentModeFifo || mode == kVkPresentModeFifoRelaxed ||
	        mode == kVkPresentModeFifoLatestReady) ? 1 : 0;
}

bool allowsTearingFor(uint32_t mode)
{
	return mode == kVkPresentModeImmediate || mode == kVkPresentModeFifoRelaxed;
}

} // namespace pml::metrics
