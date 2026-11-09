// SPDX-License-Identifier: MIT
// The process-wide segment after fork().
#include "process_segment.hpp"

#include "pml/process_info.hpp"

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

using namespace pml::layer;

TEST(ProcessSegment, ForkedChildDropsTheInheritedSegment)
{
	Process &p = process();
	pml::ProcessIdentity identity = pml::currentProcessIdentity();
	std::string error;
	auto segment = pml::Segment::create(pml::segmentNameForPid(::getpid()), identity,
	                                    pml::kDefaultRingCapacity, pml::OverlayControl{}, &error);
	ASSERT_TRUE(segment) << error;
	const std::string name = segment->name();
	{
		std::lock_guard<std::mutex> lock(p.mutex);
		p.ring = std::make_unique<pml::RingWriter>(*segment);
		p.segment = std::move(segment);
		p.attempted = true;
		p.ownerPid = ::getpid();
	}
	installForkHandler(nullptr);

	// Another thread of the parent holding the lock at fork time must not lock the child up.
	p.mutex.lock();
	const pid_t child = ::fork();
	if (child == 0)
	{
		const bool reset = !p.segment && !p.ring && !p.attempted && p.ownerPid == 0;
		const bool lockFree = p.mutex.try_lock();  // a fresh mutex: lockable although the parent held it
		::_exit(reset && lockFree ? 0 : 1);
	}
	p.mutex.unlock();
	ASSERT_GT(child, 0);
	int status = 0;
	ASSERT_EQ(::waitpid(child, &status, 0), child);
	EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "child state was not reset";

	// The parent keeps its segment and unlinks it (the child did not).
	EXPECT_TRUE(p.segment);
	p.segment->unlink();
	p.ring.reset();
	p.segment.reset();
	p.attempted = false;
	p.ownerPid = 0;
	(void)name;
}
