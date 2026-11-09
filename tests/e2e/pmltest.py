# SPDX-License-Identifier: MIT
"""Shared helpers for the presentmon-linux end-to-end tests (Python 3 stdlib only).

A scenario starts vk_truth (optionally under the build-tree layer), attaches a 'presentmon' capture, waits for
both and returns a Run with the parsed CSV rows, the ground-truth log and the raw outputs.  Every child process
is started in its own session and killed in tearDown / at interpreter exit, so a failing test cannot leave
windows, captures or shared-memory segments behind.

Environment:
  PML_BUILD_DIR      build tree (set by ctest)
  PML_DESKTOP_LOCK   optional lock file; each scenario that opens a window holds an flock on it, so several test
                     drivers (or agents) sharing one desktop do not disturb each other's timing
  PML_E2E_KEEP       directory to keep the work files in (default: a temporary directory that is deleted)
  PML_E2E_WAKE       1 = wake sleeping displays with kscreen-doctor --dpms on before a run
  PML_E2E_SHARED_DESKTOP
                     1 = someone is using the desktop (e.g. playing a game on the main screen): tests that go
                     fullscreen, load the GPU or flood the compositor are skipped.  Test windows are expected to be
                     kept below other windows (or on a spare screen) without focus by a KWin window rule (see
                     README.md, "Testing next to a game").
"""
import atexit
import bisect
import contextlib
import csv
import ctypes
import fcntl
import glob
import os
import shutil
import signal
import statistics
import subprocess
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.environ.get("PML_BUILD_DIR") or os.path.normpath(os.path.join(HERE, "..", "..", "build", "e2e"))
BIN = os.path.join(BUILD, "bin")
LAYER_DIR = os.path.join(BUILD, "layer")
VK_TRUTH = os.path.join(BIN, "vk_truth")
PRESENTMON = os.path.join(BIN, "presentmon")
RING_DUMP = os.path.join(BIN, "ring_dump")
OVERLAY_CTL = os.path.join(BIN, "overlay_ctl")
LOCK_PATH = os.environ.get("PML_DESKTOP_LOCK", "")
SHARED_DESKTOP = os.environ.get("PML_E2E_SHARED_DESKTOP") == "1"

SEGMENT_FMT = "/dev/shm/presentmon-linux.%d"
_children = []
_workdirs = []


# ---------------------------------------------------------------------------------------
# Environment checks
# ---------------------------------------------------------------------------------------

def skip_reason():
    """None when the live tests can run, else a human-readable reason."""
    if not os.environ.get("WAYLAND_DISPLAY"):
        return "WAYLAND_DISPLAY is not set (no Wayland session)"
    for p in (VK_TRUTH, PRESENTMON, os.path.join(LAYER_DIR, "libVkLayer_presentmon.so")):
        if not os.path.exists(p):
            return "missing %s (build with PML_BUILD_TEST_APPS/LAYER/COLLECTOR)" % p
    asleep = [] if ensure_awake() else displays_asleep()
    if asleep:
        return ("connected display(s) %s are in DPMS Off: KWin throttles windows to ~20 fps and timing tests are "
                "meaningless (wake the screens, or set PML_E2E_WAKE=1 to let the tests run kscreen-doctor --dpms on)" % ", ".join(asleep))
    return None


def displays_asleep():
    """Names of connected DRM connectors whose DPMS state is Off (Linux sysfs; empty when unknown)."""
    out = []
    try:
        for d in sorted(os.listdir("/sys/class/drm")):
            base = os.path.join("/sys/class/drm", d)
            try:
                with open(os.path.join(base, "status")) as f:
                    status = f.read().strip()
                with open(os.path.join(base, "dpms")) as f:
                    dpms = f.read().strip()
            except OSError:
                continue
            if status == "connected" and dpms == "Off":
                out.append(d)
    except OSError:
        pass
    return out


def ensure_awake():
    """Wake sleeping displays when PML_E2E_WAKE=1 (kscreen-doctor --dpms on).  Off by default: the tests never touch
    power state on their own.  Returns True when no connected display is asleep afterwards."""
    if displays_asleep() and os.environ.get("PML_E2E_WAKE") == "1" and shutil.which("kscreen-doctor"):
        subprocess.run(["kscreen-doctor", "--dpms", "on"], capture_output=True, timeout=20)
        time.sleep(1.0)
    return not displays_asleep()


