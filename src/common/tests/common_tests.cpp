// SPDX-License-Identifier: MIT
// Tests for the layer <-> reader contract (src/common).
#include "pml/clock.hpp"
#include "pml/process_info.hpp"
#include "pml/shm.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sched.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace pml;

namespace {

std::string uniqueName(const char *tag)
{
	static int counter = 0;
	return std::string("pml-test.") + tag + "." + std::to_string(::getpid()) + "." + std::to_string(counter++);
}

struct SegmentGuard
{
	std::unique_ptr<Segment> segment;
	~SegmentGuard()
	{
		if (segment)
			segment->unlink();
	}
};

ProcessIdentity testIdentity()
{
	ProcessIdentity id = currentProcessIdentity();
	id.engineName = "TestEngine";
	id.applicationName = "TestApp";
	id.layerVersion = "test";
	return id;
}

FrameRecord makeRecord(uint64_t i)
{
	FrameRecord r = {};
	r.swapchainSerial = 1;
	r.swapchainHandle = 0x1234;
	r.frameIndex = i;
	r.presentId = i * 7 + 1;
	r.presentStartNs = 1'000'000 + i * 1000;
	r.presentEndNs = r.presentStartNs + 50;
	r.screenNs = ~i;   // checksum-like pattern for torn-read detection
	for (uint64_t &word : r.reserved)
		word = i ^ 0x5a5a5a5a5a5a5a5aull;
	return r;
}

bool recordIsConsistent(const FrameRecord &r)
{
	if (r.presentId != r.frameIndex * 7 + 1 || r.screenNs != ~r.frameIndex ||
	    r.presentStartNs != 1'000'000 + r.frameIndex * 1000)
		return false;
	for (uint64_t word : r.reserved)
		if (word != (r.frameIndex ^ 0x5a5a5a5a5a5a5a5aull))
			return false;
	return true;
}

} // namespace

TEST(Clock, MonotonicAndNonZero)
{
	uint64_t a = monotonicNs();
	uint64_t b = monotonicNs();
	EXPECT_GT(a, 0u);
	EXPECT_GE(b, a);
}

TEST(ProcessInfo, Self)
{
	int pid = ::getpid();
	EXPECT_EQ(detectProcessName(pid), "pml_common_tests");
	EXPECT_NE(readProcessStartTicks(pid), 0u);
	EXPECT_TRUE(processAlive(pid, readProcessStartTicks(pid)));
	EXPECT_FALSE(processAlive(pid, readProcessStartTicks(pid) + 1));
	EXPECT_NE(currentThreadId(), 0u);
}

TEST(ProcessInfo, DeadChildIsNotAlive)
{
	pid_t child = ::fork();
	if (child == 0)
		::_exit(0);
	ASSERT_GT(child, 0);
	int status = 0;
	::waitpid(child, &status, 0);
	EXPECT_FALSE(processAlive(child, 0));
}

TEST(ProcessInfo, ZombieChildIsNotAlive)
{
	pid_t child = ::fork();
	if (child == 0)
		::_exit(0);
	ASSERT_GT(child, 0);
	// Wait until the child has exited but leave it unreaped, so it stays a zombie.
	siginfo_t info = {};
	ASSERT_EQ(::waitid(P_PID, id_t(child), &info, WEXITED | WNOWAIT), 0);
	EXPECT_NE(readProcessStartTicks(child), 0u); // still listed in /proc
	EXPECT_FALSE(processAlive(child, 0));
	int status = 0;
	::waitpid(child, &status, 0);
}

