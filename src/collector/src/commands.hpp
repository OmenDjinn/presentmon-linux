// SPDX-License-Identifier: MIT
// Entry points of the subcommands. Each takes the arguments after the subcommand word and returns
// the process exit code.
#pragma once

#include <pml/shm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pml::cli {

int runList(const std::vector<std::string> &args);
int runOverlay(const std::vector<std::string> &args);

// Segment filters shared by `list` and `overlay`.
struct SegmentFilter
{
	std::vector<std::string> processNames;   // canonical
	uint32_t pid = 0;                        // 0 = any
	// Parses one filter option at args[i] (advancing i past its value). Returns 1 if consumed,
	// 0 if args[i] is not a filter option, -1 on error (message in *error).
	int parse(const std::vector<std::string> &args, size_t &i, std::string *error);
	bool matches(int pid, const std::string &processName) const;
};

// True while the producer runs: pid + start time match (pml::processAlive) and it is not a zombie.
// A killed process whose parent has not reaped it still has a /proc entry, but it will never present again.
bool producerAlive(int pid, uint64_t startTicks);

// A segment found in /dev/shm, opened for `list` / `overlay`.
struct FoundSegment
{
	std::string name;
	std::unique_ptr<pml::Segment> segment;
	bool alive = false;   // producer process exists (pid + start time) and the segment is not marked exited
	bool stale = false;   // pml::segmentIsStale
};

// Opens every segment that matches the filter. `*unreadable` counts segments that could not be opened
// (still initializing) and `*hiddenStale` segments of dead processes (and abandoned half-created files) that were skipped.
std::vector<FoundSegment> findSegments(const SegmentFilter &filter, size_t *unreadable, size_t *hiddenStale);

} // namespace pml::cli
