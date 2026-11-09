// SPDX-License-Identifier: MIT
// Unit tests for the overlay's CPU-side parts: rolling statistics (through the real metrics
// engine), dump encoders, and the ImGui layout run headless.
#include "dump.hpp"
#include "stats.hpp"
#include "ui.hpp"

#include <imgui.h>
#include <vulkan/vulkan.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace pml::layer::overlay;

namespace {

constexpr uint64_t kT0 = 5'000'000'000ull;

pml::SwapchainInfo makeInfo()
{
	pml::SwapchainInfo info = {};
	info.serial = 1;
	info.handle = 0x1234;
	info.width = 1280;
	info.height = 720;
	info.format = VK_FORMAT_B8G8R8A8_SRGB;
	info.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	info.timingFlags = pml::kSwapchainLayerTiming;
	info.timingStagesSupported = VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT | VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
	info.timingStagesUsed = info.timingStagesSupported;
	std::strcpy(info.engineName, "DXVK");
	return info;
}

// Frame n of a synthetic stream: presented every `periodNs`, shown `latencyNs` later, on a display
// that updates every `refreshNs`. dropEvery > 0: every n-th frame is replaced (no screen time).
pml::FrameRecord frame(uint64_t n, uint64_t periodNs, uint64_t latencyNs, int dropEvery = 0, bool display = true)
{
	pml::FrameRecord r = {};
	r.swapchainSerial = 1;
	r.swapchainHandle = 0x1234;
	r.frameIndex = n;
	r.presentId = n;
	r.presentStartNs = kT0 + n * periodNs;
	r.presentEndNs = r.presentStartNs + 100'000;
	r.readyNs = r.presentStartNs + 400'000;
	r.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	r.flags = pml::kFrameTimingRequested | pml::kFrameTimingComplete;
	if (display)
	{
		if (dropEvery > 0 && n % dropEvery == 0)
			r.flags |= pml::kFrameNotDisplayed;
		else
		{
			r.screenNs = r.presentStartNs + latencyNs;
			r.screenStage = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
			r.flags |= pml::kFrameDisplayed;
		}
	}
	return r;
}

} // namespace

TEST(Stats, SteadyStreamMatchesTheInput)
{
	OverlayStats stats(makeInfo(), 1, kT0);
	const uint64_t period = 4'166'667;   // 240 fps
	for (uint64_t n = 1; n <= 480; n++)
		stats.push(frame(n, period, 7'000'000));
	const uint64_t now = kT0 + 480 * period + 20'000'000;
	stats.update(now);
	const Snapshot &s = stats.snapshot();
	ASSERT_TRUE(s.valid);
	EXPECT_FALSE(s.idle);
	EXPECT_TRUE(s.hasDisplayInfo);
	EXPECT_NEAR(s.presentFps, 240.0, 1.0);
	EXPECT_NEAR(s.displayFps, 240.0, 1.0);
	EXPECT_NEAR(s.avgMs, 4.1667, 0.01);
	EXPECT_NEAR(s.p99Ms, 4.1667, 0.01);
	EXPECT_NEAR(s.low1Fps, 240.0, 1.0);
	EXPECT_NEAR(s.low01Fps, 240.0, 1.0);
	EXPECT_NEAR(s.displayLatencyMs, 7.0 + 4.1667 - 0.1, 0.05);   // PresentMon: screen time minus CPU start (end of the previous present call)
	EXPECT_NEAR(s.untilDisplayedMs, 7.0, 0.05);
	EXPECT_NEAR(s.readyToScreenMs, 6.6, 0.05);
	EXPECT_NEAR(s.droppedPct, 0.0, 1e-9);
	EXPECT_TRUE(isNA(s.gpuBusyMs)) << "no GPU data in the records";
	EXPECT_TRUE(isNA(s.gpuLatencyMs));
	EXPECT_GT(s.graphCount, 0);
	EXPECT_NEAR(s.graphPresent[kGraphPoints - 2], 4.1667f, 0.05f);
	EXPECT_NEAR(s.graphDisplay[kGraphPoints - 2], 4.1667f, 0.05f);
}

