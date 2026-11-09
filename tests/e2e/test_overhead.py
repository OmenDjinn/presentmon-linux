# SPDX-License-Identifier: MIT
"""Scenario 9: cost of the layer and of the overlay on the app, measured on the app's own timestamps."""
import unittest

import pmltest as pt
from pmltest import med, show, trim

REPS = 2
LAYER_BUDGET_MS = 0.05     # layer without overlay, per present
OVERLAY_BUDGET_MS = 0.5    # overlay visible, per present

GPU_TIMING_BUDGET_MS = 0.05  # GPU timestamps around the submissions, per frame of a GPU-bound app

# name, layer, overlay, extra environment
CONFIGS = (("none", False, "off", None), ("layer", True, "off", None), ("overlay", True, "1", None),
           ("layer-nogpu", True, "off", {"PRESENTMON_GPU": "0"}))
WORKLOADS = {
    "mailbox": ["--present-mode", "mailbox", "--cpu-ms", "1", "--size", "1280x720"],
    "fifo": ["--present-mode", "fifo", "--cpu-ms", "1", "--size", "1280x720"],
    # GPU bound: the frame interval is the GPU time, so a stall from the layer's timestamps would show in it.
    "gpu": ["--present-mode", "mailbox", "--gpu-ms", "4", "--size", "1280x720"],
}


@pt.not_on_shared_desktop("overhead benchmarks need an otherwise idle machine")
class Overhead(pt.LiveCase):
    results = {}

    @classmethod
    def measure(cls, workload):
        if workload in cls.results:
            return cls.results[workload]
        samples = {name: {"iv_med": [], "iv_mean": [], "pr_med": []} for name, _, _, _ in CONFIGS}
        for rep in range(REPS):
            for name, layer, overlay, env in CONFIGS:
                run = pt.run_scenario(["--seconds", "3"] + WORKLOADS[workload], layer=layer, overlay=overlay, capture=False,
                                      env_extra=env)
                if run.exit_code != 0:
                    raise AssertionError("overhead run failed\n" + run.describe())
                iv = trim(run.truth_intervals_ms(), 0.15)
                pr = trim(run.truth_in_present_ms(), 0.15)
                samples[name]["iv_med"].append(med(iv))
                samples[name]["iv_mean"].append(sum(iv) / len(iv))
                samples[name]["pr_med"].append(med(pr))
        agg = {n: {k: med(v) for k, v in d.items()} for n, d in samples.items()}
        cls.results[workload] = agg
        base = agg["none"]
        for name in ("layer", "overlay", "layer-nogpu"):
            show("overhead %s %s vs none" % (workload, name),
                 interval_median_ms=agg[name]["iv_med"], interval_delta_median_ms=agg[name]["iv_med"] - base["iv_med"],
                 interval_delta_mean_ms=agg[name]["iv_mean"] - base["iv_mean"],
                 present_call_ms=agg[name]["pr_med"], present_call_delta_ms=agg[name]["pr_med"] - base["pr_med"])
        show("overhead %s baseline" % workload, interval_median_ms=base["iv_med"], interval_mean_ms=base["iv_mean"],
             present_call_ms=base["pr_med"])
        return agg

    def check(self, workload, interval_key):
        agg = self.measure(workload)
        b, l, o = agg["none"], agg["layer"], agg["overlay"]
        self.assertLess(l["pr_med"] - b["pr_med"], LAYER_BUDGET_MS, "layer cost inside vkQueuePresentKHR")
        self.assertLess(l[interval_key] - b[interval_key], LAYER_BUDGET_MS, "layer cost on the frame interval")
        self.assertLess(o["pr_med"] - b["pr_med"], OVERLAY_BUDGET_MS, "overlay cost inside vkQueuePresentKHR")
        self.assertLess(o[interval_key] - b[interval_key], OVERLAY_BUDGET_MS, "overlay cost on the frame interval")

    def test_mailbox_cpu_bound(self):
        # CPU-bound with a 1 ms spin: the frame interval is the app's work plus present-call cost, so deltas are real.
        self.check("mailbox", "iv_med")

    def test_fifo(self):
        # FIFO is paced by the display; the mean is steadier than the median there, the present call is the signal.
        self.check("fifo", "iv_mean")

    def test_gpu_timing_on_a_gpu_bound_app(self):
        """The GPU timestamps around each submission must not stall the GPU: on a GPU-bound app the frame interval
        (= GPU time) with GPU timing is within the budget of the same layer without it (PRESENTMON_GPU=0)."""
        agg = self.measure("gpu")
        with_gpu, without = agg["layer"], agg["layer-nogpu"]
        self.assertLess(with_gpu["iv_med"] - without["iv_med"], GPU_TIMING_BUDGET_MS, "GPU timing cost per frame")
        self.assertLess(with_gpu["pr_med"] - without["pr_med"], LAYER_BUDGET_MS, "GPU timing cost inside the present")


if __name__ == "__main__":
    unittest.main(verbosity=2)
