// SPDX-License-Identifier: MIT
#include "csvreader.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <unordered_map>

namespace pml::gui {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// A frame or display time over an hour is not one: a damaged file, or one in another unit. Left in,
// a single such value turns every average, axis and tile into nonsense.
constexpr double kMaxFrameMs = 3600.0 * 1000.0;

std::string lower(std::string s)
{
	for (char &c : s)
		if (c >= 'A' && c <= 'Z')
			c = char(c - 'A' + 'a');
	return s;
}

// Splits CSV text into records. Quoted fields may contain commas, doubled quotes and line breaks;
// both LF and CRLF end a record. `fields` is reused between calls to avoid reallocating.
class CsvParser
{
public:
	explicit CsvParser(std::string_view text) : s_(text)
	{
		if (s_.size() >= 3 && s_.substr(0, 3) == "\xEF\xBB\xBF")   // UTF-8 BOM (Intel's files have one)
			pos_ = 3;
	}

	// False at the end of the text. Blank lines are skipped.
	bool next(std::vector<std::string> &fields)
	{
		for (;;)
		{
			if (pos_ >= s_.size())
				return false;
			size_t n = 0;
			auto field = [&]() -> std::string & {
				if (n == fields.size())
					fields.emplace_back();
				fields[n].clear();
				return fields[n++];
			};
			std::string *cur = &field();
			bool quoted = false, any = false;
			while (pos_ < s_.size())
			{
				char c = s_[pos_++];
				if (quoted)
				{
					if (c == '"')
					{
						if (pos_ < s_.size() && s_[pos_] == '"')
						{
							cur->push_back('"');
							++pos_;
						}
						else
							quoted = false;
					}
					else
						cur->push_back(c);
					continue;
				}
				// The terminator itself must not count, or a blank line would look like content.
				if (c == '\n')
					break;
				if (c == '\r')
				{
					if (pos_ < s_.size() && s_[pos_] == '\n')
						++pos_;
					break;
				}
				any = true;
				if (c == '"')
					quoted = true;
				else if (c == ',')
					cur = &field();
				else
					cur->push_back(c);
			}
			if (!any && n == 1 && cur->empty())
				continue;   // blank line
			fields.resize(n);
			return true;
		}
	}

private:
	std::string_view s_;
	size_t pos_ = 0;
};

bool isNA(const std::string &s) { return s.empty() || s == "NA" || s == "N/A" || s == "na"; }

// double -> float without undefined behaviour or an infinity: 1e50 is not a frame time.
float toFloat(double v)
{
	if (!std::isfinite(v) || std::fabs(v) > double(std::numeric_limits<float>::max()))
		return float(kNaN);
	return float(v);
}

double parseNumber(const std::string &s)
{
	if (isNA(s))
		return kNaN;
	double v = 0;
	auto r = std::from_chars(s.data(), s.data() + s.size(), v);
	if (r.ec != std::errc() || r.ptr != s.data() + s.size())
	{
		// Leading '+', spaces or unusual forms: fall back to strtod (which also needs the full text).
		char *end = nullptr;
		v = std::strtod(s.c_str(), &end);
		if (end == s.c_str())
			return kNaN;
	}
	return std::isfinite(v) ? v : kNaN;
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d)
{
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = unsigned(y - era * 400);
	const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + int64_t(doe) - 719468;
}

// "2024-5-3 14:05:09.123456789" (our writer) or the same with zero padding (Intel's), as seconds.
double parseDateTime(const std::string &s)
{
	unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0;
	double sec = 0;
	if (std::sscanf(s.c_str(), "%u-%u-%u %u:%u:%lf", &y, &mo, &d, &h, &mi, &sec) != 6)
		return kNaN;
	return double(daysFromCivil(y, mo, d)) * 86400.0 + h * 3600.0 + mi * 60.0 + sec;
}

enum class TimeKind { Seconds, Millis, Ticks, DateTime };

struct TimeColumn
{
	const char *name;   // lower case
	TimeKind kind;
};
// In order of preference. Ticks: our writer emits CLOCK_MONOTONIC nanoseconds under the "QPC"
// names, Intel's files hold QueryPerformanceCounter ticks (usually 10 MHz); both are resolved
// against the frame times below. The "...QPCTime" columns are milliseconds in our writer.
const TimeColumn kTimeColumns[] = {
	{"timeinseconds", TimeKind::Seconds},
	{"timeinms", TimeKind::Millis},
	{"cpustarttime", TimeKind::Millis},
	{"cpustarttimeinms", TimeKind::Millis},
	{"timeinqpc", TimeKind::Ticks},
	{"cpustartqpc", TimeKind::Ticks},
	{"cpustartqpctime", TimeKind::Millis},
	{"cpustartqpctimeinms", TimeKind::Millis},
	{"qpctime", TimeKind::Ticks},
	{"timeindatetime", TimeKind::DateTime},
	{"cpustartdatetime", TimeKind::DateTime},
};

double nominalUnitsPerSecond(TimeKind k)
{
	switch (k)
	{
	case TimeKind::Seconds: return 1;
	case TimeKind::Millis: return 1e3;
	case TimeKind::Ticks: return 1e9;
	case TimeKind::DateTime: return 1;
	}
	return 1;
}

int findColumn(const std::unordered_map<std::string, int> &cols, const char *name)
{
	auto it = cols.find(name);
	return it == cols.end() ? -1 : it->second;
}

struct Row
{
	uint32_t group;
	uint32_t runtime;
	uint32_t mode;
	float frame;
	float display;       // MsBetweenDisplayChange / DisplayedTime as read
	int8_t droppedFlag;  // -1 no Dropped column, else 0 / 1
	double time;         // in the column's own unit
};

// Interns strings into small ints (runtime and present mode names).
struct Interner
{
	std::unordered_map<std::string, uint32_t> ids;
	std::vector<std::string> names;
	uint32_t get(const std::string &s)
	{
		auto it = ids.find(s);
		if (it != ids.end())
			return it->second;
		uint32_t id = uint32_t(names.size());
		ids.emplace(s, id);
		names.push_back(s);
		return id;
	}
};

} // namespace

