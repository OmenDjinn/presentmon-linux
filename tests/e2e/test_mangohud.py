# SPDX-License-Identifier: MIT
"""Scenario 8: coexistence with MangoHud (VK_LAYER_MANGOHUD_overlay, MANGOHUD=1) next to our layer."""
import os
import shutil
import unittest

import pmltest as pt
from pmltest import col, med, show, trim

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def mangohud_installed():
    for d in ("/usr/share/vulkan/implicit_layer.d", "/etc/vulkan/implicit_layer.d",
              os.path.expanduser("~/.local/share/vulkan/implicit_layer.d")):
        if os.path.isdir(d) and any("MangoHud" in f or "mangohud" in f for f in os.listdir(d)):
            return True
    return False


class MangoHud(pt.LiveCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        if not mangohud_installed():
            raise unittest.SkipTest("MangoHud implicit layer is not installed")

    def test_both_layers_active(self):
        d = pt.workdir()
        png = os.path.join(d, "overlay.png")
        run = pt.run_scenario(["--seconds", "3", "--present-mode", "fifo"], overlay="1", mangohud=True, wdir=d,
                              env_extra={"PRESENTMON_OVERLAY_DUMP": png, "PRESENTMON_OVERLAY_DUMP_FRAME": "120",
                                         "PRESENTMON_LOG": "info"})
        self.need(run)
        self.assertRun(run)
        mango_active = "MANGOHUD" in run.stderr or "MangoHud" in run.stderr or "mangohud" in run.stderr.lower()
        d_cmp = pt.truth_compare(run)
        show("mangohud + presentmon", mangohud_log_seen=mango_active, **d_cmp)
        self.assertTrue(mango_active, "MangoHud really loaded (its log line is missing)\n" + run.stderr[-1500:])
        self.assertTrue(d_cmp["contiguous"])
        self.assertIn(d_cmp["truth"] - d_cmp["rows"], (2, 3))
        self.assertLess(d_cmp["bp_absdiff_med"], 0.01)
        bp = med(trim(col(run.rows, "MsBetweenPresents")))
        self.assertTrue(3.5 < bp < 9.0, "plausible FIFO frame interval, got %.3f ms" % bp)
        self.assertEqual({r["Application"] for r in run.rows}, {"vk_truth"})
        # our overlay still draws: the dump of frame 120 exists and is a PNG
        self.assertTrue(os.path.exists(png), "overlay dump was written\n" + run.stderr[-1500:])
        with open(png, "rb") as f:
            head = f.read(8)
        self.assertEqual(head, PNG_MAGIC)
        self.assertGreater(os.path.getsize(png), 10000)
        self.assertTrue(len(col(run.rows, "MsUntilDisplayed")) > 0.95 * len(run.rows))


if __name__ == "__main__":
    unittest.main(verbosity=2)
