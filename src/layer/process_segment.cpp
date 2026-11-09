// SPDX-License-Identifier: MIT
#include "process_segment.hpp"

#include <mutex>
#include <new>
#include <pthread.h>

namespace pml::layer {

namespace {
void (*gOnChild)() = nullptr;
std::once_flag gForkOnce;

void childAfterFork()
{
	resetProcessAfterFork();
	if (gOnChild)
		gOnChild();
}
} // namespace

Process &process()
{
	static Process *p = new Process;
	return *p;
}

void resetProcessAfterFork()
{
	Process &p = process();
	// The mutex may have been locked by a thread that does not exist in the child.
	new (&p.mutex) std::mutex;
	// The inherited segment belongs to the parent. It is released, not destroyed: the child keeps the
	// mapping (inert: Segment::inherited() makes every producer call a no-op in a child), so pointers to it
	// that other layer state still holds (the overlay's per-swapchain copy) can never dangle.
	(void)p.ring.release();
	(void)p.segment.release();
	p.attempted = false;
	p.ownerPid = 0;
}

void installForkHandler(void (*onChild)())
{
	std::call_once(gForkOnce, [onChild] {
		gOnChild = onChild;
		pthread_atfork(nullptr, nullptr, childAfterFork);
	});
}

} // namespace pml::layer
