# SPDX-License-Identifier: MIT
"""Scenario 4: DXVK-like sharing of VK_EXT_present_timing.  vk_truth --app-timing --app-present-id uses present_id2,
present_wait2 and present timing exactly like DXVK 3.1 and validates every report it gets; the layer must not
disturb any of it."""
import unittest

import pmltest as pt
from pmltest import col, med, show, trim

APP = ["--seconds", "3", "--present-mode", "fifo", "--app-timing", "--app-present-id"]


class DxvkShare(pt.LiveCase):
    _runs = {}

    @classmethod
    def scenario(cls, key, extra, layer=True, **kw):
        if key not in cls._runs:
            cls._runs[key] = pt.run_scenario(APP + extra, layer=layer, capture=layer, **kw)
        return cls._runs[key]

    def check_app_ok(self, run):
        self.need(run)
        self.assertRun(run)
        s = pt.truth_summary(run.stdout)
        self.assertEqual(s.get("anomalies"), 0, "present-timing anomalies\n" + run.stdout[-1500:] + run.stderr[-800:])
        self.assertGreater(s.get("requested", 0), 100)
        self.assertEqual(s.get("reported"), s.get("requested"), "every timing request got a report: %s" % s)
        return s

    def check_csv_has_display(self, run):
        until = col(run.rows, "MsUntilDisplayed")
        bdc = col(run.rows, "MsBetweenDisplayChange")
        self.assertGreater(len(run.rows), 100)
        self.assertGreaterEqual(len(until), 0.95 * len(run.rows), "display columns are filled")
        self.assertTrue(all(u > 0 for u in until))
        self.assertGreaterEqual(len(bdc), 0.95 * len(run.rows))

    def test_with_and_without_layer(self):
        base = self.scenario("base", [], layer=False)
        lay = self.scenario("layer", [])
        sb = self.check_app_ok(base)
        sl = self.check_app_ok(lay)
        mb = pt.window_median(base.truth_intervals_ms())
        ml = pt.window_median(lay.truth_intervals_ms())
        ab = sum(base.truth_intervals_ms()) / len(base.truth_intervals_ms())
        al = sum(lay.truth_intervals_ms()) / len(lay.truth_intervals_ms())
        show("dxvk-like without/with layer", reports_base=sb["reports_seen"], reports_layer=sl["reports_seen"],
             median16_base=mb, median16_layer=ml, mean_base=ab, mean_layer=al, rows=len(lay.rows))
        self.assertLessEqual(abs(sb["reports_seen"] - sl["reports_seen"]), max(3, 0.01 * sb["reports_seen"]),
                             "same number of reports")
        self.assertAlmostEqual(ml, mb, delta=mb * 0.01, msg="median frame interval (16-frame windows) within 1%")
        self.assertAlmostEqual(al, ab, delta=ab * 0.01, msg="mean frame interval within 1%")
        self.check_csv_has_display(lay)
        self.assertIn(len(lay.truth) - len(lay.rows), (2, 3))

    def test_timing_stage_queue(self):
        run = self.scenario("stage-queue", ["--timing-stage", "queue"])
        self.check_app_ok(run)
        self.check_csv_has_display(run)  # the layer asks for the pixel-out stage itself

    def test_timing_stage_pixel_out(self):
        run = self.scenario("stage-pixel", ["--timing-stage", "pixel-out"])
        self.check_app_ok(run)
        self.check_csv_has_display(run)

    def test_time_domains(self):
        seen = []
        for dom in ("monotonic", "stage", "swapchain", "auto"):
            run = self.scenario("dom-" + dom, ["--time-domain", dom])
            if run.skip:
                print("[measure] time domain %s: not offered by this driver (%s)" % (dom, run.skip), flush=True)
                continue
            with self.subTest(domain=dom):
                self.check_app_ok(run)
                self.check_csv_has_display(run)
                seen.append(dom)
        if not seen:
            self.skipTest("no time domain could be exercised")
        show("time domains exercised", domains=",".join(seen))

    def test_small_timing_queue(self):
        """The app's queue size (4) is emulated on top of the layer's own headroom: no queue-full anomalies, no
        lost reports, same pacing as the default queue."""
        run = self.scenario("tq4", ["--timing-queue", "4"])
        s = self.check_app_ok(run)
        ref = self.scenario("layer", [])
        show("timing queue 4", queue_full=s.get("queue_full"), median16=pt.window_median(run.truth_intervals_ms()),
             ref_median16=pt.window_median(ref.truth_intervals_ms()))
        self.assertEqual(s.get("queue_full"), 0, "4 slots are enough for the app at this frame rate")
        self.check_csv_has_display(run)
        self.assertAlmostEqual(sum(run.truth_intervals_ms()) / len(run.truth_intervals_ms()),
                               sum(ref.truth_intervals_ms()) / len(ref.truth_intervals_ms()), delta=0.1)

    def test_tiny_timing_queue(self):
        """Queue size 1: the app runs out of slots.  vk_truth handles VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT like
        DXVK 3.1 (no retry, because a rejected present has already executed its semaphore waits; the queue size
        doubles and the swapchain is recreated).  The layer emulates the app's limit, so with the layer the app
        must see the error exactly when its own requests fill the queue (never with free slots), survive it, and
        get every accepted request answered.  The run without the layer shows the driver's own behaviour."""
        base = self.scenario("tq1-base", ["--timing-queue", "1"], layer=False)
        run = self.scenario("tq1", ["--timing-queue", "1"])
        sb = pt.truth_summary(base.stdout)
        show("timing queue 1 without the layer", exit=base.exit_code, queue_full=sb.get("queue_full"),
             spurious=sb.get("queue_full_spurious"), final_size=sb.get("final_queue_size"),
             anomalies=sb.get("anomalies"))
        s = self.check_app_ok(run)
        show("timing queue 1 with the layer", queue_full=s.get("queue_full"), spurious=s.get("queue_full_spurious"),
             final_size=s.get("final_queue_size"), rows=len(run.rows), truth=len(run.truth),
             avg_interval=sum(run.truth_intervals_ms()) / len(run.truth_intervals_ms()))
        self.assertGreaterEqual(s.get("queue_full", 0), 1, "a 1-slot queue overflows")
        self.assertEqual(s.get("queue_full_spurious"), 0, "the layer rejects only when the app's own slots are used up")
        self.assertGreater(s.get("final_queue_size", 0), 1, "the queue grew")
        self.assertGreater(len(run.rows), 0.8 * len(run.truth))
        self.check_csv_has_display(run)

    def test_dxvk2_like_v1_present_ids(self):
        """DXVK 2.x / vkd3d-proton style: VK_KHR_present_id (v1) ids plus vkWaitForPresentKHR, no present timing of
        its own.  The layer adds its own timing requests keyed by the app's v1 ids (this driver reports v1 ids in
        the timing results, and v1 and v2 ids share one namespace), so every frame gets display times, the app's
        own present waits keep working, and the pacing is the same as without the layer."""
        args = ["--seconds", "3", "--present-mode", "fifo", "--app-present-id", "v1"]
        base = pt.run_scenario(args, layer=False, capture=False)
        run = pt.run_scenario(args)
        for r in (base, run):
            self.need(r)
            self.assertRun(r)
        sb, sl = pt.truth_summary(base.stdout), pt.truth_summary(run.stdout)
        show("v1 present ids", waits_base=sb.get("present_waits"), waits_layer=sl.get("present_waits"),
             timeouts_layer=sl.get("present_wait_timeouts"), rows=len(run.rows), truth=len(run.truth),
             mean_base=sum(base.truth_intervals_ms()) / len(base.truth_intervals_ms()),
             mean_layer=sum(run.truth_intervals_ms()) / len(run.truth_intervals_ms()))
        self.assertIn("present_id + present_wait (v1)", run.stdout)
        self.assertEqual(sl.get("present_wait_timeouts"), 0, "the app's own vkWaitForPresentKHR keeps working")
        self.assertGreater(sl.get("present_waits", 0), 100)
        self.check_csv_has_display(run)
        self.assertIn(len(run.truth) - len(run.rows), (2, 3))
        ab = sum(base.truth_intervals_ms()) / len(base.truth_intervals_ms())
        al = sum(run.truth_intervals_ms()) / len(run.truth_intervals_ms())
        self.assertAlmostEqual(al, ab, delta=ab * 0.01, msg="mean frame interval within 1% of the run without the layer")

    def test_engine_name_dxvk(self):
        run = pt.run_scenario(["--seconds", "2", "--present-mode", "fifo", "--engine-name", "DXVK"])
        self.need(run)
        self.assertRun(run)
        self.assertEqual({r["PresentRuntime"] for r in run.rows}, {"DXVK"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
