// SPDX-License-Identifier: MIT
// Creating, opening and accessing the presentmon-linux shared-memory segment.
// The data layout is defined in protocol.hpp.
#pragma once

#include "pml/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pml {

// "presentmon-linux.<pid>" (shm_open() is called with a leading '/').
std::string segmentNameForPid(int pid);

// Total size of a segment with the given ring capacity.
size_t segmentSize(uint32_t ringCapacity);

// Everything the producer knows about its process when it creates the segment.
struct ProcessIdentity
{
	int32_t pid = 0;
	uint64_t processStartTicks = 0;
	std::string processName;
	std::string exePath;
	std::string engineName;
	std::string applicationName;
	uint32_t engineVersion = 0;
	uint32_t applicationVersion = 0;
	uint32_t apiVersion = 0;
	std::string layerVersion;
};

// Fills ProcessIdentity for the calling process (pid, start ticks, names). The Vulkan
// application/engine fields are left for the caller.
ProcessIdentity currentProcessIdentity();

// Why Segment::open failed (for callers that react differently, e.g. cleanup of dead leftovers).
enum class OpenFailure
{
	None,
	Missing,        // no such name in /dev/shm
	NotOurs,        // not a regular file owned by this user (or root): never trusted
	TooSmall,       // the producer died between ftruncate and the header, or is still creating it
	Initializing,   // mapped, but the magic is not published yet
	Incompatible,   // different protocol version / struct sizes
	InvalidRing,    // ring size is not a power of two or larger than the file
	Other,
};

// Failures a leftover can have: a half-created segment (the producer died before publishing the
// magic), or one from another protocol version. Such a file is removed once its process is gone
// (segmentFileAbandoned); while the process lives it is left alone.
inline bool leftoverFailure(OpenFailure why)
{
	return why == OpenFailure::TooSmall || why == OpenFailure::Initializing || why == OpenFailure::Incompatible ||
	       why == OpenFailure::InvalidRing;
}

// How long a segment whose process cannot be seen alive must have been silent (no present) before
// it counts as stale: for a running collector, and for the reaping each new producer does in
// Segment::create(). The latter is much longer because pids are per pid namespace: a live game in
// another namespace that shares /dev/shm (a sandbox) looks dead from here, and its heartbeat only
// moves when it presents (a loading screen can take a while). Such setups are not supported. When
// /dev/shm is full, create() takes the kStaleAfterNs leftovers too (it could not create its own otherwise).
inline constexpr uint64_t kStaleAfterNs = 10'000'000'000ull;
inline constexpr uint64_t kCreateReapAfterNs = 300'000'000'000ull;

// Bounded copy of a fixed-size header string (producers always terminate, a foreign or damaged
// segment might not).
template <size_t N>
std::string headerString(const char (&field)[N])
{
	size_t n = 0;
	while (n < N && field[n] != '\0')
		++n;
	return std::string(field, n);
}

class Segment
{
public:
	~Segment();
	Segment(const Segment &) = delete;
	Segment &operator=(const Segment &) = delete;

	// Producer: creates /dev/shm/<name> with mode 0600, replacing a stale segment with the same
	// name, initializes the header (overlay control from `initialOverlay`), sets state Live and
	// publishes the magic last. ringCapacity must be a power of two >= 2.
	// The pages are reserved up front (posix_fallocate): if /dev/shm is full, create() fails
	// instead of the producer taking SIGBUS on its first write. Before that it removes the
	// leftovers of dead processes (removeDeadSegments with kCreateReapAfterNs), so crashed games do
	// not pile up; if /dev/shm is still full (ENOSPC), it removes those silent for kStaleAfterNs once
	// and retries.
	static std::unique_ptr<Segment> create(const std::string &name, const ProcessIdentity &identity,
	                                       uint32_t ringCapacity, const OverlayControl &initialOverlay,
	                                       std::string *error = nullptr);

	// Reader: maps an existing segment read-write. Fails (nullptr + error) if it is still
	// initializing, if magic/version/sizes do not match this build, or if the file is not a
	// regular file owned by the caller (or the caller is root). `reason` receives the cause.
	static std::unique_ptr<Segment> open(const std::string &name, std::string *error = nullptr,
	                                     OpenFailure *reason = nullptr);

	// Removes the name from /dev/shm, but only while the name still refers to this segment (a
	// replacement created by a new process image is left alone). Existing mappings (ours and
	// readers') stay valid. A forked child that inherited this segment does nothing.
	void unlink();

	// True if /dev/shm/<name> exists and is a different segment than the one mapped here
	// (the producer re-created it, e.g. after execve). False if it is gone or the same.
	bool replacedOnDisk() const;

	// True in a forked child that inherited this mapping from its parent: the producer functions
	// below then do nothing, so parent and child never write the same ring.
	bool inherited() const;

	const std::string &name() const { return name_; }
	size_t size() const { return size_; }