TEST(Segment, CreateOpenAndHeader)
{
	SegmentGuard guard;
	OverlayControl overlay = {};
	overlay.visible = 1;
	overlay.position = uint32_t(OverlayPosition::TopRight);
	std::string error;
	std::string name = uniqueName("hdr");
	guard.segment = Segment::create(name, testIdentity(), 64, overlay, &error);
	ASSERT_TRUE(guard.segment) << error;

	auto reader = Segment::open(name, &error);
	ASSERT_TRUE(reader) << error;
	const SegmentHeader &h = reader->header();
	EXPECT_EQ(atomicLoad(h.magic), kMagic);
	EXPECT_EQ(h.version, kProtocolVersion);
	EXPECT_EQ(h.pid, ::getpid());
	EXPECT_EQ(h.ringCapacity, 64u);
	EXPECT_EQ(h.layerBits, uint32_t(sizeof(void *) * 8));
	EXPECT_STREQ(h.processName, "pml_common_tests");
	EXPECT_STREQ(h.engineName, "TestEngine");
	EXPECT_EQ(atomicLoad(h.state), uint32_t(SegmentState::Live));
	EXPECT_EQ(reader->size(), segmentSize(64));

	OverlayControl c = readOverlayControl(*reader);
	EXPECT_EQ(c.visible, 1u);
	EXPECT_EQ(c.position, uint32_t(OverlayPosition::TopRight));
	EXPECT_EQ(c.generation, 1u);

	// It shows up in discovery only for real pid-named segments; ours has a test name.
	EXPECT_FALSE(segmentIsStale(*reader, monotonicNs()));
}

TEST(Segment, RejectsBadCapacityAndMissing)
{
	std::string error;
	EXPECT_FALSE(Segment::create(uniqueName("bad"), testIdentity(), 100, {}, &error));
	EXPECT_FALSE(Segment::open("pml-test.does-not-exist", &error));
	EXPECT_FALSE(error.empty());
}

TEST(Ring, PublishAndReadInOrder)
{
	SegmentGuard guard;
	std::string name = uniqueName("ring");
	guard.segment = Segment::create(name, testIdentity(), 64, {}, nullptr);
	ASSERT_TRUE(guard.segment);

	RingReader reader(*guard.segment, RingReader::Start::Newest);
	RingWriter writer(*guard.segment);
	for (uint64_t i = 1; i <= 10; i++)
		writer.publish(makeRecord(i));

	FrameRecord out[32];
	size_t n = reader.read(out, 32);
	ASSERT_EQ(n, 10u);
	for (size_t i = 0; i < n; i++)
	{
		EXPECT_EQ(out[i].frameIndex, i + 1);
		EXPECT_TRUE(recordIsConsistent(out[i]));
	}
	EXPECT_EQ(reader.lostRecords(), 0u);
	EXPECT_EQ(reader.read(out, 32), 0u);
}

TEST(Ring, OverwriteCountsLost)
{
	SegmentGuard guard;
	guard.segment = Segment::create(uniqueName("wrap"), testIdentity(), 16, {}, nullptr);
	ASSERT_TRUE(guard.segment);

	RingWriter writer(*guard.segment);
	for (uint64_t i = 1; i <= 100; i++)
		writer.publish(makeRecord(i));

	RingReader reader(*guard.segment, RingReader::Start::Oldest);
	FrameRecord out[64];
	size_t n = reader.read(out, 64);
	ASSERT_EQ(n, 16u);
	EXPECT_EQ(out[0].frameIndex, 85u);
	EXPECT_EQ(out[15].frameIndex, 100u);

	// A reader that started at the beginning and fell behind.
	RingReader late(*guard.segment, RingReader::Start::Newest);
	for (uint64_t i = 101; i <= 140; i++)
		writer.publish(makeRecord(i));
	n = late.read(out, 64);
	EXPECT_EQ(n, 16u);
	EXPECT_EQ(late.lostRecords(), 24u);
	EXPECT_EQ(out[0].frameIndex, 125u);
}

TEST(Ring, ConcurrentWriterReaderNeverTears)
{
	SegmentGuard guard;
	guard.segment = Segment::create(uniqueName("race"), testIdentity(), 256, {}, nullptr);
	ASSERT_TRUE(guard.segment);

	constexpr uint64_t kTotal = 400000;
	RingReader reader(*guard.segment, RingReader::Start::Newest);
	std::atomic<bool> done{false};

	std::thread producer([&] {
		RingWriter writer(*guard.segment);
		for (uint64_t i = 1; i <= kTotal; i++)
			writer.publish(makeRecord(i));
		done = true;
	});

	uint64_t received = 0, lastIndex = 0;
	bool consistent = true, ordered = true;
	FrameRecord out[64];
	for (;;)
	{
		bool finished = done.load();
		size_t n = reader.read(out, 64);
		for (size_t i = 0; i < n; i++)
		{
			consistent = consistent && recordIsConsistent(out[i]);
			ordered = ordered && out[i].frameIndex > lastIndex;
			lastIndex = out[i].frameIndex;
		}
		received += n;
		if (finished && n == 0)
			break;
	}
	producer.join();

	EXPECT_TRUE(consistent);
	EXPECT_TRUE(ordered);
	EXPECT_EQ(received + reader.lostRecords(), kTotal);
	EXPECT_EQ(lastIndex, kTotal);
}

