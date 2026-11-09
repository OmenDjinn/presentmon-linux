// SPDX-License-Identifier: MIT
#include "pml/shm.hpp"

#include "pml/clock.hpp"
#include "pml/process_info.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pml {

namespace {

constexpr uint32_t kSeqlockRetries = 64;

size_t headerBytes()
{
	return sizeof(SegmentHeader);
}

size_t swapchainBytes()
{
	return size_t(kMaxSwapchains) * sizeof(SwapchainSlot);
}

bool isPowerOfTwo(uint32_t v)
{
	return v >= 2 && (v & (v - 1)) == 0;
}

void setError(std::string *error, const std::string &message)
{
	if (error)
		*error = message;
}

void copyString(char *dst, size_t dstSize, const std::string &src)
{
	size_t n = std::min(dstSize - 1, src.size());
	std::memcpy(dst, src.data(), n);
	dst[n] = '\0';
}

// Word-wise relaxed atomic copies, so concurrent readers and writers of a seqlock-protected
// payload never perform a data race in the C++ sense. Sizes are multiples of 8 and both
// sides are 8-byte aligned (static_asserts in protocol.hpp).
void loadWords(void *dst, const void *src, size_t bytes)
{
	auto *d = static_cast<uint64_t *>(dst);
	auto *s = static_cast<const uint64_t *>(src);
	for (size_t i = 0; i < bytes / sizeof(uint64_t); i++)
		d[i] = atomicLoad(s[i], std::memory_order_relaxed);
}

void storeWords(void *dst, const void *src, size_t bytes)
{
	auto *d = static_cast<uint64_t *>(dst);
	auto *s = static_cast<const uint64_t *>(src);
	for (size_t i = 0; i < bytes / sizeof(uint64_t); i++)
		atomicStore(d[i], s[i], std::memory_order_relaxed);
}

// Bumped in every forked child (pthread_atfork). A Segment remembers the value from its creation;
// one that differs was inherited from the parent and must not be written by the child.
std::atomic<uint64_t> gForkGeneration{ 0 };

void registerForkHandler()
{
	static std::once_flag once;
	std::call_once(once, [] {
		::pthread_atfork(nullptr, nullptr, [] { gForkGeneration.fetch_add(1, std::memory_order_relaxed); });
	});
}

bool ownedByUs(const struct stat &st)
{
	return S_ISREG(st.st_mode) && (st.st_uid == ::geteuid() || ::geteuid() == 0);
}

bool pidFromSegmentName(const std::string &name, int *pid)
{
	const size_t prefixLen = std::strlen(kSegmentPrefix);
	if (name.compare(0, prefixLen, kSegmentPrefix) != 0 || name.size() == prefixLen)
		return false;
	char *end = nullptr;
	long v = std::strtol(name.c_str() + prefixLen, &end, 10);
	if (*end != '\0' || v <= 0)
		return false;
	*pid = int(v);
	return true;
}

static_assert(sizeof(FrameRecord) % sizeof(uint64_t) == 0);
static_assert(sizeof(SwapchainInfo) % sizeof(uint64_t) == 0);

} // namespace

std::string segmentNameForPid(int pid)
{
	return std::string(kSegmentPrefix) + std::to_string(pid);
}

size_t segmentSize(uint32_t ringCapacity)
{
	return headerBytes() + swapchainBytes() + size_t(ringCapacity) * sizeof(RingSlot);
}

ProcessIdentity currentProcessIdentity()
{
	ProcessIdentity id;
	id.pid = int32_t(::getpid());
	id.processStartTicks = readProcessStartTicks(id.pid);
	id.exePath = readExePath(id.pid);
	id.processName = detectProcessName(id.pid);
	return id;
}

Segment::~Segment()
{
	if (base_)
		::munmap(base_, size_);
}

