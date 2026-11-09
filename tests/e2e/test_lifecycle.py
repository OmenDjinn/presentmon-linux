# SPDX-License-Identifier: MIT
"""Scenario 6: lifecycle.  kill -9 of the app mid-capture, what is left behind, and swapchain recreation."""
import glob
import mmap
import os
import signal
import struct
import subprocess
import threading
import time
import unittest

import pmltest as pt
from pmltest import col, med, show, trim

# src/common/include/pml/protocol.hpp (frozen layout, protocol version 1): magic at 0, version at 4,
# heartbeatNs (CLOCK_MONOTONIC of the last present) at 1160.
SEGMENT_MAGIC = 0x314C4D50
SEGMENT_VERSION = 1
HEARTBEAT_OFFSET = 1160


class ReaperWatch:
    """Notes, while active, every other process that may remove stale segments of this /dev/shm with the collector's
    10 s rule: presentmon captures and the unit-test binaries that call removeDeadSegments (comm names are cut to
    15 characters)."""
    NAMES = {"presentmon", "pml_common_test", "pml_collector_t"}

    def __init__(self):
        self.seen = set()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        while not self._stop.is_set():
            for comm in glob.glob("/proc/[0-9]*/comm"):
                try:
                    with open(comm) as f:
                        name = f.read().strip()
                except OSError:
                    continue
                if name in self.NAMES:
                    self.seen.add(name)
            self._stop.wait(0.05)

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        self._thread.join()
        return False


def age_heartbeat(path, seconds):
    """Make a (dead) segment look silent for `seconds`, as if its process had crashed that long ago."""
    with open(path, "r+b") as f:
        m = mmap.mmap(f.fileno(), 4096)
        try:
            magic, version = struct.unpack_from("<II", m, 0)
            if (magic, version) != (SEGMENT_MAGIC, SEGMENT_VERSION):
                raise unittest.SkipTest("segment layout %#x/%d is not the one this test knows" % (magic, version))
            struct.pack_into("<Q", m, HEARTBEAT_OFFSET, time.clock_gettime_ns(time.CLOCK_MONOTONIC) - seconds * 10**9)
        finally:
            m.close()