TEST(Ring, CrossProcess)
{
	SegmentGuard guard;
	std::string name = uniqueName("xproc");
	guard.segment = Segment::create(name, testIdentity(), 1024, {}, nullptr);
	ASSERT_TRUE(guard.segment);

	pid_t child = ::fork();
	if (child == 0)
	{
		// Child: reader + control writer.
		auto seg = Segment::open(name, nullptr);
		if (!seg)
			::_exit(10);
		RingReader reader(*seg, RingReader::Start::Oldest);
		FrameRecord out[128];
		uint64_t got = 0;
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (got < 500 && std::chrono::steady_clock::now() < deadline)
		{
			size_t n = reader.read(out, 128);
			for (size_t i = 0; i < n; i++)
				if (!recordIsConsistent(out[i]) || out[i].frameIndex != got + i + 1)
					::_exit(11);
			got += n;
			if (n == 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (got != 500)
			::_exit(12);
		OverlayControl c = readOverlayControl(*seg);
		c.visible = 0;
		c.position = uint32_t(OverlayPosition::BottomLeft);
		writeOverlayControl(*seg, c);
		::_exit(0);
	}
	ASSERT_GT(child, 0);

	RingWriter writer(*guard.segment);
	for (uint64_t i = 1; i <= 500; i++)
		writer.publish(makeRecord(i));

	int status = 0;
	::waitpid(child, &status, 0);
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);

	OverlayControl c = readOverlayControl(*guard.segment);
	EXPECT_EQ(c.generation, 2u);
	EXPECT_EQ(c.visible, 0u);
	EXPECT_EQ(c.position, uint32_t(OverlayPosition::BottomLeft));
}

TEST(Swapchains, PublishFindUpdateReuse)
{
	SegmentGuard guard;
	guard.segment = Segment::create(uniqueName("swap"), testIdentity(), 16, {}, nullptr);
	ASSERT_TRUE(guard.segment);
	Segment &seg = *guard.segment;

	SwapchainInfo info = {};
	info.serial = 1;
	info.handle = 0xabc;
	info.width = 2560;
	info.height = 1440;
	std::snprintf(info.engineName, sizeof(info.engineName), "DXVK");
	uint32_t slot = publishSwapchain(seg, info);
	ASSERT_EQ(slot, 0u);

	SwapchainInfo found = {};
	ASSERT_TRUE(findSwapchain(seg, 1, &found));
	EXPECT_EQ(found.width, 2560u);
	EXPECT_STREQ(found.engineName, "DXVK");
	EXPECT_FALSE(findSwapchain(seg, 2, &found));

	info.destroyedNs = monotonicNs();
	updateSwapchain(seg, slot, info);
	ASSERT_TRUE(readSwapchain(seg, slot, &found));
	EXPECT_EQ(found.destroyedNs, info.destroyedNs);

	// Fill the table; with nothing destroyed >= 10 s ago, the next publish fails.
	for (uint64_t s = 2; s <= kMaxSwapchains; s++)
	{
		SwapchainInfo more = {};
		more.serial = s;
		ASSERT_NE(publishSwapchain(seg, more), UINT32_MAX);
	}
	SwapchainInfo extra = {};
	extra.serial = 1000;
	EXPECT_EQ(publishSwapchain(seg, extra), UINT32_MAX);

	// Pretend slot 0 was destroyed long ago: it gets reused.
	info.destroyedNs = monotonicNs() - 20'000'000'000ull;
	updateSwapchain(seg, slot, info);
	EXPECT_EQ(publishSwapchain(seg, extra), slot);
	ASSERT_TRUE(findSwapchain(seg, 1000, &found));
	EXPECT_EQ(atomicLoad(seg.header().swapchainHighWater), kMaxSwapchains);
}

TEST(Discovery, ListsPidNamedSegments)
{
	// Create a segment the way the layer does (pid-based name) and make sure discovery and
	// staleness checks work on it.
	SegmentGuard guard;
	std::string name = segmentNameForPid(::getpid());
	guard.segment = Segment::create(name, testIdentity(), 16, {}, nullptr);
	ASSERT_TRUE(guard.segment);

	bool listed = false;
	for (const auto &entry : listSegments())
		if (entry.name == name && entry.pid == ::getpid())
			listed = true;
	EXPECT_TRUE(listed);
	EXPECT_FALSE(segmentIsStale(*guard.segment, monotonicNs()));

	setState(*guard.segment, SegmentState::Exited);
	EXPECT_TRUE(segmentIsStale(*guard.segment, monotonicNs()));
}

// ---- H1: a full /dev/shm fails create() instead of killing the producer with SIGBUS --------------

TEST(SegmentCreate, FullDevShmFailsInsteadOfSigbus)
{
	// Private user + mount namespace with a 1 MB tmpfs on /dev/shm; nothing outside the child changes.
	pid_t child = ::fork();
	ASSERT_GE(child, 0);
	if (child == 0)
	{
		uid_t uid = ::getuid();
		gid_t gid = ::getgid();
		if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0)
			::_exit(77);
		auto writeFile = [](const char *path, const std::string &text) {
			int fd = ::open(path, O_WRONLY);
			if (fd < 0)
				return false;
			bool ok = ::write(fd, text.data(), text.size()) == ssize_t(text.size());
			::close(fd);
			return ok;
		};
		if (!writeFile("/proc/self/setgroups", "deny") ||
		    !writeFile("/proc/self/uid_map", "0 " + std::to_string(uid) + " 1") ||
		    !writeFile("/proc/self/gid_map", "0 " + std::to_string(gid) + " 1"))
			::_exit(77);
		if (::mount("tmpfs", "/dev/shm", "tmpfs", 0, "size=1m") != 0)
			::_exit(77);

		std::string error;
		auto segment = Segment::create("pml-test.enospc", testIdentity(), 8192, OverlayControl{}, &error);
		if (segment)
			::_exit(10);   // would have been a sparse file
		if (error.find("reserve") == std::string::npos)
			::_exit(11);
		if (std::filesystem::exists("/dev/shm/pml-test.enospc"))
			::_exit(12);   // the half-made file must be removed
		::_exit(0);
	}
	int status = 0;
	ASSERT_EQ(::waitpid(child, &status, 0), child);
	ASSERT_TRUE(WIFEXITED(status)) << "status " << status;
	if (WEXITSTATUS(status) == 77)
		GTEST_SKIP() << "cannot create a user + mount namespace here";
	EXPECT_EQ(WEXITSTATUS(status), 0);
}

