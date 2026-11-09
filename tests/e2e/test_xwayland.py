# SPDX-License-Identifier: MIT
"""Scenario 5: Xwayland fallback (--wsi x11).  The driver only reports QUEUE_OPERATIONS_END there, so every display
column must be NA, CPU columns stay valid and nothing crashes."""
import unittest

import pmltest as pt
from pmltest import col, med, show, trim

DISPLAY_COLUMNS = ("MsBetweenDisplayChange", "MsUntilDisplayed", "MsRenderPresentLatency")


class Xwayland(pt.LiveCase):
    def x11_run(self, extra, **kw):
        run = pt.run_scenario(["--seconds", "3", "--wsi", "x11"] + extra, **kw)
        if run.exit_code != 0 and ("x11" in run.stderr.lower() or "no available video device" in run.stderr.lower()):
            self.skipTest("Xwayland not usable: " + run.stderr.strip().splitlines()[-1])
        self.need(run)
        self.assertRun(run)
        self.assertIn("video driver x11", run.stdout, "really ran through Xwayland")
        return run

    def test_display_columns_na_cpu_columns_valid(self):
        run = self.x11_run(["--present-mode", "fifo"])
        d = pt.truth_compare(run)
        show("xwayland fifo", **d)
        self.assertGreater(len(run.rows), 100)
        self.assertEqual(d["matched"], d["rows"])
        # Xwayland recreates the swapchain at startup; each swapchain loses its last couple of presents.
        self.assertGreaterEqual(d["rows"], d["truth"] - 8)
        self.assertLess(d["bp_absdiff_med"], 0.01)
        self.assertLess(abs(d["inpresent_diff_med"]), 0.05)
        for name in ("MsBetweenDisplayChange", "MsUntilDisplayed"):
            self.assertEqual(col(run.rows, name), [], name + " is NA on Xwayland")
        # CPU columns are valid on every row
        for name in ("MsBetweenPresents", "MsInPresentAPI", "MsCPUBusy", "MsCPUWait"):
            self.assertEqual(len(col(run.rows, name)), len(run.rows), name)
        self.assertTrue(all(v >= 0 for v in col(run.rows, "MsCPUBusy")))
        self.assertEqual({r["PresentMode"] for r in run.rows}, {"FIFO"})
        self.assertEqual({r["Application"] for r in run.rows}, {"vk_truth"})
        iv = med(trim(col(run.rows, "MsBetweenPresents")))
        self.assertGreater(iv, 1.0)

    def test_dxvk_like_app_on_xwayland(self):
        run = self.x11_run(["--present-mode", "fifo", "--app-timing", "--app-present-id"])
        s = pt.truth_summary(run.stdout)
        show("xwayland app-timing", **s)
        self.assertEqual(s.get("anomalies"), 0)
        self.assertEqual(s.get("reported"), s.get("requested"))
        self.assertEqual(col(run.rows, "MsUntilDisplayed"), [])
        self.assertGreater(len(run.rows), 100)

    def test_immediate_on_xwayland(self):
        run = self.x11_run(["--present-mode", "immediate", "--fps", "500"])
        self.assertGreater(len(run.rows), 500)
        self.assertEqual(col(run.rows, "MsUntilDisplayed"), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
