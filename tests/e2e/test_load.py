# SPDX-License-Identifier: MIT
"""Scenario 3: CPU-bound (--cpu-ms) and GPU-bound (--gpu-ms) runs, and the GPU columns (the layer's timestamps around
queue submissions, src/layer/gpu_timer.cpp) against vk_truth's own GPU timestamps of each frame (gpu_begin_ns /
gpu_end_ns: its first and last timestamp around the frame's GPU work)."""
import unittest

import pmltest as pt
from pmltest import col, med, num, pct, show, trim

GPU_COLUMNS = ("MsGPULatency", "MsGPUTime", "MsGPUBusy", "MsGPUWait")


class LoadBound(pt.LiveCase):
    def check_gpu_columns(self, run, label, busy_tol_ms=0.1):
        """Every row has GPU data; MsGPUBusy is the frame's GPU work as vk_truth timed it, frame by frame;
        MsGPUTime = MsGPUBusy + MsGPUWait; MsGPULatency is CPU start -> GPU start (positive, below a frame)."""
        for name in GPU_COLUMNS:
            self.assertEqual(len(col(run.rows, name)), len(run.rows), name + " is present on every row")
        pairs = run.match()
        self.assertGreater(len(pairs), len(run.rows) * 0.9, "CSV rows matched to vk_truth frames\n" + run.describe())
        diffs, missing = [], 0
        for row, i, _ in pairs:
            busy = num(row["MsGPUBusy"])
            if not busy:
                missing += 1
                continue
            t = run.truth[i]
            diffs.append(busy - (t["gpu_end_ns"] - t["gpu_begin_ns"]) / 1e6)
        absd = [abs(d) for d in diffs]
        busy_col, wait_col, lat_col, time_col = (col(run.rows, c) for c in ("MsGPUBusy", "MsGPUWait", "MsGPULatency", "MsGPUTime"))
        show(label, gpu_busy_med=med(busy_col), gpu_wait_med=med(wait_col), gpu_latency_med=med(lat_col),
             busy_vs_truth_med=med(absd), busy_vs_truth_p95=pct(absd, 95), without_gpu=missing, rows=len(pairs))
        self.assertLessEqual(missing, 3, "rows without GPU data")
        self.assertLess(pct(absd, 95), busy_tol_ms, "MsGPUBusy vs vk_truth's GPU time of the same frame")
        for b, w, t in zip(busy_col, wait_col, time_col):
            self.assertAlmostEqual(t, b + w, delta=0.0002)
            self.assertGreaterEqual(w, 0.0)
        self.assertGreater(min(lat_col), 0.0)

    def cpu_run(self, cpu_ms):
        run = pt.run_scenario(["--seconds", "3", "--present-mode", "mailbox", "--cpu-ms", str(cpu_ms)])
        self.need(run)
        self.assertRun(run)
        busy = trim(col(run.rows, "MsCPUBusy"))
        wait = trim(col(run.rows, "MsCPUWait"))
        bp = trim(col(run.rows, "MsBetweenPresents"))
        show("cpu-ms %s" % cpu_ms, busy_med=med(busy), wait_med=med(wait), bp_med=med(bp), rows=len(run.rows))
        self.assertGreater(len(run.rows), 100)
        return run, med(busy), med(wait), med(bp)

    def test_cpu_bound_tracks_cpu_ms(self):
        results = {}
        for cpu_ms in (2.0, 6.0):
            run, busy, wait, bp = self.cpu_run(cpu_ms)
            results[cpu_ms] = busy
            self.assertAlmostEqual(busy, cpu_ms, delta=cpu_ms * 0.05 + 0.1, msg="MsCPUBusy tracks --cpu-ms")
            self.assertAlmostEqual(bp, cpu_ms, delta=cpu_ms * 0.05 + 0.1, msg="unpaced frame interval = cpu time")
            self.assertLess(wait, 0.2, "time in present is tiny when the app is CPU bound")
            # Almost no GPU work: a GPU busy time far below the frame time.
            self.assertLess(med(trim(col(run.rows, "MsGPUBusy"))), 1.0)
        self.assertAlmostEqual(results[6.0] - results[2.0], 4.0, delta=0.3)

    def test_gpu_columns_match_the_apps_gpu_timestamps(self):
        """A light load (3 ms of GPU work per frame at 60 fps), safe next to a game on the same GPU."""
        run = pt.run_scenario(["--seconds", "4", "--present-mode", "fifo", "--fps", "60", "--gpu-ms", "3",
                               "--size", "640x360"])
        self.need(run)
        self.assertRun(run)
        self.check_gpu_columns(run, "gpu-ms 3 @60")
        self.assertAlmostEqual(med(trim(col(run.rows, "MsGPUBusy"))), 3.0, delta=0.6, msg="MsGPUBusy tracks --gpu-ms")

    @pt.not_on_shared_desktop("a GPU-bound load competes with the game on the same GPU")
    def test_gpu_bound_interval_tracks_gpu_ms(self):
        gpu_ms = 8.0
        run = pt.run_scenario(["--seconds", "3", "--present-mode", "mailbox", "--gpu-ms", str(gpu_ms)])
        self.need(run)
        self.assertRun(run)
        bp = trim(col(run.rows, "MsBetweenPresents"))
        truth_iv = trim(run.truth_intervals_ms())
        show("gpu-ms %.1f" % gpu_ms, bp_med=med(bp), truth_med=med(truth_iv), rows=len(run.rows),
             busy_med=med(trim(col(run.rows, "MsCPUBusy"))), wait_med=med(trim(col(run.rows, "MsCPUWait"))))
        self.assertGreater(len(run.rows), 100)
        self.assertGreaterEqual(med(bp), gpu_ms * 0.95, "frame interval cannot be shorter than the GPU work")
        self.assertLessEqual(med(bp), gpu_ms * 1.25, "frame interval tracks --gpu-ms")
        self.assertAlmostEqual(med(bp), med(truth_iv), delta=0.1, msg="CSV agrees with the truth log")
        self.check_gpu_columns(run, "gpu-ms 8 mailbox")
        # GPU bound: the GPU is busy for nearly the whole frame.
        self.assertGreater(med(trim(col(run.rows, "MsGPUBusy"))), 0.9 * med(bp))

    @pt.not_on_shared_desktop("a GPU-bound load competes with the game on the same GPU")
    def test_gpu_bound_fifo_cpu_columns_consistent(self):
        """GPU-bound under FIFO: the frame interval is still the GPU time or more, display changes stay on the
        refresh grid, and MsCPUBusy + MsCPUWait add up to the frame interval.  (The CPU blocks in a fence wait
        in the app, which PresentMon counts as CPU busy because it is outside the present call.)"""
        run = pt.run_scenario(["--seconds", "3", "--present-mode", "fifo", "--gpu-ms", "10"])
        self.need(run)
        self.assertRun(run)
        bp, busy, wait = (trim(col(run.rows, c)) for c in ("MsBetweenPresents", "MsCPUBusy", "MsCPUWait"))
        show("gpu-ms 10 fifo", bp_med=med(bp), busy_med=med(busy), wait_med=med(wait),
             bdc_med=med(trim(col(run.rows, "MsBetweenDisplayChange"))))
        self.assertGreaterEqual(med(bp), 9.5)
        self.assertAlmostEqual(sum(bp) / len(bp), sum(busy) / len(busy) + sum(wait) / len(wait), delta=0.05)
        self.check_gpu_columns(run, "gpu-ms 10 fifo")


if __name__ == "__main__":
    unittest.main(verbosity=2)
