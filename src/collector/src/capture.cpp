// SPDX-License-Identifier: MIT
#include "capture.hpp"

#include "commands.hpp"
#include "csv.hpp"
#include "log.hpp"

#include <pml/clock.hpp>
#include <pml/metrics/engine.hpp>
#include <pml/process_info.hpp>
#include <pml/shm.hpp>

#include <csignal>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <unistd.h>

namespace pml::cli {

namespace {

volatile std::sig_atomic_t gStopRequested = 0;
void onStopSignal(int) { gStopRequested = 1; }

constexpr uint64_t kNsPerMs = 1000000ull;
constexpr uint64_t kNsPerSec = 1000000000ull;
// Swapchains that have not presented for this long drop out of the console view (upstream: 4 s).
constexpr uint64_t kConsolePruneNs = 4 * kNsPerSec;
constexpr size_t kReadBatch = 256;
// How long after --timed ends the output stays open for records of frames presented before the end.
constexpr uint64_t kWindowEndGraceNs = 250 * kNsPerMs;

bool shmExists(const std::string &name) { return ::access(("/dev/shm/" + name).c_str(), F_OK) == 0; }

// Everything tracked for one swapchain of one process.
struct SwapState
{
	uint64_t handle = 0;
	pml::SwapchainInfo info = {};
	bool infoValid = false;
	std::unique_ptr<pml::metrics::SwapchainEngine> engine;
	const char *presentMode = "Unknown";
	uint64_t lastRecordNs = 0;   // collector time of the last record

	// Console statistics for the current window
	uint64_t winPresented = 0;   // records fed
	uint64_t winRows = 0;
	uint64_t winDisplayed = 0;
	uint64_t winDropped = 0;
	double sumFrameMs = 0;
	uint64_t nFrame = 0;
	double sumCpuMs = 0;
	uint64_t nCpu = 0;
	double sumLatencyMs = 0;
	uint64_t nLatency = 0;
	void resetWindow()
	{
		winPresented = winRows = winDisplayed = winDropped = nFrame = nCpu = nLatency = 0;
		sumFrameMs = sumCpuMs = sumLatencyMs = 0;
	}
};

struct Source
{
	std::string name;
	std::unique_ptr<pml::Segment> segment;
	int pid = 0;
	uint64_t startTicks = 0;
	uint64_t createdNs = 0;
	std::string processName;
	bool tracked = false;         // passes the --process_name/--exclude/--process_id filters
	bool dead = false;            // process gone (final drain done)
	std::unique_ptr<pml::RingReader> reader;
	std::map<uint64_t, SwapState> swaps;   // by swapchainSerial
	uint64_t producerDropped0 = 0;         // header.droppedRecords when attached
	uint64_t lostReported = 0;             // lostRecords() at the last warning
	uint64_t lostReportedNs = 0;           // when that warning was printed
	uint64_t lostRecords() const
	{
		return (reader ? reader->lostRecords() : 0) +
		       (pml::atomicLoad(segment->header().droppedRecords) - producerDropped0);
	}
};

// Output files. One global file, or one per process with --multi_csv. Created lazily when the first
// row arrives, like upstream.
class CsvSink
{
public:
	CsvSink(const CaptureOptions &opts, const CsvClock &clock) : opts_(opts), clock_(clock) {}
	~CsvSink() { closeAll(); }

	// False if the file could not be created or written (the capture then stops).
	bool write(int pid, const std::string &processName, const pml::metrics::Row &row)
	{
		if (opts_.csvOutput == CsvOutput::None)
			return true;
		FILE *&fp = opts_.multiCsv ? perProcess_[pid] : global_;
		if (!fp)
		{
			if (opts_.csvOutput == CsvOutput::Stdout)
				fp = stdout;
			else
			{
				std::string path = generateCsvFilename(opts_, processName, uint32_t(pid), ::time(nullptr));
				fp = std::fopen(path.c_str(), "wb");
				if (!fp)
				{
					PML_LOG_ERROR("cannot create %s: %m", path.c_str());
					return false;
				}
				std::fwrite(kUtf8Bom, 1, sizeof(kUtf8Bom), fp);
				PML_LOG_INFO("writing %s", path.c_str());
				paths_.push_back(path);
			}
			writeCsvHeader(fp, opts_);
		}
		writeCsvRow(fp, opts_, clock_, processName, row);
		++rows_;
		return !std::ferror(fp);
	}

