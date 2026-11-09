// SPDX-License-Identifier: MIT
#include "ui.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pml::layer::overlay {

namespace {

constexpr ImU32 kLabel = IM_COL32(190, 200, 215, 255);
constexpr ImU32 kValue = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kUnit = IM_COL32(190, 200, 215, 255);
constexpr ImU32 kNaCol = IM_COL32(110, 115, 125, 255);
constexpr ImU32 kWarn = IM_COL32(255, 190, 90, 255);
constexpr ImU32 kGraphPresent = IM_COL32(110, 220, 120, 255);
constexpr ImU32 kGraphDisplay = IM_COL32(255, 165, 70, 255);

// One piece of text on a line: label, value, or unit.
void part(ImU32 color, const char *text, bool first = false)
{
	if (!first)
		ImGui::SameLine(0, 0);
	ImGui::PushStyleColor(ImGuiCol_Text, color);
	ImGui::TextUnformatted(text);
	ImGui::PopStyleColor();
}

// "  12.34 ms" with a fixed width; NA when the value is missing.
void value(double v, const char *fmt, const char *unit, ImU32 color = kValue)
{
	char buf[32];
	if (isNA(v))
	{
		part(kNaCol, "    NA", false);   // no unit after NA
		return;
	}
	std::snprintf(buf, sizeof(buf), fmt, v);
	part(color, buf);
	if (unit && *unit)
		part(kUnit, unit);
}

void label(const char *text, bool first = false)
{
	part(kLabel, text, first);
}

} // namespace

std::string snapshotSummary(const Snapshot &s, bool displayTiming)
{
	auto f = [](double v, const char *fmt) {
		char buf[32];
		if (isNA(v))
			return std::string("NA");
		std::snprintf(buf, sizeof(buf), fmt, v);
		return std::string(buf);
	};
	const bool d = displayTiming && s.hasDisplayInfo;
	std::string out = "present_fps=" + f(s.presentFps, "%.1f") + " shown_fps=" + f(d ? s.displayFps : kNA, "%.1f") +
	                  " avg_ms=" + f(s.avgMs, "%.2f") + " p99_ms=" + f(s.p99Ms, "%.2f") +
	                  " low1_fps=" + f(s.low1Fps, "%.1f") + " low01_fps=" + f(s.low01Fps, "%.1f") + " gqi=" + f(s.gqi, "%.2f") +
	                  " cpu_busy_ms=" + f(s.cpuBusyMs, "%.2f") + " cpu_wait_ms=" + f(s.cpuWaitMs, "%.2f") +
	                  " gpu_busy_ms=" + f(s.gpuBusyMs, "%.2f") + " gpu_wait_ms=" + f(s.gpuWaitMs, "%.2f") +
	                  " gpu_latency_ms=" + f(s.gpuLatencyMs, "%.2f") +
	                  " display_latency_ms=" + f(d ? s.displayLatencyMs : kNA, "%.2f") +
	                  " until_displayed_ms=" + f(d ? s.untilDisplayedMs : kNA, "%.2f") +
	                  " ready_to_screen_ms=" + f(d ? s.readyToScreenMs : kNA, "%.2f") +
	                  " dropped_pct=" + f(d ? s.droppedPct : kNA, "%.1f") +
	                  " vrr_state=" + pml::metrics::vrrStateLabel(s.vrr.state) + " vrr_score=" + std::to_string(s.vrr.score) +
	                  " experience_score=" + std::to_string(s.experience.score) +
	                  " rows=" + std::to_string(s.rowsTotal);
	return out;
}

float autoOverlayScale(uint32_t width, uint32_t height)
{
	return std::clamp(float(std::min(width, height)) / 1440.0f, 0.75f, 3.0f);
}

std::string asciiOnly(const std::string &text)
{
	std::string out = text;
	for (char &c : out)
		if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e)
			c = '?';
	return out;
}

const char *glyphWarmupText()
{
	return " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
}

