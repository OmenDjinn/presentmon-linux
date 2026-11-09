# SPDX-License-Identifier: MIT
"""Scenario 1: the CSV against vk_truth's ground-truth log, for each present mode the surface supports,
plus the --v1_metrics and --v2_metrics column sets on a real run."""
import unittest

import pmltest as pt
from pmltest import col, med, num, pct, show, trim

DEFAULT_COLUMNS = (
    "Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,PresentFlags,AllowsTearing,PresentMode,"
    "TimeInQPC,MsBetweenSimulationStart,MsBetweenPresents,MsBetweenDisplayChange,MsInPresentAPI,MsRenderPresentLatency,"
    "MsUntilDisplayed,CPUStartQPC,MsBetweenAppStart,MsCPUBusy,MsCPUWait,MsGPULatency,MsGPUTime,MsGPUBusy,MsGPUWait,"
    "MsAnimationError,AnimationTime,MsFlipDelay,MsAllInputToPhotonLatency,MsClickToPhotonLatency").split(",")
V1_COLUMNS = (
    "Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags,Dropped,TimeInSeconds,msInPresentAPI,"
    "msBetweenPresents,AllowsTearing,PresentMode,msUntilRenderComplete,msUntilDisplayed,msBetweenDisplayChange,"
    "msFlipDelay,msUntilRenderStart,msGPUActive,msSinceInput").split(",")
V2_COLUMNS = (
    "Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,PresentFlags,AllowsTearing,PresentMode,"
    "CPUStartTime,FrameTime,CPUBusy,CPUWait,GPULatency,GPUTime,GPUBusy,GPUWait,DisplayLatency,DisplayedTime,"
    "AnimationError,AnimationTime,MsFlipDelay,AllInputToPhotonLatency,ClickToPhotonLatency").split(",")

SECONDS = "3"