	void closeAll()
	{
		auto close = [](FILE *&fp) {
			if (fp && fp != stdout)
				std::fclose(fp);
			else if (fp)
				std::fflush(fp);
			fp = nullptr;
		};
		close(global_);
		for (auto &kv : perProcess_)
			close(kv.second);
		perProcess_.clear();
	}

	uint64_t rows() const { return rows_; }
	const std::vector<std::string> &paths() const { return paths_; }

private:
	const CaptureOptions &opts_;
	const CsvClock &clock_;
	FILE *global_ = nullptr;
	std::map<int, FILE *> perProcess_;
	uint64_t rows_ = 0;
	std::vector<std::string> paths_;
};

class Capture
{
public:
	explicit Capture(const CaptureOptions &opts)
		: opts_(opts), clock_(CsvClock::start()), sink_(opts, clock_), tty_(::isatty(STDOUT_FILENO) != 0)
	{
		recordStartNs_ = clock_.sessionStartNs + uint64_t(opts.delay) * kNsPerSec;
		recordEndNs_ = recordStartNs_ + uint64_t(opts.timer) * kNsPerSec;
		lastStatsNs_ = clock_.sessionStartNs;
	}

	int run();

private:
	bool isTarget(int pid, const std::string &processName) const;
	void discover(uint64_t now);
	void attach(const std::string &name, uint64_t now);
	void checkSource(Source &s);
	void pump(Source &s, uint64_t now);
	void processRecord(Source &s, const pml::FrameRecord &rec, uint64_t now);
	void handleRows(Source &s, SwapState &sw, const std::vector<pml::metrics::Row> &rows);
	void printStats(uint64_t now);
	bool stale(Source &s, uint64_t now);
	void pruneSwapchains(Source &s, uint64_t now);
	void reportLoss(Source &s, uint64_t now);
	void retire(const Source &s);
	bool inRecordingWindow(uint64_t presentStartNs) const
	{
		return presentStartNs >= recordStartNs_ && !recordingEnded_ &&
		       (!opts_.startTimer || presentStartNs < recordEndNs_);
	}