void drawOverlayUi(const UiParams &p)
{
	if (!p.snapshot)
		return;
	const Snapshot &s = *p.snapshot;
	const float scale = p.scale;
	const float margin = 12.0f * scale;
	const bool right = p.position == pml::OverlayPosition::TopRight || p.position == pml::OverlayPosition::BottomRight;
	const bool bottom = p.position == pml::OverlayPosition::BottomLeft || p.position == pml::OverlayPosition::BottomRight;

	ImGuiStyle &st = ImGui::GetStyle();
	st.WindowPadding = ImVec2(10.0f * scale, 8.0f * scale);
	st.ItemSpacing = ImVec2(0.0f, 3.0f * scale);
	st.WindowRounding = 6.0f * scale;
	st.WindowBorderSize = 0.0f;
	st.Colors[ImGuiCol_WindowBg] = ImVec4(0.03f, 0.035f, 0.05f, 0.80f);

	const float charW = ImGui::GetFontSize() * 0.6f;   // Cousine is monospace, 0.6 em
	const float width = charW * 38.0f + st.WindowPadding.x * 2.0f;
	ImGui::SetNextWindowPos(ImVec2(right ? p.displayWidth - margin : margin, bottom ? p.displayHeight - margin : margin),
	                        ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
	ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
	                               ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
	                               ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
	                               ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_AlwaysAutoResize;
	if (!ImGui::Begin("##presentmon", nullptr, flags))
	{
		ImGui::End();
		return;
	}

	const bool have = s.valid;
	const double na = kNA;
	auto V = [&](double v) { return have ? v : na; };

	if (p.panels & pml::kPanelFps)
	{
		label("FPS ", true);
		if (p.displayTiming)
		{
			label("shown ");
			value(V(s.displayFps), "%6.1f", "");
			label("  ");
		}
		label("presented ");
		value(V(s.presentFps), "%6.1f", "");
		if (s.idle)
			part(kWarn, "  idle");

		label("Frame ", true);
		label("avg ");
		value(V(s.avgMs), "%6.2f", " ms");
		label("  p99 ");
		value(V(s.p99Ms), "%6.2f", " ms");

		label("Lows  ", true);
		label("1% ");
		value(V(s.low1Fps), "%6.1f", "");
		label("  0.1% ");
		value(V(s.low01Fps), "%6.1f", " fps");

		// Gaming Quality Index: 1% low^2 / (60 * average fps); >= 1 means 1% lows at 60 fps or more, steady.
		label("GQI   ", true);
		value(V(s.gqi), "%6.2f", "");
	}

	if (p.panels & pml::kPanelFrameTimeGraph)
	{
		const float h = 44.0f * scale;
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		ImGui::Dummy(ImVec2(w, h + 4.0f * scale));
		ImDrawList *dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(origin, ImVec2(origin.x + w, origin.y + h), IM_COL32(0, 0, 0, 90), 3.0f * scale);
		if (have && s.graphCount > 1)
		{
			// Vertical scale: the worst frame in view, but never below 2x the average so that
			// a steady stream stays flat and a hitch is obvious.
			double top = std::max<double>(s.graphMaxMs * 1.1, isNA(s.avgMs) ? 4.0 : s.avgMs * 2.0);
			top = std::clamp(top, 4.0, 250.0);
			auto plot = [&](const float *vals, ImU32 col) {
				ImVec2 pts[kGraphPoints];
				int n = 0;
				for (int i = 0; i < kGraphPoints; i++)
				{
					if (vals[i] <= 0.0f)
						continue;
					const float x = origin.x + w * float(i) / float(kGraphPoints - 1);
					const float y = origin.y + h - h * std::min(1.0f, float(vals[i] / top));
					pts[n++] = ImVec2(x, y);
				}
				if (n > 1)
					dl->AddPolyline(pts, n, col, ImDrawFlags_None, std::max(1.0f, scale));
			};
			if (p.displayTiming)
				plot(s.graphDisplay, kGraphDisplay);
			plot(s.graphPresent, kGraphPresent);
			char top_s[24];
			std::snprintf(top_s, sizeof(top_s), "%.0f ms", top);
			dl->AddText(ImVec2(origin.x + 3.0f * scale, origin.y + 1.0f * scale), kLabel, top_s);
		}
		else
		{
			dl->AddText(ImVec2(origin.x + 6.0f * scale, origin.y + h * 0.3f), kNaCol, "collecting frame times...");
		}
	}

	if (p.panels & pml::kPanelCpu)
	{
		label("CPU   ", true);
		label("busy ");
		value(V(s.cpuBusyMs), "%6.2f", " ms");
		label("  wait ");
		value(V(s.cpuWaitMs), "%6.2f", " ms");
	}

	if (p.panels & pml::kPanelGpu)
	{
		// Laid out like the CPU row; the latency (CPU start -> GPU start) gets a line of its own to fit.
		label("GPU   ", true);
		label("busy ");
		value(V(s.gpuBusyMs), "%6.2f", " ms");
		label("  wait ");
		value(V(s.gpuWaitMs), "%6.2f", " ms");
		label("      latency ", true);
		value(V(s.gpuLatencyMs), "%6.2f", " ms");
	}

	if (p.panels & pml::kPanelLatency)
	{
		const bool d = p.displayTiming && have && s.hasDisplayInfo;
		label("Disp  ", true);
		label("latency ");
		value(d ? s.displayLatencyMs : na, "%6.2f", " ms");
		label("  until ");
		value(d ? s.untilDisplayedMs : na, "%6.2f", " ms");
		label("Pres  ", true);
		label("ready>screen ");
		value(d ? s.readyToScreenMs : na, "%6.2f", " ms");
	}

	if (p.panels & pml::kPanelDropped)
	{
		label("Drop  ", true);
		if (p.displayTiming && have && s.hasDisplayInfo)
		{
			value(s.droppedPct, "%5.1f", " %");
			char buf[48];
			std::snprintf(buf, sizeof(buf), "  (%u of %u frames)", s.droppedCount, s.displayKnownCount);
			part(kLabel, buf);
		}
		else
		{
			value(na, "", "");
		}
	}

	if (p.panels & pml::kPanelVrr)
	{
		// "VRR   on    43/100  unstable" (on / ? / idle) and the main cause below it; "VRR   off   fixed 120 Hz".
		// In the game the driver's VRR state is unknown: "on" only when the screen visibly follows the game
		// off a known refresh grid, "off" when it visibly snaps to one, "?" when it cannot be told.
		using pml::metrics::VrrState;
		const pml::metrics::VrrVerdict &v = s.vrr;
		label("VRR   ", true);
		if (!have || !s.hasDisplayInfo || v.state == VrrState::NoData)
		{
			value(na, "", "");
			if (have && !s.hasDisplayInfo)
				part(kLabel, "  needs display times");
		}
		else if (v.state == VrrState::Off)
		{
			part(kWarn, "off");
			if (!isNA(v.refreshHz))
			{
				char buf[32];
				std::snprintf(buf, sizeof(buf), "   fixed %.0f Hz", v.refreshHz);
				part(kLabel, buf);
			}
		}
		else
		{
			part(kLabel, v.state == VrrState::Active ? "on    " : v.state == VrrState::Idle ? "idle  " : "?     ");
			const ImU32 col = v.score >= 85 ? kValue : kWarn;
			char buf[16];
			std::snprintf(buf, sizeof(buf), "%3d", v.score);
			part(col, buf);
			part(kLabel, "/100  ");
			part(col, v.state == VrrState::Idle ? "at refresh cap" : pml::metrics::vrrVerdictLabel(v.score));
			if (v.cause != pml::metrics::VrrCause::None && v.state != VrrState::Idle)
			{
				label("      ", true);
				part(kLabel, pml::metrics::vrrCauseLabel(v.cause));
			}
		}
	}

	if (p.panels & pml::kPanelExperience)
	{
		// "Exp    92/100  smooth", and the main cause below it when under 85 ("Exp    61/100  rough" / "stutters").
		const pml::metrics::ExperienceVerdict &e = s.experience;
		label("Exp   ", true);
		if (!have || !s.hasDisplayInfo || e.score < 0)
		{
			value(na, "", "");
			if (have && !s.hasDisplayInfo)
				part(kLabel, "  needs display times");
		}
		else
		{
			const ImU32 col = e.score >= 85 ? kValue : kWarn;
			char buf[16];
			std::snprintf(buf, sizeof(buf), "%3d", e.score);
			part(kLabel, "      ");
			part(col, buf);
			part(kLabel, "/100  ");
			part(col, pml::metrics::experienceVerdictLabel(e.score));
			if (e.cause != pml::metrics::ExperienceCause::None)
			{
				label("      ", true);
				part(kLabel, pml::metrics::experienceCauseLabel(e.cause));
			}
		}
	}

	if ((p.panels & pml::kPanelCap) && p.capShown)
	{
		label("Cap   ", true);
		part(kValue, p.capValue.c_str());
		if (!p.capDetail.empty())
		{
			label("  ");
			part(kLabel, p.capDetail.c_str());
		}
	}

	if (p.panels & pml::kPanelSwapchain)
	{
		part(kValue, p.runtimeLine.c_str(), true);
		part(kLabel, p.formatLine.c_str(), true);
		part(p.displayTiming ? kLabel : kWarn, p.timingLine.c_str(), true);
	}

	ImGui::End();
}

} // namespace pml::layer::overlay