def not_on_shared_desktop(reason):
    """Decorator for tests (or test classes) that must not run while someone uses the desktop."""
    return unittest.skipIf(SHARED_DESKTOP, "PML_E2E_SHARED_DESKTOP=1: " + reason)


def require_live(case):
    """Use as @unittest.skipIf-style guard in setUpClass."""
    why = skip_reason()
    if why:
        raise unittest.SkipTest(why)


def _kill_group(proc, sig=signal.SIGKILL):
    try:
        os.killpg(proc.pid, sig)
    except (ProcessLookupError, PermissionError):
        pass


def _cleanup_all():
    for p in _children:
        if p.poll() is None:
            _kill_group(p)
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
    for d in _workdirs:
        shutil.rmtree(d, ignore_errors=True)


atexit.register(_cleanup_all)


def spawn(cmd, env, stdout, stderr, cwd=None):
    p = subprocess.Popen(cmd, env=env, stdout=stdout, stderr=stderr, cwd=cwd, start_new_session=True)
    _children.append(p)
    return p


def workdir(prefix="pmle2e-"):
    keep = os.environ.get("PML_E2E_KEEP")
    if keep:
        os.makedirs(keep, exist_ok=True)
        d = tempfile.mkdtemp(prefix=prefix, dir=keep)
    else:
        d = tempfile.mkdtemp(prefix=prefix)
        _workdirs.append(d)
    return d


_lock_depth = 0


@contextlib.contextmanager
def desktop_lock():
    """Hold the flock on PML_DESKTOP_LOCK for the duration.  Re-entrant within this process, so a test can keep the
    desktop for a whole sequence (kill, wait, next app) that itself calls run_scenario()."""
    global _lock_depth
    if not LOCK_PATH or _lock_depth:
        _lock_depth += 1
        try:
            yield
        finally:
            _lock_depth -= 1
        return
    with open(LOCK_PATH, "a") as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        _lock_depth += 1
        try:
            yield
        finally:
            _lock_depth -= 1
            fcntl.flock(f, fcntl.LOCK_UN)


# ---------------------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------------------

def num(v):
    """CSV cell -> float, or None for NA / empty."""
    if v is None or v == "" or v == "NA":
        return None
    try:
        return float(v)
    except ValueError:
        return None


def read_csv(path):
    """PresentMon CSV (UTF-8 BOM) -> (header list, list of dict rows)."""
    with open(path, "r", encoding="utf-8-sig", newline="") as f:
        r = csv.DictReader(f)
        rows = list(r)
        return list(r.fieldnames or []), rows


def has_bom(path):
    with open(path, "rb") as f:
        return f.read(3) == b"\xef\xbb\xbf"


def read_truth(path):
    """vk_truth --log -> list of dict with int values (CLOCK_MONOTONIC ns)."""
    rows = []
    with open(path, "r", newline="") as f:
        for r in csv.DictReader(f):
            rows.append({k: int(v) for k, v in r.items()})
    return rows


def col(rows, name):
    """Numeric values of a column, NA skipped."""
    out = []
    for r in rows:
        v = num(r.get(name))
        if v is not None:
            out.append(v)
    return out


def med(xs):
    return statistics.median(xs) if xs else float("nan")


def pct(xs, p):
    if not xs:
        return float("nan")
    s = sorted(xs)
    return s[min(len(s) - 1, int(round(p / 100.0 * (len(s) - 1))))]


def trim(xs, frac=0.1):
    """Drop the first and last fraction of a series (startup / shutdown transients)."""
    n = len(xs)
    k = int(n * frac)
    return xs[k:n - k] if n - 2 * k > 0 else xs


def summarize_out(text, prefix="[vk_truth]"):
    return "\n".join(l for l in text.splitlines() if l.startswith(prefix))


