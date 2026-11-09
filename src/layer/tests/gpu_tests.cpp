// SPDX-License-Identifier: MIT
// GpuFrameAttributor and GpuClock (gpu_intervals.hpp): which GPU work belongs to which frame.
#include "gpu_intervals.hpp"

#include <gtest/gtest.h>

using pml::layer::GpuClock;
using pml::layer::GpuFrameAttributor;
using pml::layer::GpuFrameWork;

namespace {

constexpr uint64_t ms(double v) { return uint64_t(v * 1'000'000.0); }

} // namespace

TEST(GpuAttributor, UnionOfOverlappingWorkIdleGapsExcluded)
{
	GpuFrameAttributor a;
	// Graphics 10-14 ms, async compute 12-15 ms (overlaps), a copy 17-18 ms after an idle gap.
	a.add({ms(10), ms(14), ms(9)});
	a.add({ms(12), ms(15), ms(9)});
	a.add({ms(17), ms(18), ms(9)});
	GpuFrameWork w = a.take(ms(20), ms(9.5));
	ASSERT_TRUE(w.valid);
	EXPECT_EQ(w.startNs, ms(10));
	EXPECT_EQ(w.busyNs, ms(5) + ms(1));
	EXPECT_EQ(w.intervals, 3u);
	EXPECT_EQ(a.pending(), 0u);
}

TEST(GpuAttributor, WorkEndingAfterReadyIsTheNextFrames)
{
	// vkd3d-proton / DXVK: frame 2's work is submitted (and starts) before frame 1's present call, but ends
	// after frame 1's present was ready.
	GpuFrameAttributor a;
	a.add({ms(1), ms(5), ms(0.5)});     // frame 1
	a.add({ms(5), ms(11), ms(4)});      // frame 2, submitted before frame 1's present (at 6 ms)
	GpuFrameWork f1 = a.take(ms(5.2), ms(6));
	ASSERT_TRUE(f1.valid);
	EXPECT_EQ(f1.startNs, ms(1));
	EXPECT_EQ(f1.busyNs, ms(4));
	GpuFrameWork f2 = a.take(ms(11.1), ms(12));
	ASSERT_TRUE(f2.valid);
	EXPECT_EQ(f2.startNs, ms(5));
	EXPECT_EQ(f2.busyNs, ms(6));
}