class KillNine(pt.LiveCase):
    states = {}

    @classmethod
    def kill_scenario(cls, key="main"):
        """Start app + capture, SIGKILL the app after a few seconds of frames, observe everything once per key (each
        key leaves its own dead segment behind)."""
        if key in cls.states:
            return cls.states[key]
        d = pt.workdir()
        st = {"dir": d, "csv": os.path.join(d, "kill.csv")}
        env = pt.child_env(overlay="off")
        with pt.desktop_lock():
            pt.ensure_awake()
            app = pt.spawn([pt.VK_TRUTH, "--present-mode", "fifo"], env, subprocess.DEVNULL, subprocess.DEVNULL)
            st["pid"] = app.pid
            pm_env = dict(os.environ)
            pm_env.pop("PRESENTMON_LAYER", None)
            pm_err = open(os.path.join(d, "pm.err"), "w+")
            pm = pt.spawn([pt.PRESENTMON, "--process_id", str(app.pid), "--output_file", st["csv"], "--no_console_stats",
                           "--terminate_on_proc_exit"], pm_env, subprocess.DEVNULL, pm_err)
            st["segment_seen"] = pt.wait_segment(app.pid, 15)
            time.sleep(2.5)
            st["list_before"] = pt.run_pm(["list", "--process_id", str(app.pid)])[1]
            t0 = time.time()
            os.kill(app.pid, signal.SIGKILL)
            app.wait()
            try:
                pm.wait(timeout=10)
                st["pm_exit_s"] = time.time() - t0
            except subprocess.TimeoutExpired:
                st["pm_exit_s"] = None
                os.killpg(pm.pid, signal.SIGKILL)
                pm.wait()
            st["pm_rc"] = pm.returncode
            st["kill_time"] = t0
        pm_err.seek(0)
        st["pm_stderr"] = pm_err.read()
        pm_err.close()
        st["list_after_all"] = pt.run_pm(["list"])[1]
        st["list_after_pid"] = pt.run_pm(["list", "--process_id", str(app.pid)])[1]
        st["segment_after_kill"] = os.path.exists(pt.SEGMENT_FMT % app.pid)
        st["asleep"] = bool(pt.displays_asleep())
        cls.states[key] = st
        return st

    @classmethod
    def tearDownClass(cls):
        # The SIGKILLed apps' segments are left behind by design; do not leave them to the next test run.
        for st in cls.states.values():
            try:
                os.unlink(pt.SEGMENT_FMT % st["pid"])
            except OSError:
                pass
        super().tearDownClass()

    def state_or_skip(self, key="main"):
        st = self.kill_scenario(key)
        if st["asleep"]:
            self.skipTest("displays went to sleep (DPMS Off) during the run")
        return st

    def test_collector_exits_quickly_with_wellformed_csv(self):
        st = self.state_or_skip()
        self.assertTrue(st["segment_seen"], "layer created its segment")
        show("kill -9", pm_exit_seconds=st["pm_exit_s"], pm_rc=st["pm_rc"], segment_left_behind=st["segment_after_kill"])
        self.assertIsNotNone(st["pm_exit_s"], "presentmon --terminate_on_proc_exit exits after the app is killed\n" + st["pm_stderr"])
        self.assertLess(st["pm_exit_s"], 5.0)
        self.assertEqual(st["pm_rc"], 0, st["pm_stderr"])
        self.assertTrue(pt.has_bom(st["csv"]))
        with open(st["csv"], "rb") as f:
            raw = f.read()
        self.assertTrue(raw.endswith(b"\n"), "CSV ends on a complete line")
        header, rows = pt.read_csv(st["csv"])
        self.assertGreater(len(rows), 200, "about 2.5 s of frames were recorded")
        for r in rows:
            self.assertNotIn(None, r.values(), "every row has all columns: %r" % r)
            self.assertNotIn(None, r, "no row has more fields than the header")
        self.assertEqual({r["ProcessID"] for r in rows}, {str(st["pid"])})

    def test_list_does_not_show_dead_process(self):
        """E2: 'presentmon list' shows live layer-enabled processes only; a SIGKILLed app disappears from it right
        away, although its segment is still in /dev/shm."""
        st = self.state_or_skip()
        self.assertIn("state=live", st["list_before"])
        pid = "[%d]" % st["pid"]
        self.assertNotIn(pid, st["list_after_pid"])
        self.assertNotIn(pid, st["list_after_all"])

    def wait_until_stale(self, st, seg):
        """Wait until the dead producer's heartbeat is older than the 10 s staleness window.  Nothing may remove the
        segment while its heartbeat is fresh.  Once it is stale, any collector in this /dev/shm may remove it (unit
        tests running at the same time start many): then this scenario cannot be observed, and the test skips."""
        self.assertTrue(os.path.exists(seg), "kill -9 leaves the segment behind")
        while time.time() - st["kill_time"] < 10.8:
            if not os.path.exists(seg):
                age = time.time() - st["kill_time"]
                self.assertGreaterEqual(age, 9.5, "the segment was removed %.1f s after the kill, while its heartbeat "
                                                  "was fresh" % age)
                self.skipTest("another process reaped the stale segment first (tests running at the same time?)")
            time.sleep(0.1)

    def test_stale_segment_removed_by_next_collector(self):
        """A dead process's segment stays until its heartbeat is older than 10 s; then the next capture that starts
        unlinks it.  (A layer-enabled app starting meanwhile would not: Segment::create only reaps segments silent
        for 5 minutes.)"""
        with pt.desktop_lock():
            st = self.state_or_skip("collector")
            seg = pt.SEGMENT_FMT % st["pid"]
            self.wait_until_stale(st, seg)
            rc, out, err = pt.run_pm(["--process_name", "no_such_process_e2e", "--timed", "1",
                                      "--terminate_after_timed", "--no_csv", "--no_console_stats"], timeout=20)
        gone = not os.path.exists(seg)
        show("stale segment, next collector", removed=gone,
             collector_log=err.strip().splitlines()[-3:] if err.strip() else "")
        self.assertTrue(gone, "a later collector removes the stale segment\n" + err)
        if "removed stale segment" not in err:
            self.skipTest("another process removed the stale segment just before this collector looked")

    def test_stale_segment_reaped_by_next_layer(self):
        """The layer of a newly started app removes the segment of a dead one when it creates its own, with no
        presentmon running, but only once that segment has been silent for 5 minutes (kCreateReapAfterNs): pids are
        per pid namespace, and a live game in a sandbox that shares /dev/shm looks just like a dead one from here.
        The 5 minutes are simulated by writing an old heartbeat into the dead segment."""
        with pt.desktop_lock():
            st = self.state_or_skip("layer")
            seg = pt.SEGMENT_FMT % st["pid"]
            self.wait_until_stale(st, seg)
            with ReaperWatch() as watch:
                run = pt.run_scenario(["--seconds", "1", "--present-mode", "fifo"], capture=False)
            self.need(run)
            self.assertRun(run)
            kept = os.path.exists(seg)
            if not kept and watch.seen:
                self.skipTest("the stale segment disappeared while other reapers ran (%s)" % ", ".join(sorted(watch.seen)))
            if not os.path.exists(seg):
                self.fail("a layer reaped a segment silent for only 11 s")
            age_heartbeat(seg, 400)
            run = pt.run_scenario(["--seconds", "1", "--present-mode", "fifo"], capture=False)
        self.need(run)
        self.assertRun(run)
        show("stale segment, next layer", kept_when_silent_11s=kept, removed_when_silent_400s=not os.path.exists(seg))
        self.assertFalse(os.path.exists(seg), "the new app's layer reaped the long-silent dead segment")