def truth_summary(stdout):
    """Pull numbers out of vk_truth's summary lines."""
    d = {}
    for line in stdout.splitlines():
        if "present interval:" in line:
            # avg 8.274 ms (120.86 fps), median 8.333, min 0.032, max 9.926
            try:
                d["median_interval_ms"] = float(line.split("median")[1].split(",")[0])
                d["avg_interval_ms"] = float(line.split("avg")[1].split("ms")[0])
            except (IndexError, ValueError):
                pass
        if "present timing: requested=" in line:
            for tok in line.replace(",", " ").split():
                if "=" in tok:
                    k, _, v = tok.partition("=")
                    if v.isdigit():
                        d[k] = int(v)
        if "present_wait (v1) timeouts:" in line or "present_wait2 timeouts:" in line:
            d["present_wait_timeouts"] = int(line.rsplit(":", 1)[1])
        if "present waits:" in line:
            # present waits: 360, avg 8.264 ms, max 8.418 ms
            try:
                d["present_waits"] = int(line.split("present waits:")[1].split(",")[0])
            except ValueError:
                pass
        if "present-timing anomalies:" in line:
            d["anomalies"] = int(line.split(":")[1])
        if "summary: frames=" in line:
            d["frames"] = int(line.split("frames=")[1].split()[0])
        if "swapchain_recreations=" in line:
            d["recreations"] = int(line.split("swapchain_recreations=")[1].split()[0])
    return d


# ---------------------------------------------------------------------------------------
# A scenario run
# ---------------------------------------------------------------------------------------

class Run:
    """Everything one scenario produced."""

    def __init__(self):
        self.dir = ""
        self.exit_code = None      # vk_truth exit code
        self.pm_exit_code = None   # presentmon exit code (None = not run)
        self.stdout = ""           # vk_truth stdout
        self.stderr = ""           # vk_truth stderr
        self.pm_stderr = ""
        self.header = []
        self.rows = []
        self.truth = []
        self.pid = None
        self.wall = 0.0
        self.pm_wall = 0.0
        self.csv_path = ""
        self.truth_path = ""
        self.cmd = []
        self.skip = None           # reason when the app could not do the scenario
        self.matched = []          # (csv row, truth index) pairs after match()

    # -- ground truth helpers --
    def truth_intervals_ms(self):
        t = self.truth
        return [(t[i]["present_pre_ns"] - t[i - 1]["present_pre_ns"]) / 1e6 for i in range(1, len(t))]

    def truth_in_present_ms(self):
        return [(r["present_post_ns"] - r["present_pre_ns"]) / 1e6 for r in self.truth]

    def match(self, tol_ms=0.5):
        """Pair every CSV row with the truth frame whose present_pre is nearest to the row's TimeInQPC.
        Needs the capture to run with --qpc_time.  Returns the list of (row, truth_index, dt_ms)."""
        pre = [r["present_pre_ns"] for r in self.truth]
        out = []
        for r in self.rows:
            t = num(r.get("TimeInQPC"))
            if t is None:
                continue
            t = int(t)
            i = bisect.bisect_left(pre, t)
            best = None
            for j in (i - 1, i):
                if 0 <= j < len(pre):
                    d = abs(pre[j] - t) / 1e6
                    if best is None or d < best[1]:
                        best = (j, d)
            if best and best[1] <= tol_ms:
                out.append((r, best[0], best[1]))
        self.matched = out
        return out

    def describe(self):
        return "cmd=%s\nexit=%s pm_exit=%s\n--- vk_truth stdout ---\n%s\n--- vk_truth stderr ---\n%s\n--- presentmon stderr ---\n%s" % (
            " ".join(self.cmd), self.exit_code, self.pm_exit_code, self.stdout[-2500:], self.stderr[-2500:], self.pm_stderr[-2500:])


def child_env(layer=True, overlay="off", extra=None, mangohud=False):
    env = dict(os.environ)
    for k in ("PRESENTMON_LAYER", "DISABLE_PRESENTMON_LAYER", "PRESENTMON_OVERLAY", "MANGOHUD", "PRESENTMON_OVERLAY_DUMP",
              "PRESENTMON_OVERLAY_DUMP_FRAME", "PRESENTMON_LOG"):
        env.pop(k, None)
    if layer:
        env["VK_ADD_IMPLICIT_LAYER_PATH"] = LAYER_DIR + (":" + env["VK_ADD_IMPLICIT_LAYER_PATH"] if env.get("VK_ADD_IMPLICIT_LAYER_PATH") else "")
        env["PRESENTMON_LAYER"] = "1"
        env["PRESENTMON_OVERLAY"] = str(overlay)
    if mangohud:
        env["MANGOHUD"] = "1"
    if extra:
        env.update(extra)
    return env