	SegmentHeader &header() { return *header_; }
	const SegmentHeader &header() const { return *header_; }
	SwapchainSlot *swapchainSlots() { return swapchains_; }
	const SwapchainSlot *swapchainSlots() const { return swapchains_; }
	RingSlot *ringSlots() { return ring_; }
	const RingSlot *ringSlots() const { return ring_; }
	uint32_t ringCapacity() const { return header_->ringCapacity; }

	// Serializes producer-side writers inside one process (ring and swapchain table).
	std::mutex &producerMutex() { return producerMutex_; }

private:
	Segment() = default;
	bool map(int fd, size_t size, std::string *error);

	std::string name_;
	size_t size_ = 0;
	uint64_t dev_ = 0, ino_ = 0;   // identity of the file at map time
	uint64_t forkGeneration_ = 0;
	void *base_ = nullptr;
	SegmentHeader *header_ = nullptr;
	SwapchainSlot *swapchains_ = nullptr;
	RingSlot *ring_ = nullptr;
	std::mutex producerMutex_;
};

// ---- Producer side ----------------------------------------------------------------------

class RingWriter
{
public:
	explicit RingWriter(Segment &segment);

	// Publishes one record. Thread-safe within the process; never waits for readers. When the
	// ring is full the oldest record is overwritten (readers notice and count it as lost).
	void publish(const FrameRecord &record);

private:
	Segment &segment_;
	uint64_t mask_;
};

// Claims a free swapchain slot and publishes `info` (info.serial must be non-zero). When the
// table is full, the slot of the swapchain destroyed longest ago (at least 10 s ago) is reused.
// Returns the slot index, or UINT32_MAX if no slot is available.
uint32_t publishSwapchain(Segment &segment, const SwapchainInfo &info);

// Rewrites an existing slot (e.g. to set destroyedNs).
void updateSwapchain(Segment &segment, uint32_t slot, const SwapchainInfo &info);

void touchHeartbeat(Segment &segment, uint64_t nowNs);
void setState(Segment &segment, SegmentState state);

// ---- Reader side ------------------------------------------------------------------------

class RingReader
{
public:
	enum class Start
	{
		Newest,  // only records published after construction
		Oldest,  // everything still in the ring
	};

	explicit RingReader(const Segment &segment, Start start = Start::Newest);

	// Copies up to maxRecords new records into `out`, in publish order. Never blocks. Records
	// that were overwritten before they could be read, or torn by a concurrent overwrite, are
	// skipped and counted in lostRecords().
	size_t read(FrameRecord *out, size_t maxRecords);

	uint64_t lostRecords() const { return lost_; }
	uint64_t cursor() const { return cursor_; }

private:
	const Segment &segment_;
	uint64_t mask_;
	uint64_t cursor_ = 0;
	uint64_t lost_ = 0;
};

// Consistent copy of a swapchain slot. False if the slot is unused or kept changing.
bool readSwapchain(const Segment &segment, uint32_t slot, SwapchainInfo *out);

// Finds the slot holding `serial` (scans up to swapchainHighWater). False if not found.
bool findSwapchain(const Segment &segment, uint64_t serial, SwapchainInfo *out);

// ---- Overlay control (both sides) ---------------------------------------------------------

// Snapshot of the control area: loads `generation` (acquire), then the fields.
OverlayControl readOverlayControl(const Segment &segment);

// Stores every field except `generation` and `available`, then increments `generation`
// (release). Used by the CLI.
void writeOverlayControl(Segment &segment, const OverlayControl &control);

// Layer only: sets `available`.
void setOverlayAvailable(Segment &segment, bool available);

// ---- Discovery --------------------------------------------------------------------------

struct SegmentListing
{
	std::string name;   // e.g. "presentmon-linux.1234"
	int pid = 0;        // parsed from the name
};

// All presentmon-linux segments currently present in /dev/shm.
std::vector<SegmentListing> listSegments();

// A segment file nobody can ever finish or use: the pid in its name is gone and the file was last
// modified more than `graceNs` ago. Meant for names Segment::open cannot open (leftoverFailure:
// the layer died after shm_open, before publishing the magic, or the file is from another protocol
// version). Only files owned by the caller (or any file for root) qualify.
bool segmentFileAbandoned(const std::string &name, uint64_t graceNs = kStaleAfterNs);

// shm_unlink for segmentFileAbandoned() names. Returns true if the file was removed.
bool removeAbandonedSegmentFile(const std::string &name, uint64_t graceNs = kStaleAfterNs);

// Removes every segment in /dev/shm whose producer is gone: stale ones (segmentIsStale with
// `staleAfterNs`) and abandoned leftovers (segmentFileAbandoned with the same age). `keepName` is
// never touched. Returns how many were removed.
size_t removeDeadSegments(const std::string &keepName = std::string(), uint64_t staleAfterNs = kStaleAfterNs);

// True if the segment's producer is gone: its pid is not alive with the recorded start time
// and the heartbeat is older than `staleAfterNs`, or the header says it exited.
bool segmentIsStale(const Segment &segment, uint64_t nowNs, uint64_t staleAfterNs = kStaleAfterNs);

} // namespace pml