class SwapchainRecreation(pt.LiveCase):
    def test_resize_keeps_rows_flowing(self):
        run = pt.run_scenario(["--seconds", "4", "--present-mode", "fifo", "--resize-test"])
        self.need(run)
        self.assertRun(run)
        s = pt.truth_summary(run.stdout)
        addrs = []
        for r in run.rows:
            if not addrs or addrs[-1] != r["SwapChainAddress"]:
                addrs.append(r["SwapChainAddress"])
        show("resize-test", recreations=s.get("recreations"), rows=len(run.rows), truth=len(run.truth),
             address_changes=len(addrs), distinct_addresses=len(set(addrs)))
        self.assertGreaterEqual(s.get("recreations", 0), 4, "the app really recreated its swapchain")
        self.assertGreaterEqual(len(addrs), 3, "rows continue under a new SwapChainAddress after a recreation")
        self.assertGreaterEqual(len(set(addrs)), 3)
        # Every swapchain loses at most its last couple of presents; nothing else.
        self.assertGreaterEqual(len(run.rows), len(run.truth) - 4 * (s.get("recreations", 0) + 2))
        times = col(run.rows, "TimeInQPC")
        self.assertEqual(len(times), len(run.rows))
        # rows of the final swapchain arrive up to the end of the run
        last_truth = run.truth[-1]["present_pre_ns"]
        self.assertLess((last_truth - max(times)) / 1e6, 200.0)
        # no gap longer than 300 ms anywhere in the row stream (recreation is quick)
        ts = sorted(times)
        gaps = [(b - a) / 1e6 for a, b in zip(ts, ts[1:])]
        show("resize gaps", max_gap_ms=max(gaps))
        self.assertLess(max(gaps), 300.0)
        self.assertEqual({r["Application"] for r in run.rows}, {"vk_truth"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