def wait_segment(pid, timeout=15.0):
    end = time.time() + timeout
    p = SEGMENT_FMT % pid
    while time.time() < end:
        if os.path.exists(p):
            return True
        time.sleep(0.05)
    return False


def run_scenario(app_args, *, layer=True, overlay="off", capture=True, pm_args=(), qpc=True, csv_name="out.csv",
                 env_extra=None, mangohud=False, filter_pid=True, timeout=60, truth_log=True, wdir=None,
                 pm_terminate_on_exit=True):
    """Run vk_truth with app_args (list) and, when capture, a presentmon capture attached to its pid.

    The capture is started right after the app so it reads the ring from the first record (presentmon reads a new
    segment from its first record).  Returns a Run.  A vk_truth failure to set up the scenario (Vulkan error exit
    code 2 with a 'not supported'-style message) sets run.skip instead of raising."""
    run = Run()
    run.dir = wdir or workdir()
    run.truth_path = os.path.join(run.dir, "truth.csv")
    run.csv_path = os.path.join(run.dir, csv_name)
    cmd = [VK_TRUTH] + list(app_args)
    if truth_log:
        cmd += ["--log", run.truth_path]
    run.cmd = cmd
    env = child_env(layer=layer, overlay=overlay, extra=env_extra, mangohud=mangohud)
    with desktop_lock():
        ensure_awake()
        t0 = time.time()
        out_f = open(os.path.join(run.dir, "app.out"), "w+")
        err_f = open(os.path.join(run.dir, "app.err"), "w+")
        app = spawn(cmd, env, out_f, err_f)
        run.pid = app.pid
        pm = None
        pm_err_f = None
        if capture:
            pm_cmd = [PRESENTMON]
            if filter_pid:
                pm_cmd += ["--process_id", str(app.pid)]
            pm_cmd += ["--output_file", run.csv_path, "--no_console_stats"]
            if pm_terminate_on_exit:
                pm_cmd += ["--terminate_on_proc_exit"]
            if qpc:
                pm_cmd += ["--qpc_time"]
            pm_cmd += list(pm_args)
            pm_err_f = open(os.path.join(run.dir, "pm.err"), "w+")
            env_pm = dict(os.environ)
            env_pm.pop("PRESENTMON_LAYER", None)
            pm = spawn(pm_cmd, env_pm, subprocess.DEVNULL, pm_err_f)
            run.pm_cmd = pm_cmd
        try:
            app.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            _kill_group(app)
            app.wait()
            run.exit_code = -9
            run.stderr += "TIMEOUT: vk_truth killed after %d s\n" % timeout
        else:
            run.exit_code = app.returncode
        run.wall = time.time() - t0
        if pm is not None:
            try:
                pm.wait(timeout=15)
            except subprocess.TimeoutExpired:
                _kill_group(pm, signal.SIGTERM)
                try:
                    pm.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    _kill_group(pm)
                    pm.wait()
                run.pm_stderr += "presentmon did not exit within 15 s of the app; terminated\n"
            run.pm_exit_code = pm.returncode
            run.pm_wall = time.time() - t0
    for f, attr in ((out_f, "stdout"), (err_f, "stderr")):
        f.flush()
        f.seek(0)
        setattr(run, attr, getattr(run, attr) + f.read())
        f.close()
    if pm_err_f:
        pm_err_f.flush()
        pm_err_f.seek(0)
        run.pm_stderr += pm_err_f.read()
        pm_err_f.close()
    if capture and os.path.exists(run.csv_path):
        run.header, run.rows = read_csv(run.csv_path)
    if truth_log and os.path.exists(run.truth_path):
        run.truth = read_truth(run.truth_path)
    if displays_asleep():
        run.skip = "displays went to sleep (DPMS Off) during the run, frame timing is not meaningful"
        return run
    # A mode the surface cannot do is a skip, not a failure.
    if run.exit_code == 2 and any(s in run.stderr for s in ("not supported", "unsupported", "not offered", "does not support", "not available")):
        run.skip = run.stderr.strip().splitlines()[-1] if run.stderr.strip() else "unsupported"
    return run


def pgrep(name):
    r = subprocess.run(["pgrep", "-x", name], capture_output=True, text=True)
    return [int(x) for x in r.stdout.split()]


