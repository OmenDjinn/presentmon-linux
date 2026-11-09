# SPDX-License-Identifier: MIT
"""Scenario 7: two vk_truth processes at once: unfiltered capture, --multi_csv, per-process overlay control."""
import glob
import os
import re
import subprocess
import time
import unittest

import pmltest as pt
from pmltest import col, med, show, trim


def overlay_states(text):
    """'vk_truth[123]: overlay visible, top-left, ...' lines -> {pid: 'visible'|'hidden'}"""
    out = {}
    for m in re.finditer(r"\[(\d+)\]: overlay (visible|hidden)", text):
        out[int(m.group(1))] = m.group(2)
    return out


class TwoProcesses(pt.LiveCase):
    state = None

    @classmethod
    def run_pair(cls):
        if cls.state is not None:
            return cls.state
        d = pt.workdir()
        st = {"dir": d}
        env = pt.child_env(overlay="0")
        pm_env = dict(os.environ)
        pm_env.pop("PRESENTMON_LAYER", None)
        with pt.desktop_lock():
            pt.ensure_awake()
            logs = {k: os.path.join(d, "truth_%s.csv" % k) for k in "ab"}
            a = pt.spawn([pt.VK_TRUTH, "--seconds", "7", "--present-mode", "fifo", "--size", "800x450", "--log", logs["a"]],
                         env, subprocess.DEVNULL, subprocess.DEVNULL)
            b = pt.spawn([pt.VK_TRUTH, "--seconds", "7", "--present-mode", "mailbox", "--fps", "200", "--size", "640x360",
                          "--log", logs["b"]], env, subprocess.DEVNULL, subprocess.DEVNULL)
            st["pids"] = (a.pid, b.pid)
            errs = {}
            # Layer processes that were already running (a game on a shared desktop) would keep the unfiltered
            # captures from ever terminating: leave them out.
            others = []
            for line in pt.run_pm(["list"])[1].splitlines():
                m = re.match(r"^(\S.*)\[(\d+)\]\s", line)
                if m and m.group(1) != "vk_truth" and m.group(1) not in others:
                    others += [m.group(1)]
            exclude = [arg for name in others for arg in ("--exclude", name)]
            st["excluded"] = others

            def start_pm(name, args):
                f = open(os.path.join(d, name + ".err"), "w+")
                errs[name] = f
                return pt.spawn([pt.PRESENTMON] + args + exclude + ["--no_console_stats", "--terminate_on_proc_exit"], pm_env,
                                subprocess.DEVNULL, f)
            all_csv = os.path.join(d, "all.csv")
            multi = os.path.join(d, "multi.csv")
            pm_all = start_pm("all", ["--output_file", all_csv])
            pm_multi = start_pm("multi", ["--output_file", multi, "--multi_csv"])
            st["segments"] = (pt.wait_segment(a.pid, 15), pt.wait_segment(b.pid, 15))
            time.sleep(1.5)
            # overlay control, while both run: toggle only A
            st["status0"] = pt.run_pm(["overlay", "status"])[1]
            st["toggle_a"] = pt.run_pm(["overlay", "toggle", "--process_id", str(a.pid)])
            st["status1"] = pt.run_pm(["overlay", "status"])[1]
            st["status1_b"] = pt.run_pm(["overlay", "status", "--process_id", str(b.pid)])[1]
            st["list_b"] = pt.run_pm(["list", "--process_id", str(b.pid)])[1]
            st["toggle_b"] = pt.run_pm(["overlay", "show", "--process_id", str(b.pid)])
            st["status2"] = pt.run_pm(["overlay", "status"])[1]
            st["hide_a"] = pt.run_pm(["overlay", "hide", "--process_id", str(a.pid)])
            st["status3"] = pt.run_pm(["overlay", "status"])[1]
            for p in (a, b):
                try:
                    p.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    pt.os.killpg(p.pid, 9)
            st["exit"] = (a.returncode, b.returncode)
            for name, p in (("all", pm_all), ("multi", pm_multi)):
                try:
                    p.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    pt.os.killpg(p.pid, 9)
                    p.wait()
                st["pm_rc_" + name] = p.returncode
                errs[name].seek(0)
                st["pm_err_" + name] = errs[name].read()
                errs[name].close()
        st["all_csv"] = all_csv
        st["multi_files"] = sorted(glob.glob(os.path.join(d, "multi*.csv")))
        st["truth"] = {k: pt.read_truth(v) for k, v in logs.items() if os.path.exists(v)}
        st["asleep"] = bool(pt.displays_asleep())
        cls.state = st
        return st

    def run_pair_or_skip(self):
        st = self.run_pair()
        if st["asleep"]:
            self.skipTest("displays went to sleep (DPMS Off) during the run")
        return st

    def test_unfiltered_capture_gets_both(self):
        st = self.run_pair_or_skip()
        self.assertEqual(st["exit"], (0, 0))
        self.assertEqual(st["pm_rc_all"], 0, st["pm_err_all"])
        header, rows = pt.read_csv(st["all_csv"])
        a, b = st["pids"]
        by_pid = {}
        for r in rows:
            by_pid.setdefault(int(r["ProcessID"]), []).append(r)
        show("unfiltered capture", rows_a=len(by_pid.get(a, [])), rows_b=len(by_pid.get(b, [])),
             truth_a=len(st["truth"]["a"]), truth_b=len(st["truth"]["b"]), other_pids=sorted(set(by_pid) - {a, b}))
        self.assertIn(a, by_pid)
        self.assertIn(b, by_pid)
        self.assertGreaterEqual(len(by_pid[a]), len(st["truth"]["a"]) - 4 - 0.1 * len(st["truth"]["a"]))
        self.assertGreaterEqual(len(by_pid[b]), len(st["truth"]["b"]) - 4 - 0.1 * len(st["truth"]["b"]))
        self.assertEqual({r["PresentMode"] for r in by_pid[a]}, {"FIFO"})
        self.assertEqual({r["PresentMode"] for r in by_pid[b]}, {"MAILBOX"})
        self.assertAlmostEqual(med(trim(col(by_pid[b], "MsBetweenPresents"))), 5.0, delta=0.2)

    def test_multi_csv_one_file_per_process(self):
        st = self.run_pair_or_skip()
        a, b = st["pids"]
        self.assertEqual(st["pm_rc_multi"], 0, st["pm_err_multi"])
        files = {}
        for f in st["multi_files"]:
            m = re.search(r"-(\d+)\.csv$", f)
            self.assertTrue(m, "file name carries the pid: " + f)
            files[int(m.group(1))] = f
        show("multi_csv", files=[os.path.basename(f) for f in st["multi_files"]])
        self.assertIn(a, files)
        self.assertIn(b, files)
        for pid in (a, b):
            self.assertTrue("vk_truth" in os.path.basename(files[pid]))
            self.assertTrue(pt.has_bom(files[pid]))
            _, rows = pt.read_csv(files[pid])
            self.assertGreater(len(rows), 100)
            self.assertEqual({r["ProcessID"] for r in rows}, {str(pid)}, "one process per file")

    def test_overlay_toggle_changes_only_the_target(self):
        st = self.run_pair_or_skip()
        a, b = st["pids"]
        s0, s1, s2, s3 = (overlay_states(st[k]) for k in ("status0", "status1", "status2", "status3"))
        show("overlay states", before=(s0.get(a), s0.get(b)), after_toggle_a=(s1.get(a), s1.get(b)),
             after_show_b=(s2.get(a), s2.get(b)), after_hide_a=(s3.get(a), s3.get(b)))
        self.assertEqual((s0.get(a), s0.get(b)), ("hidden", "hidden"), "PRESENTMON_OVERLAY=0 starts hidden")
        self.assertEqual(st["toggle_a"][0], 0)
        self.assertEqual(overlay_states(st["toggle_a"][1]), {a: "visible"}, "toggle --process_id prints only that process")
        self.assertEqual((s1.get(a), s1.get(b)), ("visible", "hidden"))
        self.assertEqual(overlay_states(st["status1_b"]), {b: "hidden"})
        self.assertIn("[%d]" % b, st["list_b"])
        self.assertNotIn("[%d]" % a, st["list_b"], "list --process_id lists only that process")
        self.assertIn("overlay: hidden", st["list_b"])
        self.assertEqual((s2.get(a), s2.get(b)), ("visible", "visible"))
        self.assertEqual((s3.get(a), s3.get(b)), ("hidden", "visible"))
        self.assertTrue(st["segments"][0] and st["segments"][1])


if __name__ == "__main__":
    unittest.main(verbosity=2)