bool Segment::map(int fd, size_t size, std::string *error)
{
	void *p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
	{
		setError(error, std::string("mmap failed: ") + std::strerror(errno));
		return false;
	}
	base_ = p;
	size_ = size;
	struct stat st = {};
	if (::fstat(fd, &st) == 0)
	{
		dev_ = uint64_t(st.st_dev);
		ino_ = uint64_t(st.st_ino);
	}
	forkGeneration_ = gForkGeneration.load(std::memory_order_relaxed);
	auto *bytes = static_cast<uint8_t *>(p);
	header_ = reinterpret_cast<SegmentHeader *>(bytes);
	swapchains_ = reinterpret_cast<SwapchainSlot *>(bytes + headerBytes());
	ring_ = reinterpret_cast<RingSlot *>(bytes + headerBytes() + swapchainBytes());
	return true;
}

std::unique_ptr<Segment> Segment::create(const std::string &name, const ProcessIdentity &identity,
                                         uint32_t ringCapacity, const OverlayControl &initialOverlay,
                                         std::string *error, const CapControl *initialCap)
{
	if (!isPowerOfTwo(ringCapacity))
	{
		setError(error, "ring capacity must be a power of two >= 2");
		return nullptr;
	}

	// Crashed or killed games leave their segments behind. Clear them out first, so that a /dev/shm
	// they filled has room for this one (about 5 us per segment, once per process).
	removeDeadSegments(name, kCreateReapAfterNs);

	std::string path = "/" + name;
	const size_t size = segmentSize(ringCapacity);
	int fd = -1;
	for (int attempt = 0;; attempt++)
	{
		fd = ::shm_open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0 && errno == EEXIST)
		{
			// Left behind by an earlier process with the same pid (or a crash). Replace it; readers
			// that still map the old one keep their mapping and see it go stale.
			::shm_unlink(path.c_str());
			fd = ::shm_open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		}
		int rc = 0;
		std::string what;
		if (fd < 0)
		{
			rc = errno;
			what = "shm_open(" + path + ") failed: ";
		}
		else if (::ftruncate(fd, off_t(size)) != 0)
		{
			rc = errno;
			what = "ftruncate failed: ";
		}
		// ftruncate alone makes a sparse file: with /dev/shm full the producer would take SIGBUS on its
		// first write into a ring page. Reserve the pages now so a full /dev/shm fails create() instead.
		else if ((rc = ::posix_fallocate(fd, 0, off_t(size))) != 0)
		{
			what = "cannot reserve " + std::to_string(size) + " bytes in /dev/shm: ";
		}
		if (rc == 0)
			break;
		if (fd >= 0)
		{
			::close(fd);
			::shm_unlink(path.c_str());
		}
		// /dev/shm is full: take the leftovers of dead processes that have been silent for 10 s (the collector's
		// age) as well, once. The longer create-time age only protects live segments of other pid namespaces,
		// and this segment cannot be created otherwise.
		if (rc == ENOSPC && attempt == 0 && removeDeadSegments(name, kStaleAfterNs) > 0)
			continue;
		setError(error, what + std::strerror(rc));
		return nullptr;
	}
	registerForkHandler();

	std::unique_ptr<Segment> segment(new Segment());
	segment->name_ = name;
	bool mapped = segment->map(fd, size, error);
	::close(fd);
	if (!mapped)
	{
		::shm_unlink(path.c_str());
		return nullptr;
	}

	// Fresh shm memory is zero-filled, so only non-zero fields need writing. Nothing below is
	// visible to readers until the magic is published.
	SegmentHeader &h = *segment->header_;
	h.version = kProtocolVersion;
	h.headerSize = sizeof(SegmentHeader);
	h.swapchainSlotSize = sizeof(SwapchainSlot);
	h.ringSlotSize = sizeof(RingSlot);
	h.frameRecordSize = sizeof(FrameRecord);
	h.maxSwapchains = kMaxSwapchains;
	h.ringCapacity = ringCapacity;
	h.pid = identity.pid;
	h.layerBits = uint32_t(sizeof(void *) * 8);
	h.processStartTicks = identity.processStartTicks;
	h.createdNs = monotonicNs();
	copyString(h.processName, sizeof(h.processName), identity.processName);
	copyString(h.exePath, sizeof(h.exePath), identity.exePath);
	copyString(h.engineName, sizeof(h.engineName), identity.engineName);
	copyString(h.applicationName, sizeof(h.applicationName), identity.applicationName);
	h.engineVersion = identity.engineVersion;
	h.applicationVersion = identity.applicationVersion;
	h.apiVersion = identity.apiVersion;
	copyString(h.layerVersion, sizeof(h.layerVersion), identity.layerVersion);

	h.heartbeatNs = h.createdNs;
	h.overlay = initialOverlay;
	h.overlay.generation = 1;
	if (initialCap)
	{
		h.cap.mode = initialCap->mode;
		h.cap.fpsMilli = initialCap->fpsMilli;
		h.cap.placement = initialCap->placement;
		h.cap.flags = initialCap->flags;
	}
	h.cap.generation = 2;   // even: no write in progress (seqlock)
	h.cap.available = 1;
	h.state = uint32_t(SegmentState::Live);

	atomicStore(h.magic, kMagic, std::memory_order_release);

	return segment;
}