def run_pm(args, timeout=20, env=None):
    """Run a presentmon subcommand (list / overlay ...) and return (rc, stdout, stderr)."""
    e = dict(os.environ)
    e.pop("PRESENTMON_LAYER", None)
    if env:
        e.update(env)
    r = subprocess.run([PRESENTMON] + list(args), capture_output=True, text=True, timeout=timeout, env=e)
    return r.returncode, r.stdout, r.stderr


class LiveCase(unittest.TestCase):
    """Base class: skips everything without a Wayland session, kills strays after every test."""

    @classmethod
    def setUpClass(cls):
        require_live(cls)

    def tearDown(self):
        for p in list(_children):
            if p.poll() is None:
                _kill_group(p)
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass

    def need(self, run):
        """Skip the test when the scenario itself could not be set up."""
        if run.skip:
            self.skipTest(run.skip)

    def assertRun(self, run, expect_exit=0):
        self.assertEqual(run.exit_code, expect_exit, "vk_truth exit code\n" + run.describe())
        if run.pm_exit_code is not None:
            self.assertEqual(run.pm_exit_code, 0, "presentmon exit code\n" + run.describe())


def show(label, **kv):
    """Print measured numbers so ctest --output-on-failure / -V shows them."""
    parts = []
    for k, v in kv.items():
        parts.append("%s=%s" % (k, ("%.4g" % v) if isinstance(v, float) else v))
    print("[measure] %s: %s" % (label, " ".join(parts)), flush=True)


# ---------------------------------------------------------------------------------------
# Common comparisons
# ---------------------------------------------------------------------------------------

def truth_compare(run):
    """Per-frame comparison of the default-column CSV with the vk_truth log (capture needs --qpc_time).
    Returns a dict of numbers; the tests assert on it and print it."""
    m = run.match()
    t = run.truth
    d = {"rows": len(run.rows), "truth": len(t), "matched": len(m)}
    idx = [i for _, i, _ in m]
    d["first_idx"] = idx[0] if idx else -1
    d["last_idx"] = idx[-1] if idx else -1
    d["contiguous"] = bool(idx) and idx == list(range(idx[0], idx[0] + len(idx)))
    d["dt_ms_med"] = med([x[2] for x in m])
    bp, ip = [], []
    for r, i, _ in m:
        v = num(r.get("MsBetweenPresents"))
        if v is not None and i > 0:
            bp.append(abs(v - (t[i]["present_pre_ns"] - t[i - 1]["present_pre_ns"]) / 1e6))
        v = num(r.get("MsInPresentAPI"))
        if v is not None:
            ip.append(v - (t[i]["present_post_ns"] - t[i]["present_pre_ns"]) / 1e6)
    d["bp_absdiff_med"] = med(bp)
    d["bp_absdiff_p95"] = pct(bp, 95)
    d["inpresent_diff_med"] = med(ip)
    d["inpresent_absdiff_p95"] = pct([abs(x) for x in ip], 95)
    return d


def multiple_fraction(values, period, tol):
    """Fraction of values within tol ms of a positive integer multiple of period."""
    if not values:
        return float("nan")
    ok = 0
    for v in values:
        k = max(1, round(v / period))
        if abs(v - k * period) <= tol:
            ok += 1
    return ok / len(values)


def snap_refresh(period_ms):
    """Round a measured period to the nearest known refresh (240/165/144/120/100/75/60 Hz) when within 1%, else keep."""
    for hz in (240, 165, 144, 120, 100, 75, 60, 30):
        p = 1000.0 / hz
        if abs(period_ms - p) / p < 0.01:
            return p
    return period_ms


def fifo_period_ms(run):
    """Refresh interval of the window's output, read from a FIFO run.  The mean display change is used, not the
    median: windowed surfaces can alternate around the refresh interval (see block_multiple_fraction)."""
    v = trim(col(run.rows, "MsBetweenDisplayChange"), 0.1)
    if not v:
        v = trim(col(run.rows, "MsBetweenPresents"), 0.1)
    return snap_refresh(sum(v) / len(v))