TEST(Stats, LowsAndPercentileSeeAnOutlier)
{
	OverlayStats stats(makeInfo(), 1, kT0);
	const uint64_t period = 10'000'000;   // 100 fps
	uint64_t t = 0;
	for (uint64_t n = 1; n <= 600; n++)
	{
		pml::FrameRecord r = frame(n, period, 8'000'000);
		// One 100 ms hitch in 600 frames, then everything shifts by it.
		if (n >= 500)
			t = 90'000'000;
		r.presentStartNs += t;
		r.readyNs += t;
		r.screenNs += t;
		stats.push(r);
	}
	stats.update(kT0 + 600 * period + t + 10'000'000);
	const Snapshot &s = stats.snapshot();
	ASSERT_TRUE(s.valid);
	// 600 frames: 0.1% low = the single 100 ms frame, 1% low = mean of the 6 slowest = (100+5*10)/6 ms.
	EXPECT_NEAR(s.low01Fps, 10.0, 0.5);
	EXPECT_NEAR(s.low1Fps, 1000.0 / ((100.0 + 5 * 10.0) / 6.0), 1.0);
	EXPECT_NEAR(s.p99Ms, 10.0, 0.01);   // the 99th percentile ignores the single spike
	EXPECT_GT(s.graphMaxMs, 90.0f);
}

TEST(Stats, DroppedFramesAreCounted)
{
	OverlayStats stats(makeInfo(), 1, kT0);
	const uint64_t period = 2'000'000;
	for (uint64_t n = 1; n <= 500; n++)
		stats.push(frame(n, period, 6'000'000, /*dropEvery=*/4));
	stats.update(kT0 + 500 * period + 10'000'000);
	const Snapshot &s = stats.snapshot();
	ASSERT_TRUE(s.valid);
	ASSERT_TRUE(s.hasDisplayInfo);
	EXPECT_NEAR(s.droppedPct, 25.0, 1.5);
	EXPECT_GT(s.droppedCount, 50u);
	// 3 of 4 frames are shown: 500 fps presented, 375 shown.
	EXPECT_NEAR(s.presentFps, 500.0, 3.0);
	EXPECT_NEAR(s.displayFps, 375.0, 8.0);
}

TEST(Stats, NoDisplayInfoGivesNaInsteadOfNumbers)
{
	OverlayStats stats(makeInfo(), 1, kT0);
	const uint64_t period = 8'333'333;
	for (uint64_t n = 1; n <= 240; n++)
		stats.push(frame(n, period, 0, 0, /*display=*/false));
	stats.update(kT0 + 240 * period + 10'000'000);
	const Snapshot &s = stats.snapshot();
	ASSERT_TRUE(s.valid);
	EXPECT_FALSE(s.hasDisplayInfo);
	EXPECT_NEAR(s.presentFps, 120.0, 1.0);
	EXPECT_TRUE(isNA(s.displayFps));
	EXPECT_TRUE(isNA(s.displayLatencyMs));
	EXPECT_TRUE(isNA(s.untilDisplayedMs));
	EXPECT_TRUE(isNA(s.droppedPct));
	EXPECT_NEAR(s.avgMs, 8.3333, 0.01);
}