	const CaptureOptions &opts_;
	CsvClock clock_;
	CsvSink sink_;
	bool tty_;
	uint64_t recordStartNs_ = 0;
	uint64_t recordEndNs_ = 0;
	bool recordingEnded_ = false;
	uint64_t lastStatsNs_ = 0;
	uint64_t lastDiscoveryNs_ = 0;
	int statsLinesDrawn_ = 0;
	bool failed_ = false;
	bool everTracked_ = false;
	std::map<std::string, std::unique_ptr<Source>> sources_;
	uint64_t lostRetired_ = 0;   // records lost by tracked sources no longer in sources_
	std::map<std::string, uint64_t> openFailureSince_;   // name -> first failure time
	std::set<std::string> warnedOpen_;
};

bool Capture::isTarget(int pid, const std::string &processName) const
{
	std::string canon;
	if (!opts_.excludeProcessNames.empty() || !opts_.targetProcessNames.empty())
		canon = canonicalizeProcessName(processName);
	for (const auto &ex : opts_.excludeProcessNames)
		if (ex == canon)
			return false;
	if (opts_.targetPid == 0 && opts_.targetProcessNames.empty())
		return true;
	if (opts_.targetPid != 0 && opts_.targetPid == uint32_t(pid))
		return true;
	for (const auto &t : opts_.targetProcessNames)
		if (t == canon)
			return true;
	return false;
}

// Dead = the process is gone or the layer marked the segment exited.
void Capture::checkSource(Source &s)
{
	if (s.dead)
		return;
	const auto &h = s.segment->header();
	const bool exited = pml::atomicLoad(h.state) == uint32_t(pml::SegmentState::Exited);
	if (exited || !producerAlive(h.pid, h.processStartTicks))
	{
		// Final drain: everything the producer published before it went away.
		if (s.tracked)
			pump(s, pml::monotonicNs());
		s.dead = true;
		if (s.tracked)
			PML_LOG_INFO("%s[%d] exited", s.processName.c_str(), s.pid);
	}
}

// Stale = nobody is going to write to it again. Only dead producers qualify (segmentIsStale
// checks the pid + start time), so live segments are never touched.
bool Capture::stale(Source &s, uint64_t now)
{
	if (!pml::segmentIsStale(*s.segment, now))
		return false;
	s.segment->unlink();
	PML_LOG_INFO("removed stale segment %s (%s[%d] is gone)", s.name.c_str(), s.processName.c_str(), s.pid);
	return true;
}

void Capture::attach(const std::string &name, uint64_t now)
{
	std::string error;
	pml::OpenFailure why = pml::OpenFailure::None;
	auto segment = pml::Segment::open(name, &error, &why);
	if (!segment)
	{
		// A half-created segment of a dead process (the layer died before publishing the magic), or
		// one a dead process left from another protocol version, can never become usable: remove it.
		if (pml::leftoverFailure(why) && pml::removeAbandonedSegmentFile(name))
		{
			PML_LOG_INFO("removed abandoned segment %s (its process is gone and the file cannot be read)", name.c_str());
			openFailureSince_.erase(name);
			return;
		}
		// Still initializing, or not ours: retry on the next sweep. Say so once if it keeps failing.
		auto it = openFailureSince_.emplace(name, now).first;
		if (now - it->second > 10 * kNsPerSec && warnedOpen_.insert(name).second)
			PML_LOG_WARN("cannot open %s: %s", name.c_str(), error.c_str());
		else
			PML_LOG_DEBUG("cannot open %s yet: %s", name.c_str(), error.c_str());
		return;
	}
	openFailureSince_.erase(name);

	auto src = std::make_unique<Source>();
	const auto &h = segment->header();
	src->name = name;
	src->pid = h.pid;
	src->startTicks = h.processStartTicks;
	src->createdNs = h.createdNs;
	src->processName = pml::headerString(h.processName);
	src->segment = std::move(segment);

	// A segment whose producer died without cleaning up: remove it, do not track it.
	if (stale(*src, now))
		return;

	src->tracked = isTarget(src->pid, src->processName);
	if (src->tracked)
	{
		// Capture starts now, like PresentMon: records already in the ring are history. A segment
		// created after the capture began is new, so its first frames are kept.
		const bool newSegment = src->createdNs >= clock_.sessionStartNs;
		src->reader = std::make_unique<pml::RingReader>(
			*src->segment, newSegment ? pml::RingReader::Start::Oldest : pml::RingReader::Start::Newest);
		src->producerDropped0 = pml::atomicLoad(h.droppedRecords);
		everTracked_ = true;
		PML_LOG_INFO("capturing %s[%d] (%s, %s)", src->processName.c_str(), src->pid, name.c_str(),
		             newSegment ? "new" : "from now");
	}
	else
		PML_LOG_DEBUG("ignoring %s[%d] (filtered)", src->processName.c_str(), src->pid);
	sources_[name] = std::move(src);
}

void Capture::discover(uint64_t now)
{
	std::vector<std::string> names;
	if (!opts_.segments.empty())
	{
		for (const auto &n : opts_.segments)
			if (shmExists(n))
				names.push_back(n);
	}
	else
		for (const auto &l : pml::listSegments())
			names.push_back(l.name);

	for (const auto &name : names)
	{
		auto it = sources_.find(name);
		if (it == sources_.end())
		{
			attach(name, now);
			continue;
		}
		Source &s = *it->second;
		if (!s.dead)
		{
			// The name now refers to a different segment (the process re-created it, e.g. after
			// execve, which keeps pid and start time): finish this source and follow the new one.
			if (s.segment->replacedOnDisk())
			{
				PML_LOG_INFO("%s[%d] re-created its segment %s", s.processName.c_str(), s.pid, name.c_str());
				if (s.tracked)
				{
					pump(s, now);
					reportLoss(s, now);
				}
				retire(s);
				sources_.erase(it);
				attach(name, now);
			}
			continue;
		}
		// A dead producer whose name is still listed: either a leftover (clean it up once stale) or a
		// new process that reused the pid (replace the entry).
		std::string error;
		auto fresh = pml::Segment::open(name, &error);
		if (fresh && (fresh->header().createdNs != s.createdNs || fresh->header().processStartTicks != s.startTicks))
		{
			retire(s);
			sources_.erase(it);
			attach(name, now);
			continue;
		}
		if (stale(s, now))
		{
			retire(s);
			sources_.erase(it);
		}
	}

	// Entries whose name is gone from /dev/shm and whose process is gone are history.
	std::set<std::string> present(names.begin(), names.end());
	for (auto it = sources_.begin(); it != sources_.end();)
	{
		if (it->second->dead && !present.count(it->first))
		{
			retire(*it->second);
			it = sources_.erase(it);
		}
		else
			++it;
	}
}

// A loss is worth a warning on its own, not only inside the console block (which --no_console_stats
// and CSV on stdout suppress): the first one at once, growth after that when the count has doubled or
// at most every 10 s.
void Capture::reportLoss(Source &s, uint64_t now)
{
	if (!s.tracked)
		return;
	const uint64_t lost = s.lostRecords();
	if (lost <= s.lostReported)
		return;
	if (s.lostReported != 0 && lost < 2 * s.lostReported && now - s.lostReportedNs < 10 * kNsPerSec)
		return;
	s.lostReported = lost;
	s.lostReportedNs = now;
	PML_LOG_WARN("%s[%d]: %llu records lost so far (ring overrun; rows are missing from the output)",
	             s.processName.c_str(), s.pid, static_cast<unsigned long long>(lost));
}

// A source leaves sources_ (process gone, segment replaced or reused): its losses stay in the final total.
void Capture::retire(const Source &s)
{
	if (s.tracked)
		lostRetired_ += s.lostRecords();
}

// Swapchains that stopped presenting for good (a DXVK game recreates one per resize) are dropped once
// destroyed and idle for 4 s, as upstream drops chains idle for 4 s. A live chain is kept however long
// it is quiet (a loading screen, a paused or minimised game): upstream never prunes a lone presenter's
// chain, and dropping its engine would lose the held row before the pause and the first row after it.
// (The console block hides quiet chains by itself.)
void Capture::pruneSwapchains(Source &s, uint64_t now)
{
	constexpr uint64_t kIdleDestroyedNs = 4 * kNsPerSec;
	for (auto it = s.swaps.begin(); it != s.swaps.end();)
	{
		const SwapState &sw = it->second;
		const uint64_t idle = sw.lastRecordNs != 0 && now > sw.lastRecordNs ? now - sw.lastRecordNs : 0;
		bool prune = false;
		if (idle >= kIdleDestroyedNs)
		{
			// Gone from the table means destroyed too: a slot is only reused 10 s after its swapchain's destroy.
			pml::SwapchainInfo info;
			prune = !pml::findSwapchain(*s.segment, it->first, &info) || info.destroyedNs != 0;
		}
		if (prune)
		{
			PML_LOG_DEBUG("%s[%d]: dropped idle swapchain #%llu", s.processName.c_str(), s.pid,
			              static_cast<unsigned long long>(it->first));
			it = s.swaps.erase(it);
		}
		else
			++it;
	}
}

void Capture::handleRows(Source &s, SwapState &sw, const std::vector<pml::metrics::Row> &rows)
{
	const bool v1 = opts_.useV1Metrics;
	for (const auto &row : rows)
	{
		const auto &m = row.metrics;
		++sw.winRows;
		if (row.dropped)
			++sw.winDropped;
		if (row.hasDisplayMetrics)
			++sw.winDisplayed;
		const double frameMs = v1 ? m.msBetweenPresents : m.msCPUBusy + m.msCPUWait;
		if (row.isAppFrame && frameMs > 0)
		{
			sw.sumFrameMs += frameMs;
			++sw.nFrame;
		}
		if (row.isAppFrame && m.msCPUBusy > 0)
		{
			sw.sumCpuMs += m.msCPUBusy;
			++sw.nCpu;
		}
		const double latency = v1 ? m.msUntilDisplayed : m.msDisplayLatency;
		if (row.hasDisplayMetrics && latency > 0)
		{
			sw.sumLatencyMs += latency;
			++sw.nLatency;
		}

		// The window is decided by the present's own time, not by when the record reached us.
		if (!inRecordingWindow(row.frame.presentStartTime))
			continue;
		if (opts_.excludeDropped && row.frame.finalState != PresentResult::Presented)
			continue;
		if (!sink_.write(s.pid, s.processName, row))
			failed_ = true;
	}
}

void Capture::processRecord(Source &s, const pml::FrameRecord &rec, uint64_t now)
{
	SwapState &sw = s.swaps[rec.swapchainSerial];
	if (!sw.engine)
	{
		sw.handle = rec.swapchainHandle;
		pml::metrics::SwapchainContext ctx;
		sw.infoValid = pml::findSwapchain(*s.segment, rec.swapchainSerial, &sw.info);
		if (sw.infoValid)
			ctx = pml::metrics::SwapchainContext::fromInfo(uint32_t(s.pid), sw.info);
		else
			ctx.processId = uint32_t(s.pid);
		if (ctx.engineName.empty())
			ctx.engineName = pml::headerString(s.segment->header().engineName);
		sw.engine = std::make_unique<pml::metrics::SwapchainEngine>(
			std::move(ctx), opts_.useV1Metrics ? pml::metrics::MetricsMode::V1 : pml::metrics::MetricsMode::V2,
			clock_.sessionStartNs);
	}
	else if (!sw.infoValid && pml::findSwapchain(*s.segment, rec.swapchainSerial, &sw.info))
	{
		// The slot was not published yet when the first record arrived.
		sw.infoValid = true;
		auto ctx = pml::metrics::SwapchainContext::fromInfo(uint32_t(s.pid), sw.info);
		if (ctx.engineName.empty())
			ctx.engineName = pml::headerString(s.segment->header().engineName);
		sw.engine->setContext(std::move(ctx));
	}

	sw.lastRecordNs = now;
	sw.presentMode = pml::metrics::presentModeName(rec.presentMode);
	if (!pml::metrics::isFailedPresent(rec))
		++sw.winPresented;
	handleRows(s, sw, sw.engine->process(rec));
}

void Capture::pump(Source &s, uint64_t now)
{
	if (!s.reader)
		return;
	pml::FrameRecord buf[kReadBatch];
	for (;;)
	{
		size_t n = s.reader->read(buf, kReadBatch);
		if (n == 0)
			break;
		for (size_t i = 0; i < n; ++i)
			processRecord(s, buf[i], now);
	}
}

void Capture::printStats(uint64_t now)
{
	const double dt = double(now - lastStatsNs_) * 1e-9;
	lastStatsNs_ = now;
	std::string out;
	char buf[512];
	int lines = 0;

	for (auto &kv : sources_)
	{
		Source &s = *kv.second;
		if (!s.tracked)
			continue;
		bool headerDone = false;
		for (auto &sk : s.swaps)
		{
			SwapState &sw = sk.second;
			const bool active = sw.lastRecordNs != 0 && now - sw.lastRecordNs < kConsolePruneNs;
			if (active)
			{
				if (!headerDone)
				{
					headerDone = true;
					const uint64_t lost = s.lostRecords();
					if (lost)
						std::snprintf(buf, sizeof(buf), "%s[%d]: (lost %llu records)\n", s.processName.c_str(),
						              s.pid, static_cast<unsigned long long>(lost));
					else
						std::snprintf(buf, sizeof(buf), "%s[%d]:\n", s.processName.c_str(), s.pid);
					out += buf;
					++lines;
				}
				const char *runtime = sw.engine ? pml::metrics::runtimeName(sw.engine->context().engineName) : "Vulkan";
				char latency[32];
				if (sw.nLatency)
					std::snprintf(latency, sizeof(latency), "%.3f ms", sw.sumLatencyMs / double(sw.nLatency));
				else
					std::snprintf(latency, sizeof(latency), "NA");
				std::snprintf(buf, sizeof(buf),
				              "    0x%016llX (%s) %s: Displayed %.1f fps | Presented %.1f fps | Frame %.3f ms | "
				              "CPU busy %.3f ms | Display latency %s | Dropped %.1f%%\n",
				              static_cast<unsigned long long>(sw.handle), runtime, sw.presentMode,
				              double(sw.winDisplayed) / dt, double(sw.winPresented) / dt,
				              sw.nFrame ? sw.sumFrameMs / double(sw.nFrame) : 0.0,
				              sw.nCpu ? sw.sumCpuMs / double(sw.nCpu) : 0.0, latency,
				              sw.winRows ? 100.0 * double(sw.winDropped) / double(sw.winRows) : 0.0);
				out += buf;
				++lines;
			}
			sw.resetWindow();
		}
		if (headerDone)
		{
			out += "\n";
			++lines;
		}
	}

	if (!opts_.consoleStats)
		return;
	if (tty_ && statsLinesDrawn_ > 0)
		std::printf("\x1b[%dA\x1b[J", statsLinesDrawn_);   // redraw in place on a terminal
	std::fputs(out.c_str(), stdout);
	std::fflush(stdout);
	statsLinesDrawn_ = tty_ ? lines : 0;
}

int Capture::run()
{
	struct sigaction sa = {};
	sa.sa_handler = onStopSignal;
	sigemptyset(&sa.sa_mask);
	// Each of these stops the capture and still flushes and closes the CSV (SIGHUP: the terminal was
	// closed). A signal the parent left ignored stays ignored: `nohup presentmon ...` must survive a
	// hangup, and background jobs of non-interactive shells start with SIGINT and SIGQUIT ignored.
	for (int sig : { SIGINT, SIGTERM, SIGHUP, SIGQUIT })
	{
		struct sigaction old = {};
		if (sigaction(sig, nullptr, &old) == 0 && old.sa_handler == SIG_IGN)
			continue;
		sigaction(sig, &sa, nullptr);
	}
	std::signal(SIGPIPE, SIG_IGN);   // a closed CSV pipe is reported through ferror(), not by dying

	uint64_t now = pml::monotonicNs();
	discover(now);
	lastDiscoveryNs_ = now;
	PML_LOG_INFO("capture started (Ctrl+C to stop)%s", opts_.delay ? ", recording after the delay" : "");

	const uint64_t discoveryNs = uint64_t(opts_.discoveryMs) * kNsPerMs;
	const uint64_t statsNs = uint64_t(opts_.statsIntervalMs) * kNsPerMs;
	bool done = false;

	while (!done)
	{
		now = pml::monotonicNs();

		if (now - lastDiscoveryNs_ >= discoveryNs)
		{
			discover(now);
			lastDiscoveryNs_ = now;
			for (auto &kv : sources_)
				if (kv.second->tracked)
					pruneSwapchains(*kv.second, now);
		}

		size_t aliveTracked = 0;
		for (auto &kv : sources_)
		{
			Source &s = *kv.second;
			if (s.tracked && !s.dead)
				pump(s, now);
			if (s.tracked)
				reportLoss(s, now);
			checkSource(s);
			if (s.tracked && !s.dead)
				++aliveTracked;
		}

		if (now - lastStatsNs_ >= statsNs)
			printStats(now);

		if (failed_)
			done = true;
		if (gStopRequested)
		{
			PML_LOG_INFO("stopping");
			done = true;
		}
		// Rows are chosen by their present time (inRecordingWindow); the file closes a little after the
		// end so frames presented just before it, whose records arrive late, still make it in.
		if (opts_.startTimer && now >= recordEndNs_ + kWindowEndGraceNs && !recordingEnded_)
		{
			recordingEnded_ = true;
			sink_.closeAll();   // upstream closes the CSV when the timed recording ends
			if (opts_.terminateAfterTimer)
			{
				PML_LOG_INFO("timed capture finished");
				done = true;
			}
		}
		if (opts_.terminateOnProcExit && everTracked_ && aliveTracked == 0)
		{
			PML_LOG_INFO("all target processes have exited");
			done = true;
		}

		if (!done)
		{
			timespec ts = { 0, 10 * 1000 * 1000 };
			nanosleep(&ts, nullptr);
		}
	}

	// Last read so nothing published before the stop is lost, then close the files.
	now = pml::monotonicNs();
	uint64_t totalLost = lostRetired_;
	for (auto &kv : sources_)
	{
		if (kv.second->tracked && !kv.second->dead)
			pump(*kv.second, now);
		if (kv.second->tracked)
			totalLost += kv.second->lostRecords();
	}
	sink_.closeAll();
	for (const auto &p : sink_.paths())
		PML_LOG_INFO("closed %s", p.c_str());
	if (totalLost)
		PML_LOG_WARN("%llu records were lost (the layer's ring overran before they could be read)",
		             static_cast<unsigned long long>(totalLost));
	PML_LOG_INFO("%llu rows written", static_cast<unsigned long long>(sink_.rows()));
	return failed_ ? 1 : 0;
}

} // namespace

int runCapture(const CaptureOptions &options)
{
	Capture capture(options);
	return capture.run();
}

} // namespace pml::cli