def list_displays():
    """vk_truth --list-displays -> list of dicts {index, name, width, height, hz}."""
    import re
    try:
        r = subprocess.run([VK_TRUTH, "--list-displays"], capture_output=True, text=True, timeout=20,
                           env=child_env(layer=False))
    except (subprocess.TimeoutExpired, OSError):
        return []
    out = []
    for line in r.stdout.splitlines():
        m = re.search(r'\[(\d+)\] id=\d+ name="([^"]*)".*mode=(\d+)x(\d+)@([\d.]+)Hz', line)
        if m:
            out.append({"index": int(m.group(1)), "name": m.group(2), "width": int(m.group(3)),
                        "height": int(m.group(4)), "hz": float(m.group(5))})
    return out


def window_median(intervals, k=16):
    """Median of k-frame average intervals: a frame interval that is not dominated by per-frame phase jitter."""
    if len(intervals) < k:
        return med(intervals)
    sums = [sum(intervals[i:i + k]) / k for i in range(0, len(intervals) - k + 1)]
    return statistics.median(sums)


def block_multiple_fraction(values, period, k=8, tol=0.4):
    """Like multiple_fraction, but for sums of k consecutive values.  A windowed surface on KWin sometimes shows
    display changes alternating around the refresh interval (e.g. 3.84 / 4.50 ms at 240 Hz) although the average
    is exact; sums over a few frames are insensitive to that."""
    blocks = [sum(values[i:i + k]) for i in range(0, len(values) - k + 1, k)]
    return multiple_fraction(blocks, period, tol)


# ---------------------------------------------------------------------------------------
# VRR: the output's real VRR state (DRM, read-only) and the display changes checked against it
# ---------------------------------------------------------------------------------------
#
# KWin (policy Automatic) runs an output in VRR only while the *active* window is fullscreen on it, decided again
# for every frame (KWin 6.8 src/compositor.cpp).  A click on another window, or a window rule that blocks focus,
# turns VRR off, also in the middle of a run.  So a VRR test cannot assume one state per run: it reads the CRTC's
# VRR_ENABLED property while the run goes on and judges each frame against the state at its display time, except
# frames close to a change (see vrr_state_at).
# Tolerances come from the 16 runs at 150 fps with a logged VRR_ENABLED (KWin 6.8.2 / NVIDIA 615.78, 2026-11-08):
# in each run with VRR-on frames, at least 84% of them followed the present interval within 0.6 ms (93% pooled); in
# each run with VRR-off frames, at least 90% sat within 0.6 ms of a refresh multiple (99.6% pooled).  0.6 ms
# because a composited window (another window on top) gets display changes up to ~0.45 ms off a refresh multiple
# on that driver (typically 0.2-0.3 ms; one displaced display time shifts two adjacent intervals).

VRR_TOL_MS = 0.6
VRR_ON_SETTLE_NS = 120_000_000    # after VRR_ENABLED turns on, the display follows the frame rate within 45-80 ms
VRR_EDGE_GUARD_NS = 40_000_000    # frames this close to a change are not judged (poll and commit timing)
_DRM_MODE_OBJECT_CRTC = 0xCCCCCCCC


def _edid_monitor_name(edid):
    """Monitor name from an EDID's display descriptors (tag 0xFC), or ''."""
    for off in (54, 72, 90, 108):
        d = edid[off:off + 18]
        if len(d) == 18 and d[0] == 0 and d[1] == 0 and d[3] == 0xFC:
            return d[5:18].split(b"\n")[0].decode("ascii", "replace").strip()
    return ""


def drm_connector_for_display(display_name):
    """((card device path, connector id, connector name), None) for the one connected DRM connector whose EDID
    monitor name is part of the SDL display name, else (None, reason)."""
    matches = []
    for d in sorted(glob.glob("/sys/class/drm/card*-*")):
        try:
            with open(d + "/status") as f:
                if f.read().strip() != "connected":
                    continue
            with open(d + "/edid", "rb") as f:
                name = _edid_monitor_name(f.read())
            with open(d + "/connector_id") as f:
                conn = int(f.read())
        except (OSError, ValueError):
            continue
        if name and name in display_name:
            base = os.path.basename(d)
            matches.append(("/dev/dri/" + base.split("-")[0], conn, base.split("-", 1)[1]))
    if len(matches) != 1:
        return None, "no unique DRM connector for display %r (%d matches)" % (display_name, len(matches))
    return matches[0], None


class _ObjProps(ctypes.Structure):
    _fields_ = [("count_props", ctypes.c_uint32), ("props", ctypes.POINTER(ctypes.c_uint32)),
                ("prop_values", ctypes.POINTER(ctypes.c_uint64))]