TEST(Stats, GpuNumbersArePresentMonsGpuMetrics)
{
	// GPU work from 2 ms before the present call, 1.5 ms busy; ready 0.4 ms after the call.
	OverlayStats stats(makeInfo(), 1, kT0);
	const uint64_t period = 8'333'333;
	for (uint64_t n = 1; n <= 240; n++)
	{
		pml::FrameRecord r = frame(n, period, 6'000'000);
		if (n % 10 != 0)   // a few frames without GPU data do not blank the numbers
		{
			r.gpuStartNs = r.presentStartNs - 2'000'000;
			r.gpuBusyNs = 1'500'000;
		}
		stats.push(r);
	}
	stats.update(kT0 + 240 * period + 10'000'000);
	const Snapshot &s = stats.snapshot();
	ASSERT_TRUE(s.valid);
	EXPECT_NEAR(s.gpuBusyMs, 1.5, 1e-6);
	EXPECT_NEAR(s.gpuWaitMs, 2.4 - 1.5, 1e-6);   // (ready - GPU start) - busy
	// CPU start = end of the previous present call (previous start + 0.1 ms).
	EXPECT_NEAR(s.gpuLatencyMs, 8.333333 - 0.1 - 2.0, 1e-3);
}

TEST(Stats, VrrVerdictFromTheDisplayedFrames)
{
	// Steady presents every 6 ms. First shown evenly, then every tenth frame 3 ms late (the screen waits,
	// then catches up), as Lies of P under vkd3d-proton showed with VRR on.
	auto run = [](bool uneven) {
		OverlayStats stats(makeInfo(), 1, kT0);
		const uint64_t period = 6'000'000;
		for (uint64_t n = 1; n <= 1200; n++)
			stats.push(frame(n, period, (uneven && n % 10 == 0) ? 7'000'000 : 4'000'000));
		pml::metrics::VrrContext ctx;
		ctx.refreshHz = 240;
		ctx.driver = pml::metrics::DriverVrr::On;
		stats.setVrrContext(ctx);
		stats.update(kT0 + 1200 * period + 20'000'000);
		return stats.snapshot().vrr;
	};
	const pml::metrics::VrrVerdict even = run(false);
	EXPECT_EQ(even.state, pml::metrics::VrrState::Active);
	EXPECT_GE(even.score, 95);
	const pml::metrics::VrrVerdict bad = run(true);
	EXPECT_LT(bad.score, 65);
	EXPECT_EQ(bad.cause, pml::metrics::VrrCause::Display);
}