std::unique_ptr<Segment> Segment::open(const std::string &name, std::string *error, OpenFailure *reason)
{
	auto fail = [&](OpenFailure why, const std::string &message) -> std::unique_ptr<Segment> {
		if (reason)
			*reason = why;
		setError(error, message);
		return nullptr;
	};
	if (reason)
		*reason = OpenFailure::None;

	std::string path = "/" + name;
	int fd = ::shm_open(path.c_str(), O_RDWR | O_CLOEXEC, 0);
	if (fd < 0)
		return fail(errno == ENOENT ? OpenFailure::Missing : OpenFailure::Other,
		            "shm_open(" + path + ") failed: " + std::strerror(errno));

	struct stat st = {};
	if (::fstat(fd, &st) != 0)
	{
		::close(fd);
		return fail(OpenFailure::Other, "fstat(" + path + ") failed");
	}
	if (!ownedByUs(st))
	{
		::close(fd);
		return fail(OpenFailure::NotOurs, "segment " + name + " is not a regular file owned by this user");
	}
	if (size_t(st.st_size) < headerBytes() + swapchainBytes())
	{
		::close(fd);
		return fail(OpenFailure::TooSmall, "segment " + name + " is too small (still being created?)");
	}

	std::unique_ptr<Segment> segment(new Segment());
	segment->name_ = name;
	bool mapped = segment->map(fd, size_t(st.st_size), error);
	::close(fd);
	if (!mapped)
	{
		if (reason)
			*reason = OpenFailure::Other;
		return nullptr;
	}

	const SegmentHeader &h = *segment->header_;
	if (atomicLoad(h.magic, std::memory_order_acquire) != kMagic)
		return fail(OpenFailure::Initializing, "segment " + name + " is not initialized yet");
	if (h.version != kProtocolVersion || h.headerSize != sizeof(SegmentHeader) ||
	    h.swapchainSlotSize != sizeof(SwapchainSlot) || h.ringSlotSize != sizeof(RingSlot) ||
	    h.frameRecordSize != sizeof(FrameRecord) || h.maxSwapchains != kMaxSwapchains)
	{
		return fail(OpenFailure::Incompatible, "segment " + name + " uses protocol version " +
		                                           std::to_string(h.version) +
		                                           " with different struct sizes; this build expects version " +
		                                           std::to_string(kProtocolVersion));
	}
	if (!isPowerOfTwo(h.ringCapacity) || segment->size_ < segmentSize(h.ringCapacity))
		return fail(OpenFailure::InvalidRing, "segment " + name + " has an invalid ring size");
	return segment;
}

bool Segment::replacedOnDisk() const
{
	struct stat st = {};
	if (::stat(("/dev/shm/" + name_).c_str(), &st) != 0)
		return false;
	return uint64_t(st.st_ino) != ino_ || uint64_t(st.st_dev) != dev_;
}

bool Segment::inherited() const
{
	return forkGeneration_ != gForkGeneration.load(std::memory_order_relaxed);
}

void Segment::unlink()
{
	if (inherited())
		return;
	struct stat st = {};
	if (::stat(("/dev/shm/" + name_).c_str(), &st) != 0)
		return;   // already gone
	if (uint64_t(st.st_ino) != ino_ || uint64_t(st.st_dev) != dev_)
		return;   // the name now belongs to a replacement segment
	::shm_unlink(("/" + name_).c_str());
}