class _Prop(ctypes.Structure):
    _fields_ = [("prop_id", ctypes.c_uint32), ("flags", ctypes.c_uint32), ("name", ctypes.c_char * 32)]


class _ConnHead(ctypes.Structure):      # first fields of drmModeConnector
    _fields_ = [("connector_id", ctypes.c_uint32), ("encoder_id", ctypes.c_uint32)]


class _Encoder(ctypes.Structure):
    _fields_ = [("encoder_id", ctypes.c_uint32), ("encoder_type", ctypes.c_uint32), ("crtc_id", ctypes.c_uint32),
                ("possible_crtcs", ctypes.c_uint32), ("possible_clones", ctypes.c_uint32)]


class VrrWatch:
    """Context manager: polls the VRR_ENABLED property of the CRTC that drives a connector every 5 ms while the block
    runs (libdrm, read-only, no DRM master needed).  .available is False with a .reason when the state cannot be
    read; .steps is [(CLOCK_MONOTONIC ns, value)], the value at the start first, then one entry per change."""

    def __init__(self, target, reason=None, interval_s=0.005):
        self.available = False
        self.reason = reason or ""
        self.steps = []
        self.crtc = 0
        self._interval = interval_s
        self._stop = threading.Event()
        self._thread = None
        self._fd = -1
        if target is None:
            self.reason = self.reason or "no connector"
            return
        card, conn, _name = target
        try:
            self._drm = drm = ctypes.CDLL("libdrm.so.2")
            drm.drmModeGetConnectorCurrent.restype = ctypes.POINTER(_ConnHead)
            drm.drmModeGetConnectorCurrent.argtypes = [ctypes.c_int, ctypes.c_uint32]
            drm.drmModeFreeConnector.argtypes = [ctypes.c_void_p]
            drm.drmModeGetEncoder.restype = ctypes.POINTER(_Encoder)
            drm.drmModeGetEncoder.argtypes = [ctypes.c_int, ctypes.c_uint32]
            drm.drmModeFreeEncoder.argtypes = [ctypes.c_void_p]
            drm.drmModeObjectGetProperties.restype = ctypes.POINTER(_ObjProps)
            drm.drmModeObjectGetProperties.argtypes = [ctypes.c_int, ctypes.c_uint32, ctypes.c_uint32]
            drm.drmModeFreeObjectProperties.argtypes = [ctypes.c_void_p]
            drm.drmModeGetProperty.restype = ctypes.POINTER(_Prop)
            drm.drmModeGetProperty.argtypes = [ctypes.c_int, ctypes.c_uint32]
            drm.drmModeFreeProperty.argtypes = [ctypes.c_void_p]
            self._fd = os.open(card, os.O_RDWR | os.O_CLOEXEC)
            c = drm.drmModeGetConnectorCurrent(self._fd, conn)
            enc_id = c.contents.encoder_id if c else 0
            if c:
                drm.drmModeFreeConnector(c)
            e = drm.drmModeGetEncoder(self._fd, enc_id) if enc_id else None
            self.crtc = e.contents.crtc_id if e else 0
            if e:
                drm.drmModeFreeEncoder(e)
            if not self.crtc:
                self.reason = "connector %d has no CRTC" % conn
                return
            self._prop = 0
            p = drm.drmModeObjectGetProperties(self._fd, self.crtc, _DRM_MODE_OBJECT_CRTC)
            if p:
                for i in range(p.contents.count_props):
                    q = drm.drmModeGetProperty(self._fd, p.contents.props[i])
                    if q and q.contents.name == b"VRR_ENABLED":
                        self._prop = q.contents.prop_id
                    if q:
                        drm.drmModeFreeProperty(q)
                drm.drmModeFreeObjectProperties(p)
            if not self._prop or self._read() is None:
                self.reason = "CRTC %d has no readable VRR_ENABLED" % self.crtc
                return
            self.available = True
        except (OSError, AttributeError) as ex:
            self.reason = "DRM not readable: %s" % ex

    def _read(self):
        p = self._drm.drmModeObjectGetProperties(self._fd, self.crtc, _DRM_MODE_OBJECT_CRTC)
        if not p:
            return None
        try:
            for i in range(p.contents.count_props):
                if p.contents.props[i] == self._prop:
                    return int(p.contents.prop_values[i])
        finally:
            self._drm.drmModeFreeObjectProperties(p)
        return None

    def _poll(self):
        last = None
        while True:
            v = self._read()
            if v is not None and v != last:
                self.steps.append((time.monotonic_ns(), v))
                last = v
            if self._stop.wait(self._interval):
                break

    def __enter__(self):
        if self.available:
            self._thread = threading.Thread(target=self._poll, daemon=True)
            self._thread.start()
        return self

    def __exit__(self, *exc):
        if self._thread:
            self._stop.set()
            self._thread.join(timeout=2)
        if self._fd >= 0:
            os.close(self._fd)
            self._fd = -1
        return False