TEST(Stats, IdleWhenNothingArrives)
{
	OverlayStats stats(makeInfo(), 1, kT0);
	for (uint64_t n = 1; n <= 100; n++)
		stats.push(frame(n, 5'000'000, 7'000'000));
	const uint64_t last = kT0 + 100 * 5'000'000;
	stats.update(last + 3'000'000'000ull);
	const Snapshot &s = stats.snapshot();
	ASSERT_TRUE(s.valid);
	EXPECT_TRUE(s.idle);
	EXPECT_EQ(s.presentFps, 0.0);
}

TEST(Stats, HiddenOverlayCollectsNothing)
{
	OverlayStats stats(makeInfo(), 1, kT0);
	stats.setCollecting(false);
	for (uint64_t n = 1; n <= 50; n++)
		stats.push(frame(n, 5'000'000, 7'000'000));
	stats.update(kT0 + 300'000'000);
	EXPECT_FALSE(stats.snapshot().valid);
	EXPECT_EQ(stats.snapshot().rowsTotal, 0u);
	stats.setCollecting(true);
	stats.reset();
	for (uint64_t n = 51; n <= 150; n++)
		stats.push(frame(n, 5'000'000, 7'000'000));
	stats.update(kT0 + 151 * 5'000'000ull);
	EXPECT_TRUE(stats.snapshot().valid);
}

TEST(Stats, QueueIsBoundedWhenNobodyDrains)
{
	OverlayStats stats(makeInfo(), 1, kT0);
	for (uint64_t n = 1; n <= 20000; n++)
		stats.push(frame(n, 1'000'000, 3'000'000));
	stats.update(kT0 + 20001 * 1'000'000ull);
	EXPECT_TRUE(stats.snapshot().valid);
	EXPECT_LE(stats.snapshot().rowsTotal, 4096u);
}

TEST(Dump, PqEncodesSdrWhiteAtCodeZeroPointFiveEight)
{
	EXPECT_NEAR(pqEncodeNits(203.0f), 0.5807f, 0.001f);
	EXPECT_NEAR(pqEncodeNits(203.0f) * 1023.0f, 594.0f, 1.5f);
	EXPECT_NEAR(pqDecodeToNits(pqEncodeNits(203.0f)), 203.0f, 0.5f);
	EXPECT_NEAR(pqDecodeToNits(pqEncodeNits(1000.0f)), 1000.0f, 3.0f);
	EXPECT_NEAR(srgbEncode(0.5f), 0.7354f, 0.001f);
}

TEST(Dump, DecodesAllSupportedFormatsToWhite)
{
	auto decode = [](uint32_t format, uint32_t cs, std::vector<uint8_t> px) {
		DumpImage img;
		img.vkFormat = format;
		img.vkColorSpace = cs;
		img.width = 1;
		img.height = 1;
		img.bytes = std::move(px);
		std::vector<uint8_t> rgb;
		EXPECT_TRUE(dumpToRgb8(img, &rgb));
		return rgb;
	};
	// 8-bit BGRA (blue, green, red, alpha in memory)
	auto a = decode(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, {10, 20, 30, 255});
	EXPECT_EQ(a, (std::vector<uint8_t>{30, 20, 10}));
	// HDR10: PQ code of 203 nits in all three channels = white
	const uint32_t code = uint32_t(std::lround(pqEncodeNits(203.0f) * 1023.0f));
	const uint32_t v = code | (code << 10) | (code << 20) | (3u << 30);
	std::vector<uint8_t> px(4);
	std::memcpy(px.data(), &v, 4);
	auto b = decode(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT, px);
	EXPECT_NEAR(b[0], 255, 2);
	EXPECT_NEAR(b[1], 255, 2);
	EXPECT_NEAR(b[2], 255, 2);
	// scRGB: 2.5375 (203 / 80) in half precision is 0x4113 (2.53515625), ~ white
	std::vector<uint8_t> h(8, 0);
	const uint16_t half = 0x4113;
	for (int c = 0; c < 3; c++)
		std::memcpy(h.data() + 2 * c, &half, 2);
	auto c = decode(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, h);
	EXPECT_NEAR(c[0], 255, 2);
}

TEST(Dump, WritesPngAndRawFile)
{
	DumpImage img;
	img.vkFormat = VK_FORMAT_B8G8R8A8_SRGB;
	img.vkColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	img.width = 4;
	img.height = 2;
	img.frameIndex = 9;
	img.bytes.assign(4 * 2 * 4, 0x80);
	const std::string path = std::string(::testing::TempDir()) + "pml_dump_test.png";
	EXPECT_EQ(writeDump(img, path), "");
	FILE *f = std::fopen(path.c_str(), "rb");
	ASSERT_NE(f, nullptr);
	unsigned char sig[4];
	ASSERT_EQ(std::fread(sig, 1, 4, f), 4u);
	std::fclose(f);
	EXPECT_EQ(sig[1], 'P');
	f = std::fopen((path + ".raw").c_str(), "rb");
	ASSERT_NE(f, nullptr);
	char head[64];
	ASSERT_EQ(std::fread(head, 1, 64, f), 64u);
	std::fclose(f);
	EXPECT_EQ(std::string(head, 8), "PMLDUMP1");
	uint32_t fields[7];
	std::memcpy(fields, head + 8, sizeof(fields));
	EXPECT_EQ(fields[0], uint32_t(VK_FORMAT_B8G8R8A8_SRGB));
	EXPECT_EQ(fields[2], 4u);
	EXPECT_EQ(fields[4], 16u);
	EXPECT_EQ(fields[6], 9u);
	std::remove(path.c_str());
	std::remove((path + ".raw").c_str());
}

// The layout code run without a renderer: checks that every panel combination produces geometry,
// and that "no display info" does not print numbers.
class UiTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ctx_ = ImGui::CreateContext();
		ImGuiIO &io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(1280, 720);
		io.DeltaTime = 1.0f / 60.0f;
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
		io.Fonts->AddFontDefaultBitmap();
	}
	void TearDown() override { ImGui::DestroyContext(ctx_); }

	// Returns the number of vertices drawn.
	int drawFrame(const UiParams &p)
	{
		int vtx = 0;
		for (int i = 0; i < 3; i++)
		{
			ImGui::NewFrame();
			drawOverlayUi(p);
			ImGui::Render();
			vtx = ImGui::GetDrawData()->TotalVtxCount;
			for (ImTextureData *tex : ImGui::GetPlatformIO().Textures)
				if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates)
					tex->SetStatus(ImTextureStatus_OK);   // no renderer to upload to
		}
		return vtx;
	}
	ImGuiContext *ctx_ = nullptr;
};

