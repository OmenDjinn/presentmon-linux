// SPDX-License-Identifier: MIT
// The process-wide shared-memory segment and ring the layer publishes into. One per process; a
// fork()ed child must not keep using its parent's (it would write the same single-producer ring
// under a different pid and with copied, unrelated mutexes), so a pthread_atfork handler drops the
// inherited segment in the child and lets it create its own lazily.
#pragma once

#include "pml/shm.hpp"

#include <memory>
#include <mutex>

namespace pml::layer {

struct Process
{
	std::mutex mutex;
	std::unique_ptr<pml::Segment> segment;
	std::unique_ptr<pml::RingWriter> ring;
	bool attempted = false;
	int ownerPid = 0;
};

// Leaked on purpose: nothing here may run destructors while the process is shutting down.
Process &process();

// Registers (once) the atfork child handler. `onChild` runs after the process state was reset and
// may reinitialise other layer globals; it can be null.
void installForkHandler(void (*onChild)());

// What the child handler does to the process state (public for tests).
void resetProcessAfterFork();

} // namespace pml::layer