bool parseCaptureCsv(const std::string &text, CaptureData *out, std::string *error, const std::atomic<bool> *cancel)
{
	auto fail = [&](const char *msg) {
		if (error)
			*error = msg;
		return false;
	};
	if (!out)
		return fail("no output");
	*out = CaptureData{};

	CsvParser parser(text);
	std::vector<std::string> f;
	if (!parser.next(f))
		return fail("The file is empty");

	const size_t headerFields = f.size();
	std::unordered_map<std::string, int> cols;
	std::map<std::string, std::string> spelling;   // lower case -> as written
	for (size_t i = 0; i < f.size(); ++i)
	{
		std::string l = lower(f[i]);
		cols.emplace(l, int(i));
		spelling.emplace(l, f[i]);
	}

	// Frame time: MsBetweenPresents, then FrameTime, then MsBetweenAppStart.
	int frameCol = -1;
	for (const char *n : {"msbetweenpresents", "frametime", "msbetweenappstart"})
	{
		frameCol = findColumn(cols, n);
		if (frameCol >= 0)
		{
			out->frameColumn = spelling[n];
			break;
		}
	}
	if (frameCol < 0)
		return fail("No frame time column (MsBetweenPresents, FrameTime or MsBetweenAppStart) in this file");

	int timeCol = -1;
	TimeKind timeKind = TimeKind::Seconds;
	for (const auto &tc : kTimeColumns)
	{
		timeCol = findColumn(cols, tc.name);
		if (timeCol >= 0)
		{
			timeKind = tc.kind;
			out->timeColumn = spelling[tc.name];
			break;
		}
	}

	// Display: MsBetweenDisplayChange, else derived from the v2 DisplayedTime.
	int displayCol = findColumn(cols, "msbetweendisplaychange");
	bool displayIsV2 = false;
	if (displayCol < 0)
	{
		displayCol = findColumn(cols, "displayedtime");
		displayIsV2 = displayCol >= 0;
	}
	const int droppedCol = findColumn(cols, "dropped");
	const int pidCol = findColumn(cols, "processid");
	const int chainCol = findColumn(cols, "swapchainaddress");
	int runtimeCol = findColumn(cols, "presentruntime");
	if (runtimeCol < 0)
		runtimeCol = findColumn(cols, "runtime");
	const int modeCol = findColumn(cols, "presentmode");
	const int appCol = findColumn(cols, "application");

	struct Group
	{
		std::string key, application, pid;
		size_t rows = 0;
	};
	std::vector<Group> groups;
	std::unordered_map<std::string, uint32_t> groupIds;
	std::vector<Row> rows;
	Interner runtimes, modes;

	auto cell = [&](int col) -> const std::string & {
		static const std::string empty;
		return col >= 0 && size_t(col) < f.size() ? f[size_t(col)] : empty;
	};

	size_t parsed = 0;
	while (parser.next(f))
	{
		if (cancel && (++parsed & 4095) == 0 && cancel->load(std::memory_order_relaxed))
			return fail("cancelled");
		if (f.size() < headerFields)
			continue;   // truncated row (e.g. a capture cut mid-line): not a frame, and no swap chain of its own
		std::string key = cell(pidCol) + "/" + cell(chainCol);
		auto it = groupIds.find(key);
		if (it == groupIds.end())
		{
			it = groupIds.emplace(key, uint32_t(groups.size())).first;
			groups.push_back({key, cell(appCol), cell(pidCol), 0});
		}
		++groups[it->second].rows;

		Row r{};
		r.group = it->second;
		r.runtime = runtimeCol >= 0 ? runtimes.get(cell(runtimeCol)) : 0;
		r.mode = modeCol >= 0 ? modes.get(cell(modeCol)) : 0;
		r.frame = toFloat(parseNumber(cell(frameCol)));
		r.display = displayCol >= 0 ? toFloat(parseNumber(cell(displayCol))) : float(kNaN);
		r.droppedFlag = droppedCol >= 0 ? (parseNumber(cell(droppedCol)) > 0.5 ? 1 : 0) : -1;
		if (timeCol >= 0)
			r.time = timeKind == TimeKind::DateTime ? parseDateTime(cell(timeCol)) : parseNumber(cell(timeCol));
		else
			r.time = kNaN;
		rows.push_back(r);
	}
	if (rows.empty())
		return fail("The file has no data rows");

	// The group with the most rows (the first one on a tie).
	uint32_t best = 0;
	for (uint32_t i = 1; i < groups.size(); ++i)
		if (groups[i].rows > groups[best].rows)
			best = i;
	{
		std::map<std::string, int> pids;
		for (const auto &g : groups)
			pids[g.pid] = 1;
		out->processCount = pids.size();
		out->swapchainCount = groups.size();
		if (groups.size() > 1)
		{
			std::string w = "The file has " + std::to_string(groups.size()) + " swap chains";
			if (pids.size() > 1)
				w += " from " + std::to_string(pids.size()) + " processes";
			w += "; only " + (groups[best].application.empty() ? "process " + groups[best].pid : groups[best].application) +
			     " (PID " + groups[best].pid + ", swap chain " + (groups[best].key.substr(groups[best].key.find('/') + 1)) +
			     ", " + std::to_string(groups[best].rows) + " rows) is shown. Skipped:";
			size_t listed = 0;
			for (uint32_t i = 0; i < groups.size(); ++i)
			{
				if (i == best)
					continue;
				if (listed++ < 5)
					w += " " + (groups[i].application.empty() ? "?" : groups[i].application) + " (PID " + groups[i].pid + ", " +
					     std::to_string(groups[i].rows) + " rows);";
			}
			if (groups.size() - 1 > 5)
				w += " and " + std::to_string(groups.size() - 6) + " more.";
			out->warnings.push_back(w);
		}
	}
	out->application = groups[best].application;

	std::erase_if(rows, [&](const Row &r) { return r.group != best; });
	const size_t n = rows.size();

	{
		size_t absurd = 0;
		for (Row &r : rows)
			for (float *v : {&r.frame, &r.display})
				if (std::isfinite(*v) && std::fabs(*v) > kMaxFrameMs)
				{
					*v = float(kNaN);
					++absurd;
				}
		if (absurd > 0)
			out->warnings.push_back(std::to_string(absurd) + (absurd == 1 ? " frame or display time" : " frame or display times") +
			                        " over one hour " + (absurd == 1 ? "was" : "were") +
			                        " ignored: the file may be damaged, or not in milliseconds.");
	}

	// Most common runtime / present mode.
	auto mostCommon = [&](bool useRuntime, const Interner &in) -> std::string {
		if (in.names.empty())
			return {};
		std::vector<size_t> counts(in.names.size(), 0);
		for (const auto &r : rows)
			++counts[useRuntime ? r.runtime : r.mode];
		size_t bestId = size_t(std::max_element(counts.begin(), counts.end()) - counts.begin());
		return in.names[bestId];
	};
	out->runtime = mostCommon(true, runtimes);
	out->presentMode = mostCommon(false, modes);

	out->frameMs.resize(n);
	double frameSumMs = 0;
	for (size_t i = 0; i < n; ++i)
	{
		out->frameMs[i] = rows[i].frame;
		if (std::isfinite(rows[i].frame) && rows[i].frame > 0)
			frameSumMs += rows[i].frame;
	}

	// ---- Time axis ----
	out->timeSec.assign(n, 0.0);
	bool haveTime = timeCol >= 0;
	if (haveTime)
	{
		size_t valid = 0;
		for (const auto &r : rows)
			valid += std::isfinite(r.time) ? 1 : 0;
		haveTime = valid >= std::max<size_t>(2, n / 2);
	}
	if (haveTime)
	{
		double unitsPerSec = nominalUnitsPerSecond(timeKind);
		double lo = std::numeric_limits<double>::infinity(), hi = -lo;
		for (const auto &r : rows)
			if (std::isfinite(r.time))
			{
				lo = std::min(lo, r.time);
				hi = std::max(hi, r.time);
			}
		// The unit of a time column is not always what its name says: QPC ticks are nanoseconds in
		// our files but 10 MHz in Intel's, and our writer labels some millisecond columns "seconds".
		// The frame times give the real duration, so when the nominal unit is far off, take the
		// unit whose span fits them best.
		const double expectedSec = frameSumMs / 1000.0;
		if (timeKind != TimeKind::DateTime && n >= 10 && expectedSec > 0 && hi > lo)
		{
			auto off = [&](double ups) { return std::fabs(std::log((hi - lo) / ups / expectedSec)); };
			if (off(unitsPerSec) > std::log(3.0))
			{
				double bestUps = unitsPerSec;
				for (double ups : {1.0, 1e3, 1e6, 1e7, 1e9})
					if (off(ups) < off(bestUps))
						bestUps = ups;
				unitsPerSec = bestUps;
			}
		}
		double prev = kNaN, prevFrameMs = 0;
		for (size_t i = 0; i < n; ++i)
		{
			double t = std::isfinite(rows[i].time) ? (rows[i].time - lo) / unitsPerSec : kNaN;
			if (!std::isfinite(t))   // NA in the time column: continue from the previous row
				t = std::isfinite(prev) ? prev + prevFrameMs / 1000.0 : 0.0;
			out->timeSec[i] = t;
			prev = t;
			prevFrameMs = std::isfinite(rows[i].frame) ? rows[i].frame : 0.0;
		}
	}
	else
	{
		out->timeColumn.clear();   // cumulative frame times
		double t = 0;
		for (size_t i = 0; i < n; ++i)
		{
			out->timeSec[i] = t;
			if (std::isfinite(rows[i].frame) && rows[i].frame > 0)
				t += rows[i].frame / 1000.0;
		}
	}

	// ---- Display ----
	out->displayMs.assign(n, float(kNaN));
	if (displayCol >= 0)
	{
		if (!displayIsV2)
		{
			for (size_t i = 0; i < n; ++i)
			{
				float d = rows[i].display;
				if (std::isfinite(d) && d > 0)
					out->displayMs[i] = d;
			}
		}
		else
		{
			// v2 DisplayedTime(i) is how long frame i stayed on screen, i.e. the time from its
			// display change to the next one. MsBetweenDisplayChange(i) is the time since the
			// previous displayed frame changed the screen: the previous displayed row's DisplayedTime.
			float prevShown = float(kNaN);
			for (size_t i = 0; i < n; ++i)
			{
				float d = rows[i].display;
				if (!(std::isfinite(d) && d > 0))
					continue;   // not displayed
				out->displayMs[i] = prevShown;
				prevShown = d;
			}
		}
		for (size_t i = 0; i < n; ++i)
			if (std::isfinite(rows[i].display) && rows[i].display > 0)
			{
				out->hasDisplay = true;
				break;
			}
	}
	if (out->hasDisplay)
	{
		for (size_t i = 0; i < n; ++i)
		{
			bool shown = std::isfinite(rows[i].display) && rows[i].display > 0;
			bool dropped = rows[i].droppedFlag >= 0 ? rows[i].droppedFlag == 1 : !shown;
			out->droppedFrames += dropped ? 1 : 0;
		}
	}
	else if (droppedCol >= 0)
	{
		// No display values at all, but an explicit Dropped column.
		for (size_t i = 0; i < n; ++i)
			out->droppedFrames += rows[i].droppedFlag == 1 ? 1 : 0;
		out->hasDisplay = out->droppedFrames > 0;
	}
	return true;
}