// ---- Producer side ----------------------------------------------------------------------

RingWriter::RingWriter(Segment &segment)
	: segment_(segment), mask_(uint64_t(segment.ringCapacity()) - 1)
{
}

void RingWriter::publish(const FrameRecord &record)
{
	if (segment_.inherited())
		return;   // forked child: the parent's ring is not ours to write
	std::lock_guard<std::mutex> lock(segment_.producerMutex());
	SegmentHeader &h = segment_.header();

	// Only producers (serialized by the mutex above) modify writeSeq.
	uint64_t seq = atomicLoad(h.writeSeq, std::memory_order_relaxed);
	RingSlot &slot = segment_.ringSlots()[seq & mask_];

	atomicStore(slot.seq, kSlotWriting, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	storeWords(&slot.record, &record, sizeof(FrameRecord));
	atomicStore(slot.seq, seq + 1, std::memory_order_release);
	atomicStore(h.writeSeq, seq + 1, std::memory_order_release);
}

namespace {

void writeSwapchainSlot(SwapchainSlot &slot, const SwapchainInfo &info)
{
	uint64_t seq = atomicLoad(slot.seq, std::memory_order_relaxed);
	atomicStore(slot.seq, seq | 1, std::memory_order_relaxed);   // odd: writing
	std::atomic_thread_fence(std::memory_order_release);
	storeWords(&slot.info, &info, sizeof(SwapchainInfo));
	atomicStore(slot.seq, (seq | 1) + 1, std::memory_order_release);  // even: stable
}

} // namespace

uint32_t publishSwapchain(Segment &segment, const SwapchainInfo &info)
{
	if (info.serial == 0 || segment.inherited())
		return UINT32_MAX;

	std::lock_guard<std::mutex> lock(segment.producerMutex());
	SegmentHeader &h = segment.header();
	SwapchainSlot *slots = segment.swapchainSlots();

	uint32_t chosen = UINT32_MAX;
	uint32_t highWater = atomicLoad(h.swapchainHighWater, std::memory_order_relaxed);
	if (highWater < kMaxSwapchains)
	{
		chosen = highWater;
	}
	else
	{
		// Reuse the slot of the swapchain that was destroyed longest ago (>= 10 s).
		uint64_t now = monotonicNs();
		uint64_t oldest = UINT64_MAX;
		for (uint32_t i = 0; i < kMaxSwapchains; i++)
		{
			uint64_t destroyed = slots[i].info.destroyedNs;  // only producers write slots
			if (destroyed != 0 && now - destroyed >= 10'000'000'000ull && destroyed < oldest)
			{
				oldest = destroyed;
				chosen = i;
			}
		}
	}

	if (chosen == UINT32_MAX)
		return UINT32_MAX;

	writeSwapchainSlot(slots[chosen], info);
	if (chosen >= highWater)
		atomicStore(h.swapchainHighWater, chosen + 1, std::memory_order_release);
	return chosen;
}

void updateSwapchain(Segment &segment, uint32_t slot, const SwapchainInfo &info)
{
	if (slot >= kMaxSwapchains || segment.inherited())
		return;
	std::lock_guard<std::mutex> lock(segment.producerMutex());
	writeSwapchainSlot(segment.swapchainSlots()[slot], info);
}

void touchHeartbeat(Segment &segment, uint64_t nowNs)
{
	if (segment.inherited())
		return;
	atomicStore(segment.header().heartbeatNs, nowNs, std::memory_order_relaxed);
}

void setState(Segment &segment, SegmentState state)
{
	if (segment.inherited())
		return;
	atomicStore(segment.header().state, uint32_t(state), std::memory_order_release);
}

// ---- Reader side ------------------------------------------------------------------------

RingReader::RingReader(const Segment &segment, Start start)
	: segment_(segment), mask_(uint64_t(segment.ringCapacity()) - 1)
{
	uint64_t written = atomicLoad(segment.header().writeSeq, std::memory_order_acquire);
	uint64_t capacity = segment.ringCapacity();
	if (start == Start::Newest)
		cursor_ = written;
	else if (written > capacity)
	{
		// Records overwritten before the reader attached are lost too.
		cursor_ = written - capacity;
		lost_ = cursor_;
	}
}

size_t RingReader::read(FrameRecord *out, size_t maxRecords)
{
	const SegmentHeader &h = segment_.header();
	const uint64_t capacity = segment_.ringCapacity();
	uint64_t written = atomicLoad(h.writeSeq, std::memory_order_acquire);

	if (cursor_ > written)
		cursor_ = written;  // producer restarted with a fresh segment under the same mapping (not expected)
	if (written - cursor_ > capacity)
	{
		lost_ += written - cursor_ - capacity;
		cursor_ = written - capacity;
	}

	size_t count = 0;
	while (cursor_ < written && count < maxRecords)
	{
		const RingSlot &slot = segment_.ringSlots()[cursor_ & mask_];
		uint64_t before = atomicLoad(slot.seq, std::memory_order_acquire);
		if (before != cursor_ + 1)
		{
			// Already overwritten by a newer record, or being overwritten right now.
			lost_++;
			cursor_++;
			continue;
		}

		loadWords(&out[count], &slot.record, sizeof(FrameRecord));
		std::atomic_thread_fence(std::memory_order_acquire);
		uint64_t after = atomicLoad(slot.seq, std::memory_order_relaxed);
		cursor_++;

		if (after != before)
		{
			lost_++;   // torn: the producer lapped us while we copied
			continue;
		}
		count++;
	}
	return count;
}

bool readSwapchain(const Segment &segment, uint32_t slot, SwapchainInfo *out)
{
	if (slot >= kMaxSwapchains)
		return false;
	const SwapchainSlot &s = segment.swapchainSlots()[slot];
	for (uint32_t attempt = 0; attempt < kSeqlockRetries; attempt++)
	{
		uint64_t before = atomicLoad(s.seq, std::memory_order_acquire);
		if (before & 1)
			continue;
		SwapchainInfo copy;
		loadWords(&copy, &s.info, sizeof(SwapchainInfo));
		std::atomic_thread_fence(std::memory_order_acquire);
		if (atomicLoad(s.seq, std::memory_order_relaxed) != before)
			continue;
		if (copy.serial == 0)
			return false;
		*out = copy;
		return true;
	}
	return false;
}

bool findSwapchain(const Segment &segment, uint64_t serial, SwapchainInfo *out)
{
	uint32_t highWater = std::min(atomicLoad(segment.header().swapchainHighWater, std::memory_order_acquire),
	                              kMaxSwapchains);
	for (uint32_t i = 0; i < highWater; i++)
	{
		SwapchainInfo info;
		if (readSwapchain(segment, i, &info) && info.serial == serial)
		{
			*out = info;
			return true;
		}
	}
	return false;
}

// ---- Overlay control --------------------------------------------------------------------

OverlayControl readOverlayControl(const Segment &segment)
{
	const OverlayControl &c = segment.header().overlay;
	OverlayControl copy = {};
	copy.generation = atomicLoad(c.generation, std::memory_order_acquire);
	copy.visible = atomicLoad(c.visible, std::memory_order_relaxed);
	copy.position = atomicLoad(c.position, std::memory_order_relaxed);
	copy.scalePermille = atomicLoad(c.scalePermille, std::memory_order_relaxed);
	copy.panels = atomicLoad(c.panels, std::memory_order_relaxed);
	copy.available = atomicLoad(c.available, std::memory_order_relaxed);
	return copy;
}

void writeOverlayControl(Segment &segment, const OverlayControl &control)
{
	OverlayControl &c = segment.header().overlay;
	atomicStore(c.visible, control.visible, std::memory_order_relaxed);
	atomicStore(c.position, control.position, std::memory_order_relaxed);
	atomicStore(c.scalePermille, control.scalePermille, std::memory_order_relaxed);
	atomicStore(c.panels, control.panels, std::memory_order_relaxed);
	atomicFetchAdd(c.generation, 1u, std::memory_order_release);
}

void setOverlayAvailable(Segment &segment, bool available)
{
	if (segment.inherited())
		return;
	atomicStore(segment.header().overlay.available, available ? 1u : 0u, std::memory_order_release);
}

// ---- Frame cap control ------------------------------------------------------------------

namespace {

void loadCapSettings(const CapControl &c, CapControl *out)
{
	out->mode = atomicLoad(c.mode, std::memory_order_relaxed);
	out->fpsMilli = atomicLoad(c.fpsMilli, std::memory_order_relaxed);
	out->placement = atomicLoad(c.placement, std::memory_order_relaxed);
	out->flags = atomicLoad(c.flags, std::memory_order_relaxed);
}

} // namespace

bool tryReadCapSettings(const Segment &segment, CapControl *out)
{
	const CapControl &c = segment.header().cap;
	const uint32_t g = atomicLoad(c.generation, std::memory_order_acquire);
	if (g & 1u)
		return false;
	CapControl copy = *out;
	copy.generation = g;
	loadCapSettings(c, &copy);
	// Seqlock read side: the settings loads stay before the second generation load.
	std::atomic_thread_fence(std::memory_order_acquire);
	if (atomicLoad(c.generation, std::memory_order_relaxed) != g)
		return false;
	*out = copy;
	return true;
}

CapControl readCapControl(const Segment &segment)
{
	const CapControl &c = segment.header().cap;
	CapControl copy = {};
	bool consistent = false;
	for (int attempt = 0; attempt < 64 && !consistent; attempt++)
	{
		consistent = tryReadCapSettings(segment, &copy);
		if (!consistent)
			sched_yield();
	}
	if (!consistent)
	{
		// A writer is stuck mid-write (it died): best effort; its successor repairs the lock.
		copy.generation = atomicLoad(c.generation, std::memory_order_acquire);
		loadCapSettings(c, &copy);
	}
	copy.appliedGeneration = atomicLoad(c.appliedGeneration, std::memory_order_acquire);
	copy.available = atomicLoad(c.available, std::memory_order_relaxed);
	copy.method = atomicLoad(c.method, std::memory_order_relaxed);
	copy.effectiveMilli = atomicLoad(c.effectiveMilli, std::memory_order_relaxed);
	copy.refreshMilli = atomicLoad(c.refreshMilli, std::memory_order_relaxed);
	copy.vrr = atomicLoad(c.vrr, std::memory_order_relaxed);
	copy.meanWaitUs = atomicLoad(c.meanWaitUs, std::memory_order_relaxed);
	copy.lateP99Us = atomicLoad(c.lateP99Us, std::memory_order_relaxed);
	return copy;
}

uint32_t writeCapControl(Segment &segment, const CapControl &control)
{
	CapControl &c = segment.header().cap;
	std::atomic_ref<uint32_t> gen(c.generation);
	uint32_t g = gen.load(std::memory_order_relaxed);
	uint32_t stuckValue = 0;   // the odd value seen, and since when: a live writer changes it within microseconds
	uint64_t stuckSince = 0;
	for (;;)
	{
		if (!(g & 1u))
		{
			if (gen.compare_exchange_weak(g, g + 1, std::memory_order_acquire, std::memory_order_relaxed))
				break;
			continue;
		}
		const uint64_t now = monotonicNs();
		if (g != stuckValue)
		{
			stuckValue = g;
			stuckSince = now;
		}
		else if (now - stuckSince > 50'000'000ull)
		{
			// The same odd value for 50 ms: its writer died between its stores. Take the lock over (a new odd
			// value, so another waiting writer restarts its own 50 ms instead of taking it from us).
			if (gen.compare_exchange_weak(g, g + 2, std::memory_order_acquire, std::memory_order_relaxed))
			{
				g += 1;
				break;
			}
			continue;
		}
		sched_yield();
		g = gen.load(std::memory_order_relaxed);
	}
	// Seqlock write side: the odd generation is visible before any of the stores below.
	std::atomic_thread_fence(std::memory_order_release);
	atomicStore(c.mode, control.mode, std::memory_order_relaxed);
	atomicStore(c.fpsMilli, control.fpsMilli, std::memory_order_relaxed);
	atomicStore(c.placement, control.placement, std::memory_order_relaxed);
	atomicStore(c.flags, control.flags, std::memory_order_relaxed);
	gen.store(g + 2, std::memory_order_release);
	return g + 2;
}

void writeCapStatus(Segment &segment, const CapControl &status)
{
	if (segment.inherited())
		return;
	CapControl &c = segment.header().cap;
	atomicStore(c.available, status.available, std::memory_order_relaxed);
	atomicStore(c.method, status.method, std::memory_order_relaxed);
	atomicStore(c.effectiveMilli, status.effectiveMilli, std::memory_order_relaxed);
	atomicStore(c.refreshMilli, status.refreshMilli, std::memory_order_relaxed);
	atomicStore(c.vrr, status.vrr, std::memory_order_relaxed);
	atomicStore(c.meanWaitUs, status.meanWaitUs, std::memory_order_relaxed);
	atomicStore(c.lateP99Us, status.lateP99Us, std::memory_order_relaxed);
	atomicStore(c.appliedGeneration, status.appliedGeneration, std::memory_order_release);
}

// ---- Discovery --------------------------------------------------------------------------

std::vector<SegmentListing> listSegments()
{
	std::vector<SegmentListing> result;
	DIR *dir = ::opendir("/dev/shm");
	if (!dir)
		return result;

	const size_t prefixLen = std::strlen(kSegmentPrefix);
	while (dirent *entry = ::readdir(dir))
	{
		const char *name = entry->d_name;
		if (std::strncmp(name, kSegmentPrefix, prefixLen) != 0)
			continue;
		const char *digits = name + prefixLen;
		if (*digits == '\0')
			continue;
		char *end = nullptr;
		long pid = std::strtol(digits, &end, 10);
		if (*end != '\0' || pid <= 0)
			continue;
		result.push_back({ name, int(pid) });
	}
	::closedir(dir);

	std::sort(result.begin(), result.end(),
	          [](const SegmentListing &a, const SegmentListing &b) { return a.pid < b.pid; });
	return result;
}

bool segmentIsStale(const Segment &segment, uint64_t nowNs, uint64_t staleAfterNs)
{
	const SegmentHeader &h = segment.header();
	if (atomicLoad(h.state, std::memory_order_acquire) == uint32_t(SegmentState::Exited))
		return true;
	if (processAlive(h.pid, h.processStartTicks))
		return false;
	uint64_t heartbeat = atomicLoad(h.heartbeatNs, std::memory_order_relaxed);
	return nowNs > heartbeat && nowNs - heartbeat > staleAfterNs;
}

bool segmentFileAbandoned(const std::string &name, uint64_t graceNs)
{
	int pid = 0;
	if (!pidFromSegmentName(name, &pid))
		return false;
	struct stat st = {};
	if (::stat(("/dev/shm/" + name).c_str(), &st) != 0 || !ownedByUs(st))
		return false;
	if (processAlive(pid, 0))
		return false;
	timespec now = {};
	::clock_gettime(CLOCK_REALTIME, &now);
	const int64_t newest = st.st_mtim.tv_sec;   // ftruncate and every write move it
	return int64_t(now.tv_sec) - newest >= int64_t(graceNs / 1'000'000'000ull);
}

bool removeAbandonedSegmentFile(const std::string &name, uint64_t graceNs)
{
	if (!segmentFileAbandoned(name, graceNs))
		return false;
	return ::shm_unlink(("/" + name).c_str()) == 0;
}

size_t removeDeadSegments(const std::string &keepName, uint64_t staleAfterNs)
{
	size_t removed = 0;
	const uint64_t now = monotonicNs();
	for (const auto &listing : listSegments())
	{
		if (listing.name == keepName)
			continue;
		OpenFailure why = OpenFailure::None;
		auto segment = Segment::open(listing.name, nullptr, &why);
		if (segment)
		{
			if (segmentIsStale(*segment, now, staleAfterNs))
			{
				segment->unlink();
				++removed;
			}
		}
		else if (leftoverFailure(why) && removeAbandonedSegmentFile(listing.name, staleAfterNs))
			++removed;
	}
	return removed;
}

} // namespace pml