TEST_F(UiTest, DrawsForEveryPanelMaskAndPosition)
{
	OverlayStats stats(makeInfo(), 1, kT0);
	for (uint64_t n = 1; n <= 300; n++)
		stats.push(frame(n, 4'166'667, 7'000'000));
	stats.update(kT0 + 301 * 4'166'667ull);
	for (uint32_t pos = 0; pos < 4; pos++)
		for (uint32_t mask : {kDefaultPanels, uint32_t(pml::kPanelFps), uint32_t(pml::kPanelFrameTimeGraph),
		                      uint32_t(pml::kPanelCpu | pml::kPanelGpu), uint32_t(pml::kPanelSwapchain | pml::kPanelDropped)})
		{
			UiParams p;
			p.snapshot = &stats.snapshot();
			p.panels = mask;
			p.position = pml::OverlayPosition(pos);
			p.scale = 1.0f;
			p.displayWidth = 1280;
			p.displayHeight = 720;
			p.runtimeLine = "DXVK  FIFO  1280x720";
			p.formatLine = "B8G8R8A8_SRGB  sRGB";
			p.timingLine = "timing: pixel-out + queue-end";
			EXPECT_GT(drawFrame(p), 0) << "mask " << mask << " pos " << pos;
		}
}

TEST_F(UiTest, EmptySnapshotStillDraws)
{
	Snapshot empty;
	UiParams p;
	p.snapshot = &empty;
	p.panels = kDefaultPanels;
	p.displayWidth = 1280;
	p.displayHeight = 720;
	p.displayTiming = false;
	EXPECT_GT(drawFrame(p), 0);
}

TEST(Ui, AutoScaleFollowsTheShorterEdge)
{
	EXPECT_FLOAT_EQ(autoOverlayScale(2560, 1440), 1.0f);
	EXPECT_FLOAT_EQ(autoOverlayScale(1440, 2560), 1.0f) << "portrait monitor: same panel as landscape";
	EXPECT_FLOAT_EQ(autoOverlayScale(3840, 2160), 1.5f);
	EXPECT_FLOAT_EQ(autoOverlayScale(800, 450), 0.75f) << "clamped";
	EXPECT_FLOAT_EQ(autoOverlayScale(9000, 9000), 3.0f);
}

TEST(Ui, ApplicationTextIsReducedToPrintableAscii)
{
	EXPECT_EQ(asciiOnly("DXVK  FIFO"), "DXVK  FIFO");
	EXPECT_EQ(asciiOnly("Motor\xc3\xa9"), "Motor??");
	EXPECT_EQ(asciiOnly("a\tb\n"), "a?b?");
	EXPECT_EQ(asciiOnly(""), "");
	for (const char *c = glyphWarmupText(); *c; c++)
		EXPECT_EQ(asciiOnly(std::string(1, *c)), std::string(1, *c)) << "every warmed-up glyph survives";
}