def vrr_state_at(steps, t_ns):
    """VRR_ENABLED at time t from VrrWatch.steps, or None when t is before the first reading, within
    VRR_EDGE_GUARD_NS of a change, or less than VRR_ON_SETTLE_NS after VRR turned on."""
    ts = [s[0] for s in steps]
    i = bisect.bisect_right(ts, t_ns) - 1
    if i < 0:
        return None
    for j in (i, i + 1):
        if 1 <= j < len(steps):
            when, value = steps[j]
            if abs(t_ns - when) < VRR_EDGE_GUARD_NS:
                return None
            if value == 1 and 0 <= t_ns - when < VRR_ON_SETTLE_NS:
                return None
    return steps[i][1]


def display_frames(rows):
    """[(display time ns, MsBetweenDisplayChange, MsBetweenPresents)] for displayed frames of a --qpc_time CSV."""
    out = []
    for r in rows:
        start, until = num(r.get("TimeInQPC")), num(r.get("MsUntilDisplayed"))
        bdc, bp = num(r.get("MsBetweenDisplayChange")), num(r.get("MsBetweenPresents"))
        if None not in (start, until, bdc, bp):
            out.append((int(start + until * 1e6), bdc, bp))
    return out


def _follows(bdc, bp):
    return abs(bdc - bp) <= VRR_TOL_MS


def _on_multiple(bdc, refresh_ms):
    k = max(1, round(bdc / refresh_ms))
    return abs(bdc - k * refresh_ms) <= VRR_TOL_MS


def vrr_check(frames, steps, refresh_ms):
    """Judge frames against the DRM VRR state.  Returns a dict: on/off = frames judged in each state,
    follow = share of VRR-on frames whose display change follows the present interval, multiple = share of
    VRR-off frames on a refresh multiple (None when fewer than 20 frames were judged in that state)."""
    on = [f for f in frames if vrr_state_at(steps, f[0]) == 1]
    off = [f for f in frames if vrr_state_at(steps, f[0]) == 0]
    return {"on": len(on), "off": len(off),
            "follow": sum(_follows(b, p) for (_, b, p) in on) / len(on) if len(on) >= 20 else None,
            "multiple": sum(_on_multiple(b, refresh_ms) for (_, b, _) in off) / len(off) if len(off) >= 20 else None}


def vrr_pattern(frames, refresh_ms, bin_ns=250_000_000):
    """Classify a run from its display changes alone, per 0.25 s: 'V' = at least 70% of the frames follow the
    present interval (and are not refresh multiples), 'f' = at least 70% on refresh multiples, '?' = neither."""
    if not frames:
        return ""
    out = []
    start, end = frames[0][0], frames[-1][0]
    b = start
    while b <= end:
        xs = [(bdc, bp) for (t, bdc, bp) in frames if b <= t < b + bin_ns]
        b += bin_ns
        if not xs:
            continue
        v = sum(_follows(bdc, bp) and not _on_multiple(bdc, refresh_ms) for (bdc, bp) in xs) / len(xs)
        m = sum(_on_multiple(bdc, refresh_ms) for (bdc, _) in xs) / len(xs)
        out.append("V" if v >= 0.7 else ("f" if m >= 0.7 else "?"))
    return "".join(out)


def describe_steps(steps, t0_ns):
    """'off@-0.68s,on@+0.01s,...': VRR_ENABLED readings relative to t0 (the first is the value at the start)."""
    return ",".join("%s@%+.2fs" % ("on" if v else "off", (t - t0_ns) / 1e9) for (t, v) in steps) or "none"