// ---- M2: unlink / replacement are tracked by inode ---------------------------------------------

TEST(SegmentReplace, UnlinkOnlyRemovesTheSegmentItMapped)
{
	const std::string name = uniqueName("replace");
	std::string error;
	auto first = Segment::create(name, testIdentity(), 64, OverlayControl{}, &error);
	ASSERT_TRUE(first) << error;
	EXPECT_FALSE(first->replacedOnDisk());

	// The same name re-created (what a new process image with the same pid does).
	auto second = Segment::create(name, testIdentity(), 64, OverlayControl{}, &error);
	ASSERT_TRUE(second) << error;
	EXPECT_TRUE(first->replacedOnDisk());
	EXPECT_FALSE(second->replacedOnDisk());

	first->unlink();   // must not take the replacement's name away
	EXPECT_TRUE(std::filesystem::exists("/dev/shm/" + name));
	second->unlink();
	EXPECT_FALSE(std::filesystem::exists("/dev/shm/" + name));
	EXPECT_FALSE(second->replacedOnDisk());   // gone is not "replaced"
}

// ---- L6: a forked child never writes the parent's segment --------------------------------------

TEST(SegmentFork, ChildDoesNotWriteTheParentsRing)
{
	SegmentGuard guard;
	std::string error;
	guard.segment = Segment::create(uniqueName("fork"), testIdentity(), 64, OverlayControl{}, &error);
	ASSERT_TRUE(guard.segment) << error;
	RingWriter writer(*guard.segment);
	writer.publish(makeRecord(1));
	EXPECT_FALSE(guard.segment->inherited());

	pid_t child = ::fork();
	ASSERT_GE(child, 0);
	if (child == 0)
	{
		if (!guard.segment->inherited())
			::_exit(10);
		for (uint64_t i = 2; i < 40; ++i)
			writer.publish(makeRecord(i));
		SwapchainInfo info = {};
		info.serial = 9;
		if (publishSwapchain(*guard.segment, info) != UINT32_MAX)
			::_exit(11);
		setState(*guard.segment, SegmentState::Exited);
		touchHeartbeat(*guard.segment, 1);
		guard.segment->unlink();
		::_exit(0);
	}
	int status = 0;
	ASSERT_EQ(::waitpid(child, &status, 0), child);
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);

	const SegmentHeader &h = guard.segment->header();
	EXPECT_EQ(atomicLoad(h.writeSeq), 1u);
	EXPECT_EQ(atomicLoad(h.state), uint32_t(SegmentState::Live));
	EXPECT_NE(atomicLoad(h.heartbeatNs), 1u);
	EXPECT_EQ(atomicLoad(h.swapchainHighWater), 0u);
	EXPECT_TRUE(std::filesystem::exists("/dev/shm/" + guard.segment->name()));
	writer.publish(makeRecord(2));   // the parent still can
	EXPECT_EQ(atomicLoad(h.writeSeq), 2u);
}

