# SPDX-License-Identifier: MIT
"""Scenario 2: fullscreen on each monitor (FIFO interval = that monitor's refresh interval) and the VRR check."""
import unittest

import pmltest as pt
from pmltest import col, med, show, trim


@pt.not_on_shared_desktop("fullscreen takes over a whole screen")
class Fullscreen(pt.LiveCase):
    displays = []

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.displays = pt.list_displays()

    def display(self, index):
        for d in self.displays:
            if d["index"] == index:
                return d
        self.skipTest("no SDL display %d on this machine (have %d)" % (index, len(self.displays)))

    def fifo_fullscreen(self, index):
        disp = self.display(index)
        period = 1000.0 / disp["hz"]
        run = pt.run_scenario(["--seconds", "3", "--present-mode", "fifo", "--fullscreen", "--display", str(index)])
        self.need(run)
        self.assertRun(run)
        bp = trim(col(run.rows, "MsBetweenPresents"))
        bdc = trim(col(run.rows, "MsBetweenDisplayChange"))
        until = col(run.rows, "MsUntilDisplayed")
        show("fullscreen display %d (%s %dx%d@%.2f)" % (index, disp["name"], disp["width"], disp["height"], disp["hz"]),
             period_ms=period, bp_median=med(bp), bdc_median=med(bdc), rows=len(run.rows), displayed=len(until))
        self.assertGreater(len(run.rows), 100)
        self.assertAlmostEqual(med(bp), period, delta=period * 0.02, msg="FIFO frame interval = refresh interval")
        self.assertAlmostEqual(med(bdc), period, delta=period * 0.02)
        self.assertGreaterEqual(max(pt.multiple_fraction(bdc, period, 0.3), pt.block_multiple_fraction(bdc, period)), 0.95)
        self.assertGreaterEqual(len(until), 0.99 * len(run.rows))
        self.assertTrue(all(u > 0 for u in until))
        # The machine description says DP-2 is 240 Hz and DP-1 120 Hz; only report, the live mode is authoritative.
        return run, period

    def test_fifo_display_0(self):
        self.fifo_fullscreen(0)

    def test_fifo_display_1(self):
        self.fifo_fullscreen(1)

    def test_vrr_cap_below_refresh(self):
        """Fullscreen on display 0 with a 150 fps cap: with VRR the display changes follow the frame rate, without
        it they sit on refresh multiples.  KWin (policy Automatic) runs VRR only while the test window is the active
        fullscreen window, so a click on another window, or a window rule that blocks focus, turns it off, also in
        the middle of a run.  So every frame is checked against the output's VRR state at its display time, read
        from the CRTC's VRR_ENABLED property (DRM, read-only).  Without DRM access the run is only classified from
        its display changes.  VRR on, off or both are accepted; the result is printed."""
        disp = self.display(0)
        refresh = 1000.0 / disp["hz"]
        cap = 150.0
        if disp["hz"] <= cap * 1.2:
            self.skipTest("display 0 refresh %.1f Hz is not clearly above the %.0f fps cap" % (disp["hz"], cap))
        target, why = pt.drm_connector_for_display(disp["name"])
        verdicts = {}
        ever_on = False
        for mode in ("mailbox", "fifo"):
            with pt.VrrWatch(target, why) as watch:
                run = pt.run_scenario(["--seconds", "3", "--present-mode", mode, "--fps", "150", "--fullscreen",
                                       "--display", "0"])
            self.need(run)
            self.assertRun(run)
            frames = pt.display_frames(run.rows)
            self.assertGreater(len(frames), 100, "display changes are reported")
            pattern = pt.vrr_pattern(frames, refresh)
            if watch.available:
                c = pt.vrr_check(frames, watch.steps, refresh)
                ever_on = ever_on or c["on"] > 0
                verdict = ("VRR on for %.0f%% of the judged frames" % (100.0 * c["on"] / (c["on"] + c["off"]))
                           if c["on"] else "VRR off (VRR_ENABLED stayed 0)")
                show("vrr %s cap 150" % mode, connector=target[2], crtc=watch.crtc,
                     vrr_enabled=pt.describe_steps(watch.steps, frames[0][0]),
                     frames_on=c["on"], frames_off=c["off"], follow_share=c["follow"], multiple_share=c["multiple"],
                     pattern=pattern)
                self.assertTrue(c["follow"] is not None or c["multiple"] is not None, "no frame could be judged")
                if c["follow"] is not None:
                    self.assertGreaterEqual(c["follow"], 0.75, "while VRR_ENABLED=1 the display changes must follow "
                                            "the present intervals (%s)" % pattern)
                if c["multiple"] is not None:
                    self.assertGreaterEqual(c["multiple"], 0.80, "while VRR_ENABLED=0 the display changes must sit on "
                                            "%.3f ms refresh multiples (%s)" % (refresh, pattern))
            else:
                ever_on = ever_on or "V" in pattern
                judged = (pattern.count("V") + pattern.count("f")) / max(1, len(pattern))
                verdict = ("VRR on" if "f" not in pattern else "VRR on and off") if "V" in pattern else "VRR off"
                show("vrr %s cap 150" % mode, drm=watch.reason.replace(" ", "_"), pattern=pattern, judged=judged)
                # Each VRR switch leaves an ambiguous bin ('?'); a run that flapped three times was 75% judged.
                self.assertGreaterEqual(judged, 0.6, "display changes neither follow the %.2f ms frame interval nor "
                                        "sit on %.3f ms refresh multiples: %s" % (1000.0 / cap, refresh, pattern))
            verdicts[mode] = verdict
        print("[measure] VRR verdict: " + "; ".join("%s: %s" % kv for kv in verdicts.items()), flush=True)
        if not ever_on:
            print("[measure] VRR never engaged.  KWin's Automatic policy needs the test window to be the active "
                  "fullscreen window: a window rule that blocks its focus, or a click elsewhere, keeps VRR off.",
                  flush=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