bool readCaptureCsv(const std::string &path, CaptureData *out, std::string *error, const std::atomic<bool> *cancel)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
	{
		if (error)
			*error = "Cannot open " + path;
		return false;
	}
	// One buffer sized from the file: no stream copies on top of the rows.
	in.seekg(0, std::ios::end);
	const std::streamoff size = in.tellg();
	in.seekg(0, std::ios::beg);
	std::string text;
	if (size > 0)
	{
		text.resize(size_t(size));
		in.read(text.data(), size);
		text.resize(size_t(in.gcount()));
	}
	if (in.bad() || size < 0)
	{
		if (error)
			*error = "Cannot read " + path;
		return false;
	}
	return parseCaptureCsv(text, out, error, cancel);
}

CaptureStats computeCaptureStats(const CaptureData &data)
{
	CaptureStats s;
	s.frames = data.frameMs.size();
	s.times = pml::metrics::summarizeFrameTimes(data.frameMs);
	if (s.frames == 0)
		return s;
	if (data.timeColumn.empty())
	{
		for (float v : data.frameMs)
			if (std::isfinite(v) && v > 0)
				s.durationSec += v / 1000.0;
	}
	else
		s.durationSec = data.timeSec.back() - data.timeSec.front();
	if (s.times.count > 0 && s.times.avgMs > 0)
		s.avgFps = 1000.0 / s.times.avgMs;
	if (data.hasDisplay)
	{
		const size_t shown = s.frames > data.droppedFrames ? s.frames - data.droppedFrames : 0;
		if (s.durationSec > 0)
			s.displayFps = double(shown) / s.durationSec;
		s.droppedPct = 100.0 * double(data.droppedFrames) / double(s.frames);
	}
	return s;
}

std::vector<double> frameTimeHistogram(const std::vector<float> &frameMs, double binMs, double maxMs)
{
	if (!(binMs > 0) || !(maxMs > 0) || !std::isfinite(maxMs / binMs))
		return {};
	// The caller asks for about 80 bins; the cap keeps a hostile file from allocating millions (and
	// keeps the double -> size_t conversions below in range).
	constexpr double kMaxBins = 1000;
	const size_t bins = size_t(std::clamp(std::ceil(maxMs / binMs - 1e-9), 1.0, kMaxBins));
	std::vector<double> h(bins, 0.0);
	size_t total = 0;
	for (float v : frameMs)
	{
		if (!std::isfinite(v) || !(v > 0))
			continue;
		const double at = double(v) / binMs;
		const size_t i = at >= double(bins - 1) ? bins - 1 : size_t(at);
		h[i] += 1;
		++total;
	}
	if (total > 0)
		for (double &x : h)
			x = 100.0 * x / double(total);
	return h;
}

} // namespace pml::gui