// ---- M5: records overwritten before a reader attached count as lost ---------------------------

TEST(RingReaderLost, OldestStartCountsRecordsOverwrittenBeforeAttach)
{
	SegmentGuard guard;
	std::string error;
	guard.segment = Segment::create(uniqueName("oldest"), testIdentity(), 8, OverlayControl{}, &error);
	ASSERT_TRUE(guard.segment) << error;
	RingWriter writer(*guard.segment);
	for (uint64_t i = 1; i <= 20; ++i)
		writer.publish(makeRecord(i));
	RingReader reader(*guard.segment, RingReader::Start::Oldest);
	EXPECT_EQ(reader.lostRecords(), 12u);
	FrameRecord out[16];
	EXPECT_EQ(reader.read(out, 16), 8u);
	EXPECT_EQ(reader.lostRecords(), 12u);

	RingReader newest(*guard.segment, RingReader::Start::Newest);
	EXPECT_EQ(newest.lostRecords(), 0u);   // history before a "from now" attach is not a loss
}

// ---- M1 / K3: leftovers that cannot be opened ---------------------------------------------------

namespace {

pid_t deadPid()
{
	pid_t child = ::fork();
	if (child == 0)
		::_exit(0);
	int st = 0;
	::waitpid(child, &st, 0);
	return child;
}

// A /dev/shm file as the layer leaves it when killed between shm_open and publishing the magic.
std::string makeHalfCreated(pid_t pid, size_t bytes, int ageSeconds)
{
	std::string name = segmentNameForPid(pid);
	int fd = ::shm_open(("/" + name).c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
	EXPECT_GE(fd, 0);
	if (fd >= 0)
	{
		EXPECT_EQ(::ftruncate(fd, off_t(bytes)), 0);
		::close(fd);
		timeval tv[2];
		tv[0].tv_sec = tv[1].tv_sec = ::time(nullptr) - ageSeconds;
		tv[0].tv_usec = tv[1].tv_usec = 0;
		::utimes(("/dev/shm/" + name).c_str(), tv);
	}
	return name;
}

} // namespace

TEST(SegmentOpen, FailureReasons)
{
	OpenFailure why = OpenFailure::None;
	std::string error;
	EXPECT_FALSE(Segment::open("presentmon-linux.2999998", &error, &why));
	EXPECT_EQ(why, OpenFailure::Missing);

	pid_t pid = deadPid();
	std::string name = makeHalfCreated(pid, 100, 60);
	EXPECT_FALSE(Segment::open(name, &error, &why));
	EXPECT_EQ(why, OpenFailure::TooSmall);
	::shm_unlink(("/" + name).c_str());

	name = makeHalfCreated(pid, segmentSize(64), 60);
	EXPECT_FALSE(Segment::open(name, &error, &why));
	EXPECT_EQ(why, OpenFailure::Initializing);
	::shm_unlink(("/" + name).c_str());

	std::string live = uniqueName("reason");
	SegmentGuard guard;
	guard.segment = Segment::create(live, testIdentity(), 64, OverlayControl{}, &error);
	ASSERT_TRUE(guard.segment) << error;
	why = OpenFailure::TooSmall;
	EXPECT_TRUE(Segment::open(live, &error, &why));
	EXPECT_EQ(why, OpenFailure::None);
}

TEST(SegmentCleanup, AbandonedHalfCreatedFilesAreRecognised)
{
	pid_t pid = deadPid();
	// Old file of a dead pid: abandoned. Fresh one: not yet (the grace period).
	std::string old = makeHalfCreated(pid, segmentSize(64), 60);
	EXPECT_TRUE(segmentFileAbandoned(old));
	::shm_unlink(("/" + old).c_str());
	std::string fresh = makeHalfCreated(pid, segmentSize(64), 0);
	EXPECT_FALSE(segmentFileAbandoned(fresh));
	EXPECT_TRUE(segmentFileAbandoned(fresh, 0));
	EXPECT_FALSE(removeAbandonedSegmentFile(fresh));
	EXPECT_TRUE(std::filesystem::exists("/dev/shm/" + fresh));
	EXPECT_TRUE(removeAbandonedSegmentFile(fresh, 0));
	EXPECT_FALSE(std::filesystem::exists("/dev/shm/" + fresh));

	// The pid is alive (this process): never abandoned, however old.
	std::string mine = makeHalfCreated(::getpid(), segmentSize(64), 600);
	EXPECT_FALSE(segmentFileAbandoned(mine));
	::shm_unlink(("/" + mine).c_str());

	EXPECT_FALSE(segmentFileAbandoned("not-ours.123"));
	EXPECT_FALSE(segmentFileAbandoned("presentmon-linux.2999997"));   // missing
}

// ---- M3: a new segment clears out the leftovers of dead processes ------------------------------

TEST(SegmentCleanup, CreateRemovesDeadLeftoversButNotLiveOrFreshOnes)
{
	pid_t deadA = deadPid();
	std::string error;

	// 1: a complete segment of a dead pid whose heartbeat is long gone
	ProcessIdentity id = testIdentity();
	id.pid = deadA;
	id.processStartTicks = 12345;
	std::string staleName = segmentNameForPid(deadA);
	auto stale = Segment::create(staleName, id, 64, OverlayControl{}, &error);
	ASSERT_TRUE(stale) << error;
	touchHeartbeat(*stale, 1);

	// 2: a half-created file of another dead pid (old)
	pid_t deadB = deadPid();
	while (deadB == deadA)
		deadB = deadPid();
	std::string halfOld = makeHalfCreated(deadB, segmentSize(64), 60);

	// 3: a segment of a dead pid with a fresh heartbeat: kept for now
	pid_t deadC = deadPid();
	while (deadC == deadA || deadC == deadB)
		deadC = deadPid();
	ProcessIdentity idC = testIdentity();
	idC.pid = deadC;
	idC.processStartTicks = 12345;
	auto recent = Segment::create(segmentNameForPid(deadC), idC, 64, OverlayControl{}, &error);
	ASSERT_TRUE(recent) << error;
	touchHeartbeat(*recent, monotonicNs());

	// 4: a live one (this process)
	auto live = Segment::create(segmentNameForPid(::getpid()), currentProcessIdentity(), 64, OverlayControl{}, &error);
	ASSERT_TRUE(live) << error;

	// The creation of any new segment reaps (1) and (2) only; (1) may already be gone from the creates above.
	auto trigger = Segment::create(uniqueName("trigger"), testIdentity(), 64, OverlayControl{}, &error);
	ASSERT_TRUE(trigger) << error;
	removeDeadSegments(trigger->name());   // explicit call: same effect
	EXPECT_FALSE(std::filesystem::exists("/dev/shm/" + staleName));
	EXPECT_FALSE(std::filesystem::exists("/dev/shm/" + halfOld));
	EXPECT_TRUE(std::filesystem::exists("/dev/shm/" + segmentNameForPid(deadC)));
	EXPECT_TRUE(std::filesystem::exists("/dev/shm/" + segmentNameForPid(::getpid())));

	recent->unlink();
	live->unlink();
	trigger->unlink();
	::shm_unlink(("/" + halfOld).c_str());
}

TEST(SegmentCleanup, CreateReapsOnItsOwn)
{
	pid_t dead = deadPid();
	// Older than create()'s reaping age (kCreateReapAfterNs, 5 minutes).
	std::string name = makeHalfCreated(dead, segmentSize(64), 600);
	ASSERT_TRUE(std::filesystem::exists("/dev/shm/" + name));
	std::string error;
	auto trigger = Segment::create(segmentNameForPid(::getpid()), currentProcessIdentity(), 64, OverlayControl{}, &error);
	ASSERT_TRUE(trigger) << error;
	EXPECT_FALSE(std::filesystem::exists("/dev/shm/" + name)) << "create() should have removed the abandoned file";
	trigger->unlink();
}

// NF8: pids are per pid namespace, so a live game in another namespace looks dead from here. create(),
// which runs in every layer-enabled process, only takes segments silent for kCreateReapAfterNs; the
// collector's own rule (kStaleAfterNs) is shorter.
TEST(SegmentCleanup, CreateKeepsRecentlySilentSegmentsOfInvisibleProcesses)
{
	pid_t invisible = deadPid();
	ProcessIdentity id = testIdentity();
	id.pid = invisible;
	id.processStartTicks = 12345;
	std::string error;
	const std::string name = segmentNameForPid(invisible);
	auto other = Segment::create(name, id, 64, OverlayControl{}, &error);
	ASSERT_TRUE(other) << error;
	touchHeartbeat(*other, monotonicNs() - 60 * 1'000'000'000ull);   // silent for a minute (a loading screen)

	auto trigger = Segment::create(uniqueName("trigger"), testIdentity(), 64, OverlayControl{}, &error);
	ASSERT_TRUE(trigger) << error;
	EXPECT_TRUE(std::filesystem::exists("/dev/shm/" + name)) << "create() reaped a segment silent for only a minute";
	removeDeadSegments(trigger->name());   // the collector's age
	EXPECT_FALSE(std::filesystem::exists("/dev/shm/" + name));
	trigger->unlink();
}

// M3: a dead process's segment from another protocol version is removed like a half-created one; a live
// process's is kept.
TEST(SegmentCleanup, OtherVersionLeftoversOfDeadProcessesAreRemoved)
{
	auto makeOtherVersion = [](pid_t pid, int ageSeconds) {
		std::string error;
		const std::string name = segmentNameForPid(pid);
		ProcessIdentity id = testIdentity();
		id.pid = pid;
		auto seg = Segment::create(name, id, 64, OverlayControl{}, &error);
		EXPECT_TRUE(seg) << error;
		if (seg)
			seg->header().version = kProtocolVersion + 1000;   // as an older or newer layer would have it
		seg.reset();
		timeval tv[2];
		tv[0].tv_sec = tv[1].tv_sec = ::time(nullptr) - ageSeconds;
		tv[0].tv_usec = tv[1].tv_usec = 0;
		::utimes(("/dev/shm/" + name).c_str(), tv);
		return name;
	};
	const std::string dead = makeOtherVersion(deadPid(), 60);
	OpenFailure why = OpenFailure::None;
	EXPECT_FALSE(Segment::open(dead, nullptr, &why));
	EXPECT_EQ(why, OpenFailure::Incompatible);
	EXPECT_TRUE(leftoverFailure(why));
	const std::string live = makeOtherVersion(::getpid(), 600);

	EXPECT_GE(removeDeadSegments(), 1u);
	EXPECT_FALSE(std::filesystem::exists("/dev/shm/" + dead));
	EXPECT_TRUE(std::filesystem::exists("/dev/shm/" + live)) << "the process is alive: its segment stays";
	::shm_unlink(("/" + live).c_str());
}

// NF4: dead leftovers that filled /dev/shm are removed before the new segment's pages are reserved.
TEST(SegmentCreate, DevShmFullOfDeadLeftoversHealsItself)
{
	// Private user + mount namespace with a 6 MB tmpfs on /dev/shm; nothing outside the child changes.
	pid_t child = ::fork();
	ASSERT_GE(child, 0);
	if (child == 0)
	{
		uid_t uid = ::getuid();
		gid_t gid = ::getgid();
		if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0)
			::_exit(77);
		auto writeFile = [](const char *path, const std::string &text) {
			int fd = ::open(path, O_WRONLY);
			if (fd < 0)
				return false;
			bool ok = ::write(fd, text.data(), text.size()) == ssize_t(text.size());
			::close(fd);
			return ok;
		};
		if (!writeFile("/proc/self/setgroups", "deny") ||
		    !writeFile("/proc/self/uid_map", "0 " + std::to_string(uid) + " 1") ||
		    !writeFile("/proc/self/gid_map", "0 " + std::to_string(gid) + " 1"))
			::_exit(77);
		if (::mount("tmpfs", "/dev/shm", "tmpfs", 0, "size=6m") != 0)
			::_exit(77);

		const uint32_t capacity = 8192;   // about 2.7 MB per segment: two fill most of the 6 MB
		std::string error;
		std::vector<std::unique_ptr<Segment>> leftovers;
		for (int k = 0; k < 2; ++k)
		{
			pid_t dead = deadPid();
			ProcessIdentity id = testIdentity();
			id.pid = dead;
			id.processStartTicks = 12345;
			// Fresh heartbeats while the second one is created, or that create() would already reap the first.
			auto seg = Segment::create(segmentNameForPid(dead), id, capacity, OverlayControl{}, &error);
			if (!seg)
				::_exit(20 + k);
			leftovers.push_back(std::move(seg));
		}
		for (auto &seg : leftovers)
			touchHeartbeat(*seg, 1);   // both crashed long ago
		leftovers.clear();   // unmapped (not unlinked): the pages stay allocated until someone removes the files
		if (listSegments().size() != 2)
			::_exit(29);
		auto fresh = Segment::create("pml-test.after-crashes", testIdentity(), capacity, OverlayControl{}, &error);
		if (!fresh)
			::_exit(30);   // "cannot reserve ... No space left on device": nobody reaped first
		if (listSegments().size() != 0)
			::_exit(31);   // both leftovers are gone (the new name is not a presentmon-linux.<pid> one)
		fresh.reset();
		::shm_unlink("/pml-test.after-crashes");

		// Crashed less than 5 minutes ago, silent for more than 10 s: create() takes them only when /dev/shm is full.
		for (int k = 0; k < 2; ++k)
		{
			pid_t dead = deadPid();
			ProcessIdentity id = testIdentity();
			id.pid = dead;
			id.processStartTicks = 12345;
			auto seg = Segment::create(segmentNameForPid(dead), id, capacity, OverlayControl{}, &error);
			if (!seg)
				::_exit(40 + k);
			leftovers.push_back(std::move(seg));
		}
		for (auto &seg : leftovers)
			touchHeartbeat(*seg, monotonicNs() - 60 * 1'000'000'000ull);
		leftovers.clear();
		auto again = Segment::create("pml-test.after-crash-loop", testIdentity(), capacity, OverlayControl{}, &error);
		if (!again)
			::_exit(50);   // full of young leftovers: the ENOSPC retry with the 10 s age did not happen
		::_exit(0);
	}
	int status = 0;
	ASSERT_EQ(::waitpid(child, &status, 0), child);
	ASSERT_TRUE(WIFEXITED(status)) << "status " << status;
	if (WEXITSTATUS(status) == 77)
		GTEST_SKIP() << "cannot create a user + mount namespace here";
	EXPECT_EQ(WEXITSTATUS(status), 0) << "2x/4x: leftover not created, 29: a leftover was reaped too early, "
	                                     "30: create failed, 31: leftovers remain, 50: no retry on a full /dev/shm";
}

// ---- L5: header strings are read with a bound ---------------------------------------------------

TEST(HeaderString, StopsAtTheFieldSize)
{
	char field[8];
	std::memset(field, 'A', sizeof(field));
	EXPECT_EQ(headerString(field), std::string(8, 'A'));
	field[3] = '\0';
	EXPECT_EQ(headerString(field), "AAA");
	field[0] = '\0';
	EXPECT_EQ(headerString(field), "");
}
