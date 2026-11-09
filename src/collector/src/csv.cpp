// SPDX-License-Identifier: MIT
// Port of PresentMon/CsvOutput.cpp. Copyright (C) 2017-2024 Intel Corporation, MIT.
// Differences from upstream:
//  * FILE*/fprintf instead of wide printf (identical text).
//  * pmSession time helpers are CsvClock; the timebase is CLOCK_MONOTONIC ns.
//  * The Application column replaces ',' and newlines (the Windows names cannot contain them).
//  * Legacy ETW-only columns (--track_etw_status) are not ported.
#include "csv.hpp"

#include <cfloat>
#include <cinttypes>
#include <cmath>
#include <ctime>

namespace pml::cli {

using pml::metrics::IsMissingFrameMetricValue;
using pml::metrics::Row;

namespace {

bool hasValue(double v) { return !IsMissingFrameMetricValue(v); }

constexpr int kPrec = DBL_DIG - 1;   // upstream prints the "full precision" columns with 14 decimals

void writeMetricOrZero(FILE *fp, double value, int precision = 4)
{
	std::fprintf(fp, ",%.*f", precision, hasValue(value) ? value : 0.0);
}

void putNaOr(FILE *fp, double value, int precision = 4)
{
	if (hasValue(value))
		std::fprintf(fp, ",%.*f", precision, value);
	else
		std::fputs(",NA", fp);
}

} // namespace

CsvClock CsvClock::start()
{
	CsvClock c;
	timespec mono = {}, real = {};
	clock_gettime(CLOCK_MONOTONIC, &mono);
	clock_gettime(CLOCK_REALTIME, &real);
	c.sessionStartNs = uint64_t(mono.tv_sec) * 1000000000ull + uint64_t(mono.tv_nsec);
	c.wallOffsetNs = (int64_t(real.tv_sec) * 1000000000ll + real.tv_nsec) -
	                 (int64_t(mono.tv_sec) * 1000000000ll + mono.tv_nsec);
	return c;
}

double CsvClock::msSinceStart(uint64_t ts) const
{
	if (ts == 0)
		return 0.0;
	return ts >= sessionStartNs ? double(ts - sessionStartNs) * 1e-6 : -double(sessionStartNs - ts) * 1e-6;
}

std::string CsvClock::dateTime(uint64_t ts) const
{
	int64_t wall = int64_t(ts) + wallOffsetNs;
	time_t secs = time_t(wall / 1000000000ll);
	uint64_t ns = uint64_t(wall % 1000000000ll);
	tm lt = {};
	localtime_r(&secs, &lt);
	char buf[96];
	std::snprintf(buf, sizeof(buf), "%u-%u-%u %u:%02u:%02u.%09llu", unsigned(lt.tm_year + 1900),
	              unsigned(lt.tm_mon + 1), unsigned(lt.tm_mday), unsigned(lt.tm_hour), unsigned(lt.tm_min),
	              unsigned(lt.tm_sec), static_cast<unsigned long long>(ns));
	return buf;
}

std::string csvSafeName(std::string name)
{
	for (char &c : name)
		if (c == ',' || c == '\n' || c == '\r')
			c = '_';
	return name;
}

const char *frameTypeToString(FrameType ft)
{
	switch (ft)
	{
	case FrameType::NotSet:
	case FrameType::Repeated:
	case FrameType::Application: return "Application";
	case FrameType::Intel_XEFG: return "Intel XeSS-FG";
	case FrameType::AMD_AFMF: return "AMD AFMF";
	case FrameType::Unspecified: break;   // upstream has no case for it either
	}
	return "Unknown";
}

// ---- Headers -----------------------------------------------------------------------------

namespace {

void writeHeaderV1(FILE *fp, const CaptureOptions &args)
{
	std::fputs("Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags,Dropped", fp);
	std::fputs(",TimeInSeconds,msInPresentAPI,msBetweenPresents", fp);
	if (args.trackDisplay)
		std::fputs(",AllowsTearing,PresentMode,msUntilRenderComplete,msUntilDisplayed,msBetweenDisplayChange,msFlipDelay", fp);
	if (args.trackGPU)
		std::fputs(",msUntilRenderStart,msGPUActive", fp);
	if (args.trackGPUVideo)
		std::fputs(",msGPUVideoActive", fp);
	if (args.trackInput)
		std::fputs(",msSinceInput", fp);
	if (args.timeUnit == TimeUnit::QPC || args.timeUnit == TimeUnit::QPCMilliSeconds)
		std::fputs(",QPCTime", fp);
	if (args.writeDisplayTime)
		std::fputs(",msDisplayTime", fp);
	if (args.writeFrameId)
		std::fputs(",FrameId", fp);
	std::fputs("\n", fp);
}

void writeHeaderMain(FILE *fp, const CaptureOptions &args)
{
	std::fputs("Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,PresentFlags", fp);
	if (args.writeDisplayMetadata)
		std::fputs(",VidPnSourceId,LayerIndex", fp);
	if (args.trackDisplay)
		std::fputs(",AllowsTearing,PresentMode", fp);
	if (args.trackFrameType)
		std::fputs(",FrameType", fp);
	if (args.trackHybridPresent)
		std::fputs(",HybridPresent", fp);

	if (!args.useV2Metrics)
	{
		switch (args.timeUnit)
		{
		case TimeUnit::MilliSeconds: std::fputs(",TimeInMs", fp); break;
		case TimeUnit::QPC: std::fputs(",TimeInQPC", fp); break;
		case TimeUnit::DateTime: std::fputs(",TimeInDateTime", fp); break;
		default: std::fputs(",TimeInSeconds", fp); break;
		}
		std::fputs(",MsBetweenSimulationStart,MsBetweenPresents", fp);
		if (args.trackDisplay)
			std::fputs(",MsBetweenDisplayChange", fp);
		std::fputs(",MsInPresentAPI,MsRenderPresentLatency", fp);
		if (args.trackDisplay)
		{
			std::fputs(",MsUntilDisplayed", fp);
			if (args.trackPcLatency)
				std::fputs(",MsPCLatency", fp);
		}
	}

	if (args.useV2Metrics)
	{
		switch (args.timeUnit)
		{
		case TimeUnit::MilliSeconds: std::fputs(",CPUStartTime", fp); break;
		case TimeUnit::QPC: std::fputs(",CPUStartQPC", fp); break;
		case TimeUnit::QPCMilliSeconds: std::fputs(",CPUStartQPCTime", fp); break;
		case TimeUnit::DateTime: std::fputs(",CPUStartDateTime", fp); break;
		}
		std::fputs(",FrameTime,CPUBusy,CPUWait", fp);
		if (args.trackGPU)
			std::fputs(",GPULatency,GPUTime,GPUBusy,GPUWait", fp);
		if (args.trackGPUVideo)
			std::fputs(",VideoBusy", fp);
		if (args.trackDisplay)
			std::fputs(",DisplayLatency,DisplayedTime,AnimationError,AnimationTime,MsFlipDelay", fp);
		if (args.trackInput)
			std::fputs(",AllInputToPhotonLatency,ClickToPhotonLatency", fp);
		if (args.trackAppTiming)
			std::fputs(",InstrumentedLatency", fp);
	}
	else
	{
		switch (args.timeUnit)
		{
		case TimeUnit::MilliSeconds: std::fputs(",CPUStartTimeInMs", fp); break;
		case TimeUnit::QPC: std::fputs(",CPUStartQPC", fp); break;
		case TimeUnit::QPCMilliSeconds: std::fputs(",CPUStartQPCTimeInMs", fp); break;
		case TimeUnit::DateTime: std::fputs(",CPUStartDateTime", fp); break;
		}
		std::fputs(",MsBetweenAppStart,MsCPUBusy,MsCPUWait", fp);
		if (args.trackGPU)
			std::fputs(",MsGPULatency,MsGPUTime,MsGPUBusy,MsGPUWait", fp);
		if (args.trackGPUVideo)
			std::fputs(",MsVideoBusy", fp);
		if (args.trackDisplay)
			std::fputs(",MsAnimationError,AnimationTime,MsFlipDelay", fp);
		if (args.trackInput)
			std::fputs(",MsAllInputToPhotonLatency,MsClickToPhotonLatency", fp);
		if (args.trackAppTiming)
			std::fputs(",MsInstrumentedLatency", fp);
	}
	if (args.writeDisplayTime)
		std::fputs(",DisplayTimeAbs", fp);
	if (args.writeFrameId)
		std::fputs(",FrameId", fp);
	if (args.writeDisplayMetadata)
		std::fputs(",PresentId", fp);
	if (args.writeFrameId)
	{
		if (args.trackAppTiming)
			std::fputs(",AppFrameId", fp);
		if (args.trackPcLatency)
			std::fputs(",PCLFrameId", fp);
	}
	std::fputs("\n", fp);
}

} // namespace

void writeCsvHeader(FILE *fp, const CaptureOptions &opts)
{
	if (opts.useV1Metrics)
		writeHeaderV1(fp, opts);
	else
		writeHeaderMain(fp, opts);
	if (opts.csvOutput == CsvOutput::Stdout)
		std::fflush(fp);
}

// ---- Rows --------------------------------------------------------------------------------

namespace {

// Upstream: FrameMetrics1 / ToFrameMetrics1 in OutputThread.cpp.
void writeRowV1(FILE *fp, const CaptureOptions &args, const CsvClock &clock, const std::string &application,
                const Row &row)
{
	const auto &p = row.frame;
	const auto &m = row.metrics;
	const double msFlipDelay = IsMissingFrameMetricValue(m.msFlipDelay) ? 0.0 : m.msFlipDelay;

	std::fprintf(fp, "%s,%d,0x%016llX,%s,%d,%d,%s", csvSafeName(application).c_str(), int(p.processId),
	             static_cast<unsigned long long>(p.swapChainAddress), row.runtime, int(row.syncInterval),
	             int(p.presentFlags), p.finalState == PresentResult::Presented ? "0" : "1");
	if (args.timeUnit == TimeUnit::DateTime)
		std::fprintf(fp, ",%s", clock.dateTime(p.presentStartTime).c_str());
	else
		std::fprintf(fp, ",%.*f", kPrec, 0.001 * clock.msSinceStart(p.presentStartTime));
	std::fprintf(fp, ",%.*f,%.*f", kPrec, m.msInPresentApi, kPrec, m.msBetweenPresents);
	if (args.trackDisplay)
		std::fprintf(fp, ",%d,%s,%.*f,%.*f,%.*f,%.*f", int(row.allowsTearing), row.presentMode, kPrec,
		             m.msUntilRenderComplete, kPrec, m.msUntilDisplayed, kPrec, m.msBetweenDisplayChange, kPrec,
		             msFlipDelay);
	if (args.trackGPU)
		std::fprintf(fp, ",%.*f,%.*f", kPrec, m.msUntilRenderStart, kPrec, m.msGpuDuration);
	if (args.trackGPUVideo)
		std::fprintf(fp, ",%.*f", kPrec, m.msVideoDuration);
	if (args.trackInput)
		std::fprintf(fp, ",%.*f", kPrec, m.msSinceInput);
	switch (args.timeUnit)
	{
	case TimeUnit::QPC:
		std::fprintf(fp, ",%llu", static_cast<unsigned long long>(p.presentStartTime));
		break;
	case TimeUnit::QPCMilliSeconds:
		std::fprintf(fp, ",%.*f", kPrec, 0.001 * CsvClock::deltaToMs(p.presentStartTime));
		break;
	default: break;
	}
	if (args.writeDisplayTime)
	{
		if (m.screenTimeQpc == 0)
			std::fputs(",NA", fp);
		else
			std::fprintf(fp, ",%.*f", kPrec, 0.001 * clock.msSinceStart(m.screenTimeQpc));
	}
	if (args.writeFrameId)
		std::fprintf(fp, ",%u", unsigned(p.frameId));
	std::fputs("\n", fp);
}

void writeRowMain(FILE *fp, const CaptureOptions &args, const CsvClock &clock, const std::string &application,
                  const Row &row)
{
	const auto &p = row.frame;
	const auto &m = row.metrics;

	std::fprintf(fp, "%s,%d,0x%llX,%s,%d,%d", csvSafeName(application).c_str(), int(p.processId),
	             static_cast<unsigned long long>(p.swapChainAddress), row.runtime, int(row.syncInterval),
	             int(p.presentFlags));
	if (args.writeDisplayMetadata)
		std::fprintf(fp, ",%u,%u", unsigned(m.displayId.first), unsigned(m.displayId.second));
	if (args.trackDisplay)
		std::fprintf(fp, ",%d,%s", int(row.allowsTearing), row.presentMode);
	if (args.trackFrameType)
		std::fprintf(fp, ",%s", frameTypeToString(m.frameType));
	if (args.trackHybridPresent)
		std::fprintf(fp, ",%d", int(p.isHybridPresent));

	if (!args.useV2Metrics)
	{
		// Time column (the field is called timeInSeconds upstream but holds the present's timestamp)
		switch (args.timeUnit)
		{
		case TimeUnit::DateTime: std::fprintf(fp, ",%s", clock.dateTime(m.timeInSeconds).c_str()); break;
		case TimeUnit::MilliSeconds:
		case TimeUnit::QPCMilliSeconds: std::fprintf(fp, ",%.4f", clock.msSinceStart(m.timeInSeconds)); break;
		case TimeUnit::QPC: std::fprintf(fp, ",%llu", static_cast<unsigned long long>(m.timeInSeconds)); break;
		}

		putNaOr(fp, m.msBetweenSimStarts);                          // MsBetweenSimulationStart
		std::fprintf(fp, ",%.*f", kPrec, m.msBetweenPresents);       // MsBetweenPresents
		if (args.trackDisplay)
		{
			if (m.msBetweenDisplayChange == 0.0)                     // MsBetweenDisplayChange
				std::fputs(",NA", fp);
			else
				std::fprintf(fp, ",%.*f", kPrec, m.msBetweenDisplayChange);
		}
		std::fprintf(fp, ",%.*f", kPrec, m.msInPresentApi);          // MsInPresentAPI
		std::fprintf(fp, ",%.*f", kPrec, m.msUntilRenderComplete);   // MsRenderPresentLatency
		if (args.trackDisplay)
		{
			if (m.msUntilDisplayed == 0.0)                           // MsUntilDisplayed
				std::fputs(",NA", fp);
			else
				std::fprintf(fp, ",%.4f", m.msUntilDisplayed);
			if (args.trackPcLatency)                                 // MsPCLatency (in the header only
				putNaOr(fp, m.msPcLatency);                          // with display tracking)
		}
	}

	// CPUStartTime
	switch (args.timeUnit)
	{
	case TimeUnit::MilliSeconds: std::fprintf(fp, ",%.4f", clock.msSinceStart(m.cpuStartQpc)); break;
	case TimeUnit::QPC: std::fprintf(fp, ",%llu", static_cast<unsigned long long>(m.cpuStartQpc)); break;
	case TimeUnit::QPCMilliSeconds: std::fprintf(fp, ",%.4f", CsvClock::deltaToMs(m.cpuStartQpc)); break;
	case TimeUnit::DateTime: std::fprintf(fp, ",%s", clock.dateTime(m.cpuStartQpc).c_str()); break;
	}

	// MsBetweenAppStart, MsCPUBusy, MsCPUWait
	writeMetricOrZero(fp, m.msCPUBusy + m.msCPUWait);
	writeMetricOrZero(fp, m.msCPUBusy);
	writeMetricOrZero(fp, m.msCPUWait);

	if (args.trackGPU)
		std::fprintf(fp, ",%.4f,%.4f,%.4f,%.4f", m.msGPULatency, m.msGPUBusy + m.msGPUWait, m.msGPUBusy, m.msGPUWait);
	if (args.trackGPUVideo)
		std::fprintf(fp, ",%.4f", m.msVideoBusy);
	if (args.trackDisplay)
	{
		if (args.useV2Metrics)
		{
			if (m.msDisplayedTime == 0.0)
				std::fputs(",NA,NA", fp);
			else
				std::fprintf(fp, ",%.4f,%.4f", m.msDisplayLatency, m.msDisplayedTime);
		}
		putNaOr(fp, m.msAnimationError);
		if (hasValue(m.msAnimationTime))
			std::fprintf(fp, ",%.4f", m.msAnimationTime);
		else if (m.msDisplayedTime == 0.0 ||
		         (m.frameType != FrameType::Application && m.frameType != FrameType::NotSet))
			std::fputs(",NA", fp);
		else
			std::fprintf(fp, ",%.4f", 0.0);
		putNaOr(fp, m.msFlipDelay);
	}
	if (args.trackInput)
	{
		putNaOr(fp, m.msAllInputPhotonLatency);
		putNaOr(fp, m.msClickToPhotonLatency);
	}
	if (args.trackAppTiming)
		putNaOr(fp, m.msInstrumentedLatency);
	if (args.writeDisplayTime)
	{
		if (m.screenTimeQpc == 0)
			std::fputs(",NA", fp);
		else
			std::fprintf(fp, ",%.4f", clock.msSinceStart(m.screenTimeQpc));
	}
	if (args.writeFrameId)
		std::fprintf(fp, ",%u", unsigned(p.frameId));
	if (args.writeDisplayMetadata)
		std::fprintf(fp, ",%llu", static_cast<unsigned long long>(m.presentId));
	if (args.writeFrameId)
	{
		if (args.trackAppTiming)
			std::fprintf(fp, ",%u", unsigned(p.appFrameId));
		if (args.trackPcLatency)
			std::fprintf(fp, ",%u", unsigned(p.pclFrameId));
	}
	std::fputs("\n", fp);
}

} // namespace

void writeCsvRow(FILE *fp, const CaptureOptions &opts, const CsvClock &clock, const std::string &application,
                 const Row &row)
{
	if (opts.useV1Metrics)
		writeRowV1(fp, opts, clock, application, row);
	else
		writeRowMain(fp, opts, clock, application, row);
	if (opts.csvOutput == CsvOutput::Stdout)
		std::fflush(fp);
}

// ---- File names --------------------------------------------------------------------------

std::string generateCsvFilename(const CaptureOptions &opts, const std::string &processName, uint32_t processId,
                                time_t now)
{
	std::string path;
	std::string ext;
	if (!opts.outputCsvFileName.empty())
	{
		// _wsplitpath_s: the extension starts at the last '.' of the file name part.
		const std::string &f = opts.outputCsvFileName;
		size_t slash = f.find_last_of('/');
		size_t dot = f.find_last_of('.');
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash + 1))
		{
			path = f.substr(0, dot);
			ext = f.substr(dot);
		}
		else
			path = f;
	}
	else
	{
		tm lt = {};
		localtime_r(&now, &lt);
		char buf[64];
		std::snprintf(buf, sizeof(buf), "PresentMon-%4d-%02d-%02dT%02d%02d%02d", lt.tm_year + 1900, lt.tm_mon + 1,
		              lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
		path = buf;
		ext = ".csv";
	}

	if (opts.multiCsv)
	{
		if (!processName.empty() && processName != "<unknown>")
		{
			std::string safe = processName;
			for (char &c : safe)
				if (c == '/' || c == '\\')
					c = '_';
			path += "-" + safe;
		}
		path += "-" + std::to_string(processId);
	}
	return path + ext;
}

} // namespace pml::cli