TEST(GpuAttributor, CalibrationSkewAfterReadyStaysWithTheFrame)
{
	// Measured with vk_truth: the frame's last timestamp converted ~10-20 us after its ready time. Work
	// submitted before the present call and ending within the slack is the frame's.
	GpuFrameAttributor a;
	a.add({ms(1), ms(4) + 15'000, ms(0.5)});
	GpuFrameWork w = a.take(ms(4), ms(2));
	ASSERT_TRUE(w.valid);
	EXPECT_EQ(w.busyNs, ms(3) + 15'000);
	// The same, but submitted after the present call: the next frame's.
	a.add({ms(20), ms(24) + 15'000, ms(23)});
	EXPECT_FALSE(a.take(ms(24), ms(22)).valid);
	EXPECT_EQ(a.pending(), 1u);
	// Beyond the slack: never this frame's.
	GpuFrameAttributor b;
	b.add({ms(1), ms(4) + GpuFrameAttributor::kReadySlackNs + 1, ms(0.5)});
	EXPECT_FALSE(b.take(ms(4), ms(2)).valid);
}

TEST(GpuAttributor, LateOrAncientWorkIsNoFramesWork)
{
	GpuFrameAttributor a;
	// Loading work 3 s before the first frame: no frame's.
	a.add({ms(1000), ms(1500), ms(900)});
	a.add({ms(4000), ms(4003), ms(3999)});
	GpuFrameWork f1 = a.take(ms(4004), ms(4003.5));
	ASSERT_TRUE(f1.valid);
	EXPECT_EQ(f1.startNs, ms(4000));
	EXPECT_EQ(f1.busyNs, ms(3));
	EXPECT_EQ(a.dropped(), 1u);
	// A batch of frame 1 read back only after frame 1 was taken: dropped, not charged to frame 2.
	a.add({ms(4002), ms(4003.9), ms(3999)});
	a.add({ms(4010), ms(4012), ms(4009)});
	GpuFrameWork f2 = a.take(ms(4013), ms(4012.5));
	ASSERT_TRUE(f2.valid);
	EXPECT_EQ(f2.startNs, ms(4010));
	EXPECT_EQ(f2.busyNs, ms(2));
	EXPECT_EQ(a.dropped(), 2u);
}

TEST(GpuAttributor, FrameWithoutWork)
{
	GpuFrameAttributor a;
	GpuFrameWork w = a.take(ms(5), ms(4));
	EXPECT_FALSE(w.valid);
	EXPECT_EQ(w.startNs, 0u);
}

TEST(GpuAttributor, FrameWithoutReadyTimeDropsItsWork)
{
	// No ready time (lost report): the frame gets nothing, and its work must not inflate the next frame.
	GpuFrameAttributor a;
	a.add({ms(1), ms(3), ms(0.5)});
	a.add({ms(7), ms(9), ms(6)});
	a.discardBefore(ms(4));   // the untimed frame's present ended at 4 ms
	GpuFrameWork w = a.take(ms(10), ms(8));
	ASSERT_TRUE(w.valid);
	EXPECT_EQ(w.startNs, ms(7));
	EXPECT_EQ(w.busyNs, ms(2));
}

TEST(GpuAttributor, BadIntervalsAreClampedAndPendingIsCapped)
{
	GpuFrameAttributor a;
	a.add({ms(5), ms(4), ms(1)});   // start after end: zero length at the end
	a.add({0, 0, 0});               // no end: ignored
	GpuFrameWork w = a.take(ms(6), ms(5));
	ASSERT_TRUE(w.valid);
	EXPECT_EQ(w.startNs, ms(4));
	EXPECT_EQ(w.busyNs, 0u);

	const size_t n = GpuFrameAttributor::kMaxPending + 10;
	for (size_t i = 0; i < n; i++)
		a.add({ms(100) + i, ms(100) + i + 1, ms(99)});
	EXPECT_LE(a.pending(), GpuFrameAttributor::kMaxPending);
	EXPECT_EQ(a.dropped() + a.pending(), n);
	// The oldest went: what is left starts where the dropped ones end.
	GpuFrameWork all = a.take(ms(200), ms(150));
	EXPECT_EQ(all.startNs, ms(100) + a.dropped());
}

TEST(GpuClock, TicksToMonotonic)
{
	GpuClock c;
	EXPECT_EQ(c.toMonotonic(1000), 0u) << "not calibrated";
	c.calibrated = true;
	c.calTicks = 1'000'000;
	c.calMonoNs = 5'000'000'000;
	c.periodNs = 1.0;
	EXPECT_EQ(c.toMonotonic(1'000'500), 5'000'000'500u);
	EXPECT_EQ(c.toMonotonic(999'000), 4'999'999'000u) << "before the calibration point";
	c.periodNs = 2.5;
	EXPECT_EQ(c.toMonotonic(1'000'004), 5'000'000'010u);
	EXPECT_EQ(c.toMonotonic(0), 0u) << "0 = missing";
}

TEST(GpuClock, NarrowCountersUnwrapAroundTheCalibration)
{
	GpuClock c;
	c.calibrated = true;
	c.validBits = 36;
	const uint64_t mask = (uint64_t(1) << 36) - 1;
	c.calTicks = mask - 100;   // just before the wrap
	c.calMonoNs = 10'000'000'000;
	EXPECT_EQ(c.toMonotonic(50), 10'000'000'151u) << "after the wrap";
	EXPECT_EQ(c.toMonotonic(mask - 300), 9'999'999'800u) << "before the calibration";
}