class CsvTruth(pt.LiveCase):
    _runs = {}

    @classmethod
    def scenario(cls, key, args, **kw):
        if key not in cls._runs:
            cls._runs[key] = pt.run_scenario(["--seconds", SECONDS] + args, **kw)
        return cls._runs[key]

    @classmethod
    def refresh_ms(cls):
        run = cls.scenario("fifo", ["--present-mode", "fifo"])
        return pt.fifo_period_ms(run)

    # -- shared checks -------------------------------------------------------------------
    def check_common(self, run, mode, sync, tearing, expect_interval_ms=None):
        self.need(run)
        self.assertRun(run)
        self.assertTrue(pt.has_bom(run.csv_path), "CSV starts with a UTF-8 BOM")
        self.assertEqual(run.header, DEFAULT_COLUMNS)
        d = pt.truth_compare(run)
        show(mode + " vs truth", **d)
        self.assertGreater(len(run.truth), 100, run.describe())
        # Row count: truth minus the first present (no predecessor) and the last (v2 needs a successor).
        self.assertTrue(d["contiguous"], "rows are consecutive presents of the truth log: %s" % d)
        self.assertEqual(d["matched"], d["rows"], "every row matches a truth frame by CPU start time")
        self.assertLessEqual(d["first_idx"], 2)
        self.assertIn(d["truth"] - d["rows"], (2, 3), "default/V2 output = truth minus 2 (one more tolerated): %s" % d)
        self.assertLess(d["dt_ms_med"], 0.05, "TimeInQPC equals present entry time")
        self.assertLess(d["bp_absdiff_med"], 0.01, "MsBetweenPresents per frame vs truth")
        self.assertLess(d["bp_absdiff_p95"], 0.1)
        self.assertLess(abs(d["inpresent_diff_med"]), 0.05, "MsInPresentAPI vs post - pre of the app")
        self.assertLess(d["inpresent_absdiff_p95"], 0.2)
        # Identity columns.
        for r in run.rows:
            if r["Application"] != "vk_truth" or r["PresentMode"] != mode:
                self.fail("bad identity columns in %r" % r)
        self.assertEqual({r["PresentRuntime"] for r in run.rows}, {"Vulkan"})
        self.assertEqual({r["ProcessID"] for r in run.rows}, {str(run.pid)})
        self.assertEqual({r["SyncInterval"] for r in run.rows}, {str(sync)})
        self.assertEqual({r["AllowsTearing"] for r in run.rows}, {str(tearing)})
        self.assertTrue(all(r["SwapChainAddress"].startswith("0x") for r in run.rows))
        # CPU columns: busy + wait is the end-to-end frame time, so it equals MsBetweenPresents on average.  Per
        # frame they differ by exactly the change in time-in-present (PresentMon: busy runs from the end of the
        # previous present to the start of this one, wait is this present's own time in present), so after
        # subtracting that only the CSV's 4-decimal rounding is left, however busy the machine is.
        diffs, residuals = [], []
        for prev, r in zip(run.rows, run.rows[1:]):
            bp, busy, wait = num(r["MsBetweenPresents"]), num(r["MsCPUBusy"]), num(r["MsCPUWait"])
            tip, prev_tip = num(r["MsInPresentAPI"]), num(prev["MsInPresentAPI"])
            if None not in (bp, busy, wait):
                diffs.append(bp - (busy + wait))
                if None not in (tip, prev_tip):
                    residuals.append(bp - (busy + wait) - (prev_tip - tip))
        self.assertLess(abs(sum(diffs) / len(diffs)), 0.01, "mean(MsBetweenPresents - (busy + wait))")
        self.assertGreater(len(residuals), 0.9 * len(diffs))
        self.assertLess(pct([abs(x) for x in residuals], 95), 0.002,
                        "MsBetweenPresents - (busy + wait) equals the change in MsInPresentAPI per frame")
        # GPU columns (frame by frame against vk_truth's GPU timestamps in e2e.load): present, busy within the
        # GPU time, and GPU data on nearly every row.
        gbusy, gtime = col(run.rows, "MsGPUBusy"), col(run.rows, "MsGPUTime")
        self.assertEqual(len(gbusy), len(run.rows))
        self.assertTrue(all(b <= t + 0.0002 for b, t in zip(gbusy, gtime)), "MsGPUBusy <= MsGPUTime")
        self.assertGreaterEqual(sum(1 for b in gbusy if b > 0), len(run.rows) - 5, "rows without GPU data")
        # Displayed frames
        until = col(run.rows, "MsUntilDisplayed")
        self.assertTrue(until, "some frames have display times")
        self.assertTrue(all(u > 0 for u in until), "MsUntilDisplayed > 0 on every displayed frame")
        self.assertLess(max(until), 250.0)
        if expect_interval_ms:
            self.assertAlmostEqual(med(trim(col(run.rows, "MsBetweenPresents"))), expect_interval_ms,
                                   delta=expect_interval_ms * 0.03)
        return d, until

    def check_display_multiples(self, run, period):
        bdc = trim(col(run.rows, "MsBetweenDisplayChange"), 0.1)
        self.assertGreater(len(bdc), 20)
        frac = pt.multiple_fraction(bdc, period, 0.3)
        block = pt.block_multiple_fraction(bdc, period)
        show("display change multiples", period_ms=period, n=len(bdc), per_value_fraction=frac, block8_fraction=block,
             median=med(bdc), p5=pct(bdc, 5), p95=pct(bdc, 95))
        # Per value on a quiet compositor; the sums of 8 consecutive values must always be refresh multiples.
        self.assertGreaterEqual(block, 0.9, "MsBetweenDisplayChange adds up to multiples of the %.3f ms refresh" % period)
        return frac

    # -- present modes -------------------------------------------------------------------
    def test_fifo(self):
        run = self.scenario("fifo", ["--present-mode", "fifo"])
        period = self.refresh_ms()
        d, until = self.check_common(run, "FIFO", 1, 0)
        show("fifo", refresh_ms=period, displayed=len(until), rows=len(run.rows))
        self.assertGreaterEqual(len(until), 0.99 * len(run.rows), "FIFO displays every frame")
        self.assertAlmostEqual(med(trim(col(run.rows, "MsBetweenPresents"))), period, delta=period * 0.02)
        self.check_display_multiples(run, period)

    def test_mailbox_capped(self):
        run = self.scenario("mailbox", ["--present-mode", "mailbox", "--fps", "1000"])
        period = self.refresh_ms()
        d, until = self.check_common(run, "MAILBOX", 0, 0, expect_interval_ms=1.0)
        frac = len(until) / len(run.rows)
        show("mailbox", refresh_ms=period, displayed=len(until), rows=len(run.rows), displayed_fraction=frac)
        # 1000 fps into a <= 240 Hz output: most frames are replaced and dropped (NA display columns).
        self.assertLess(frac, 0.6)
        self.assertGreater(len(until), 20)
        self.check_display_multiples(run, period)

    def test_immediate_capped(self):
        run = self.scenario("immediate", ["--present-mode", "immediate", "--fps", "240"])
        d, until = self.check_common(run, "IMMEDIATE", 0, 1, expect_interval_ms=1000 / 240.0)
        bdc = col(run.rows, "MsBetweenDisplayChange")
        show("immediate", displayed=len(until), rows=len(run.rows), bdc_median=med(bdc))
        self.assertTrue(bdc)
        self.assertTrue(all(v > 0 for v in bdc))

    def test_fifo_latest_ready(self):
        run = self.scenario("flr", ["--present-mode", "fifo_latest_ready", "--fps", "240"])
        period = self.refresh_ms()
        # SyncInterval is 1 for any FIFO-family mode (docs/ARCHITECTURE.md 3.2), tearing is not allowed.
        d, until = self.check_common(run, "FIFO_LATEST_READY", 1, 0, expect_interval_ms=1000 / 240.0)
        show("fifo_latest_ready", refresh_ms=period, displayed=len(until), rows=len(run.rows))
        self.check_display_multiples(run, period)

    # -- column sets ---------------------------------------------------------------------
    def test_v1_metrics(self):
        run = self.scenario("v1", ["--present-mode", "fifo"], pm_args=["--v1_metrics"], qpc=False)
        self.need(run)
        self.assertRun(run)
        self.assertEqual(run.header, V1_COLUMNS)
        n = len(run.truth)
        show("v1", rows=len(run.rows), truth=n)
        self.assertIn(n - len(run.rows), (0, 1, 2), "v1 writes one row per present (the first has no predecessor)")
        self.assertEqual({r["Runtime"] for r in run.rows}, {"Vulkan"})
        self.assertEqual({r["PresentMode"] for r in run.rows}, {"FIFO"})
        self.assertEqual({r["Dropped"] for r in run.rows}, {"0"}, "FIFO drops nothing")
        bp = trim(col(run.rows, "msBetweenPresents"))
        truth_med = med(trim(run.truth_intervals_ms()))
        self.assertAlmostEqual(med(bp), truth_med, delta=0.2)
        self.assertTrue(all(v > 0 for v in col(run.rows, "msUntilDisplayed")))
        # TimeInSeconds grows monotonically
        ts = col(run.rows, "TimeInSeconds")
        self.assertEqual(ts, sorted(ts))

    def test_v2_metrics(self):
        run = self.scenario("v2", ["--present-mode", "fifo"], pm_args=["--v2_metrics"], qpc=False)
        self.need(run)
        self.assertRun(run)
        self.assertEqual(run.header, V2_COLUMNS)
        n = len(run.truth)
        show("v2", rows=len(run.rows), truth=n)
        self.assertIn(n - len(run.rows), (2, 3), "v2 = truth minus 2")
        ft, busy, wait = col(run.rows, "FrameTime"), col(run.rows, "CPUBusy"), col(run.rows, "CPUWait")
        self.assertEqual(len(ft), len(run.rows))
        mean = lambda xs: sum(xs) / len(xs)
        self.assertAlmostEqual(mean(ft), mean(busy) + mean(wait), delta=0.01)
        self.assertAlmostEqual(mean(trim(ft)), mean(trim(run.truth_intervals_ms())), delta=0.2)
        self.assertTrue(all(v > 0 for v in col(run.rows, "DisplayLatency")))
        self.assertTrue(all(v > 0 for v in col(run.rows, "DisplayedTime")))
        gbusy = col(run.rows, "GPUBusy")
        self.assertGreaterEqual(sum(1 for b in gbusy if b > 0), len(run.rows) - 5, "v2 rows without GPU data")

    @pt.not_on_shared_desktop("~19000 presents per second flood the compositor")
    def test_uncapped_mailbox_keeps_producing_rows(self):
        """E1 (fixed): mailbox at ~19000 fps (uncapped, tiny frame), where the layer soon stops getting display
        times.  The V2 engine used to hold every row back waiting for the next displayed frame, so the CSV stopped
        after ~0.6 s.  Rows must keep flowing for the whole run (losses are fine, silence is not)."""
        run = pt.run_scenario(["--seconds", SECONDS, "--present-mode", "mailbox"])
        self.need(run)
        self.assertRun(run)
        times = col(run.rows, "TimeInQPC")
        self.assertTrue(times)
        truth_span = (run.truth[-1]["present_pre_ns"] - run.truth[0]["present_pre_ns"]) / 1e6
        row_span = (max(times) - min(times)) / 1e6
        show("uncapped mailbox", truth_frames=len(run.truth), rows=len(run.rows), row_span_ms=row_span,
             truth_span_ms=truth_span)
        self.assertGreaterEqual(row_span, 0.8 * truth_span)


if __name__ == "__main__":
    unittest.main(verbosity=2)
