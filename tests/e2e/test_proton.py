# SPDX-License-Identifier: MIT
"""A real D3D11 game loop under Proton + DXVK with the build-tree layer, compared with the game thread's own clock.

tests/apps/d3d11_truth.cpp (built with mingw-w64 as <build>/bin/d3d11_truth.exe) logs QueryPerformanceCounter around
every IDXGISwapChain::Present call.  DXVK calls vkQueuePresentKHR on its own thread ("dxvk-submit"), so this is the
only test where the layer sees a different thread than the game.  It pins down (numbers: docs/ARCHITECTURE.md,
"Under DXVK"):

  - frame times and display times match the game thread's Present() calls;
  - DXVK's own use of VK_EXT_present_timing (GetFrameStatistics) keeps working next to the layer's;
  - DXVK 2.x, which uses VK_KHR_present_id (v1), gets display times too;
  - Xwayland: no display times (NA) and no stall;
  - the overlay draws on DXVK's swapchain;
  - the documented limitation: MsInPresentAPI / CPUBusy / CPUWait describe DXVK's presenter thread.

Needs a Proton build with DXVK: PML_PROTON=<directory with the 'proton' script>, default the newest GE-Proton in
~/.local/share/Steam/compatibilitytools.d.  DXVK 2.x: PML_DXVK2_DIR=<directory with x86_64 d3d11.dll + dxgi.dll>,
default the DXVK of a Proton 10 build in the same directory (loaded app-locally next to a copy of the exe, so the
prefix is shared).  PML_D3D11_TRUTH overrides <build>/bin/d3d11_truth.exe.  The prefix is kept in
<build>/e2e-proton/ and created on first use (about 10 s).  Wine processes of that prefix are stopped with its
own wineserver -k; nothing else is touched.
"""
import glob
import os
import re
import shutil
import signal
import struct
import subprocess
import time
import unittest

import pmltest as pt

COMPAT_DIR = os.path.expanduser("~/.local/share/Steam/compatibilitytools.d")
EXE = os.environ.get("PML_D3D11_TRUTH") or os.path.join(pt.BIN, "d3d11_truth.exe")
EXE_NAME = "d3d11_truth.exe"
WORK = os.path.join(pt.BUILD, "e2e-proton")


# ---------------------------------------------------------------------------------------
# Finding Proton / DXVK
# ---------------------------------------------------------------------------------------

def dxvk_version(proton_dir):
    try:
        with open(os.path.join(proton_dir, "files", "lib", "wine", "dxvk", "version")) as f:
            m = re.search(r"\((v[0-9][^)]*)\)", f.read())
            return m.group(1) if m else None
    except OSError:
        return None


def _version_key(path):
    return [int(x) for x in re.findall(r"\d+", os.path.basename(path))]


def find_proton():
    p = os.environ.get("PML_PROTON")
    if p:
        return p if os.path.isfile(os.path.join(p, "proton")) else None
    for c in sorted(glob.glob(os.path.join(COMPAT_DIR, "GE-Proton*")), key=_version_key, reverse=True):
        v = dxvk_version(c)
        if os.path.isfile(os.path.join(c, "proton")) and v and v.startswith("v3"):
            return c
    return None


def find_dxvk2():
    """(directory with d3d11.dll + dxgi.dll, version) of a DXVK 2.x, or (None, None)."""
    d = os.environ.get("PML_DXVK2_DIR")
    if d:
        ok = all(os.path.isfile(os.path.join(d, n)) for n in ("d3d11.dll", "dxgi.dll"))
        return (d, "PML_DXVK2_DIR") if ok else (None, None)
    for c in sorted(glob.glob(os.path.join(COMPAT_DIR, "*")), key=_version_key, reverse=True):
        v = dxvk_version(c)
        x64 = os.path.join(c, "files", "lib", "wine", "dxvk", "x86_64-windows")
        if v and v.startswith("v2") and all(os.path.isfile(os.path.join(x64, n)) for n in ("d3d11.dll", "dxgi.dll")):
            return x64, v
    return None, None


PROTON = find_proton()
DXVK2_DIR, DXVK2_VERSION = find_dxvk2()


def skip_reason():
    if not PROTON:
        return "no Proton with DXVK 3.x found (set PML_PROTON)"
    if not os.path.isfile(EXE):
        return "missing %s (needs x86_64-w64-mingw32-g++ when configuring, or set PML_D3D11_TRUTH)" % EXE
    return None


def winpath(p):
    return "Z:" + p.replace("/", "\\")


def mono_minus_raw_ns():
    """CLOCK_MONOTONIC - CLOCK_MONOTONIC_RAW, from the tightest of 20 bracketed reads (Wine's QPC is MONOTONIC_RAW)."""
    best = None
    for _ in range(20):
        r1 = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
        m = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        r2 = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
        if best is None or r2 - r1 < best[0]:
            best = (r2 - r1, m - (r1 + r2) // 2, (r1 + r2) // 2)
    return best[1], best[2]


# ---------------------------------------------------------------------------------------
# One run
# ---------------------------------------------------------------------------------------

class ProtonRun:
    def __init__(self, name):
        self.name = name
        self.dir = ""
        self.pid = None
        self.launcher_rc = None
        self.meta = {}
        self.truth = []          # dicts with *_ns on CLOCK_MONOTONIC
        self.v1 = []
        self.v2 = []
        self.ring = []
        self.threads = {}        # tid -> name
        self.window = ""
        self.notes = []

    def describe(self):
        tail = ""
        for n in ("launcher.err", "pm_v1.err", "pm_v2.err"):
            p = os.path.join(self.dir, n)
            if os.path.exists(p):
                with open(p, errors="replace") as f:
                    tail += "--- %s ---\n%s\n" % (n, f.read()[-2000:])
        return "run %s in %s pid=%s launcher_rc=%s truth=%d v1=%d v2=%d notes=%s\n%s" % (
            self.name, self.dir, self.pid, self.launcher_rc, len(self.truth), len(self.v1), len(self.v2), self.notes, tail)


def _read_truth(path):
    meta, body = {}, []
    with open(path, encoding="utf-8", errors="replace") as f:
        for ln in f.read().splitlines():
            if ln.startswith("#"):
                for tok in ln[1:].split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        meta.setdefault(k, v)
            elif ln.strip():
                body.append(ln)
    import csv
    return meta, list(csv.DictReader(body))


def _find_app_pid(mark, launcher, timeout, exe_name=EXE_NAME):
    end = time.time() + timeout
    while time.time() < end:
        for s in glob.glob("/dev/shm/presentmon-linux.*"):
            try:
                pid = int(s.rsplit(".", 1)[1])
                with open("/proc/%d/cmdline" % pid, "rb") as f:
                    if exe_name.encode() not in f.read():
                        continue
                with open("/proc/%d/environ" % pid, "rb") as f:
                    if ("PML_E2E_MARK=" + mark).encode() in f.read().split(b"\0"):
                        return pid
            except (ValueError, OSError):
                continue
        if launcher.poll() is not None:
            return None
        time.sleep(0.05)
    return None


def _kwin_window(pid, tag):
    """Class and output of the app's window, read-only through a KWin script (empty when KWin scripting fails)."""
    if not shutil.which("kdotool"):
        return ""
    js = ('var out=[];var ws=workspace.windowList();for(var i=0;i<ws.length;i++){var w=ws[i];'
          'if(w.pid==%d){out.push("class="+w.resourceClass+" output="+(w.output?w.output.name:"?")+" active="+w.active);}}'
          'print("%s "+out.join(" | "));') % (pid, tag)
    path = os.path.join(WORK, tag + ".js")
    with open(path, "w") as f:
        f.write(js)
    since = time.time() - 1
    subprocess.run(["kdotool", "kwinscript", "--file", path], capture_output=True, timeout=10)
    time.sleep(0.4)
    r = subprocess.run(["journalctl", "--user", "--since", "@%d" % int(since), "--no-pager", "-o", "cat"],
                       capture_output=True, text=True, timeout=10)
    lines = [ln for ln in r.stdout.splitlines() if ln.startswith(tag + " ")]
    return lines[-1][len(tag) + 1:] if lines else ""


def run_proton(name, app_args, *, display="wayland", dxvk2=False, overlay="off", env_extra=None, timeout=120, exe=None):
    run = ProtonRun(name)
    os.makedirs(WORK, exist_ok=True)
    run.dir = pt.workdir("pmlproton-%s-" % name)
    pfx = os.path.join(WORK, "pfx-" + os.path.basename(PROTON))
    os.makedirs(pfx, exist_ok=True)   # Proton expects the compat data directory to exist (Steam creates it)
    exe = exe or EXE
    exe_name = os.path.basename(exe)
    if dxvk2:
        d = os.path.join(WORK, "dxvk2")
        os.makedirs(d, exist_ok=True)
        for src, n in ((EXE, EXE_NAME), (os.path.join(DXVK2_DIR, "d3d11.dll"), "d3d11.dll"),
                       (os.path.join(DXVK2_DIR, "dxgi.dll"), "dxgi.dll")):
            dst = os.path.join(d, n)
            if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src):
                shutil.copyfile(src, dst)
        exe = os.path.join(d, EXE_NAME)
    truth_path = os.path.join(run.dir, "truth.csv")
    mark = "%s-%d-%d" % (name, os.getpid(), time.monotonic_ns())

    env = pt.child_env(layer=True, overlay=overlay, extra=env_extra)
    for k in ("SteamAppId", "SteamGameId", "STEAM_COMPAT_APP_ID", "WINEPREFIX", "WINEDLLOVERRIDES", "DXVK_HUD"):
        env.pop(k, None)
    env.update({
        "STEAM_COMPAT_DATA_PATH": pfx,
        "STEAM_COMPAT_CLIENT_INSTALL_PATH": os.path.expanduser("~/.local/share/Steam"),
        "PROTONFIXES_DISABLE": "1",
        "PROTON_LOG": "1",
        "PROTON_LOG_DIR": run.dir,
        "DXVK_LOG_LEVEL": "info",
        "DXVK_LOG_PATH": winpath(run.dir),
        "PRESENTMON_LOG": "info",
        "PML_E2E_MARK": mark,
    })
    if dxvk2:
        env["WINEDLLOVERRIDES"] = "d3d11,dxgi=n"   # native first; the application directory precedes system32
    if display == "wayland":
        env.pop("DISPLAY", None)
        env["PROTON_ENABLE_WAYLAND"] = "1"
    else:
        env.pop("WAYLAND_DISPLAY", None)
    if env_extra:
        env.update(env_extra)
    cmd = [os.path.join(PROTON, "proton"), "run", exe, "--log", winpath(truth_path)] + list(app_args)

    off0, raw0 = mono_minus_raw_ns()
    with pt.desktop_lock():
        pt.ensure_awake()
        # The captures start before the game, by name: a capture that attaches to an existing segment reads it from
        # "now", one that sees the segment appear reads it from the first record.  The app's pid (for ring_dump and
        # the thread names) is only known once its segment exists.
        env_pm = dict(os.environ)
        env_pm.pop("PRESENTMON_LAYER", None)
        pms, ring, files = [], None, []
        for metrics in ("v1", "v2"):
            pm_cmd = [pt.PRESENTMON, "--process_name", exe_name, "--output_file",
                      os.path.join(run.dir, "pm_%s.csv" % metrics), "--%s_metrics" % metrics, "--qpc_time",
                      "--terminate_on_proc_exit", "--no_console_stats"]
            files.append(open(os.path.join(run.dir, "pm_%s.err" % metrics), "w"))
            pms.append(pt.spawn(pm_cmd, env_pm, subprocess.DEVNULL, files[-1]))
        time.sleep(0.3)
        files.append(open(os.path.join(run.dir, "launcher.err"), "w"))
        launcher = pt.spawn(cmd, env, subprocess.DEVNULL, files[-1], cwd=run.dir)
        try:
            run.pid = _find_app_pid(mark, launcher, timeout=60, exe_name=exe_name)
            if run.pid:
                files.append(open(os.path.join(run.dir, "ring.csv"), "w"))
                ring = pt.spawn([pt.RING_DUMP, "--pid", str(run.pid), "--oldest", "--seconds", str(timeout)], env_pm,
                                files[-1], subprocess.DEVNULL)
                time.sleep(2.0)
                for t in glob.glob("/proc/%d/task/*" % run.pid):
                    try:
                        with open(t + "/comm") as f:
                            run.threads[os.path.basename(t)] = f.read().strip()
                    except OSError:
                        pass
                run.window = _kwin_window(run.pid, "PMLPROTON:" + mark)
            else:
                run.notes.append("no layer segment for %s appeared" % exe_name)
            try:
                run.launcher_rc = launcher.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                run.notes.append("launcher timed out")
                pt._kill_group(launcher)
                launcher.wait()
            for p in pms:
                try:
                    p.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    pt._kill_group(p, signal.SIGINT)
                    try:
                        p.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        pt._kill_group(p)
                        p.wait()
                    run.notes.append("presentmon did not exit with the app")
            if ring is not None and ring.poll() is None:
                pt._kill_group(ring, signal.SIGTERM)
                ring.wait()
        finally:
            # Stop what is left of this prefix (and only this prefix) with its own wineserver; never let the cleanup
            # replace the real error (a Proton without files/bin, a wineserver that hangs).
            wenv = dict(os.environ, WINEPREFIX=os.path.join(pfx, "pfx"))
            for ws in (os.path.join(PROTON, "files", "bin", "wineserver"), os.path.join(PROTON, "dist", "bin", "wineserver")):
                if os.path.exists(ws):
                    for arg in ("-k", "-w"):
                        try:
                            subprocess.run([ws, arg], env=wenv, capture_output=True, timeout=20)
                        except (OSError, subprocess.SubprocessError) as e:
                            run.notes.append("wineserver %s: %s" % (arg, e))
                    break
            for f in files:
                f.close()
    off1, raw1 = mono_minus_raw_ns()

    if os.path.exists(truth_path):
        run.meta, rows = _read_truth(truth_path)
        qpf = int(run.meta.get("qpf", "10000000"))
        unit = 1_000_000_000 // qpf

        def mono(q):
            raw = int(q) * unit
            f = (raw - raw0) / (raw1 - raw0) if raw1 != raw0 else 0.0
            return raw + off0 + int((off1 - off0) * min(max(f, 0.0), 1.0))
        for r in rows:
            run.truth.append({"begin": mono(r["present_begin_qpc"]), "end": mono(r["present_end_qpc"]),
                              "fs_hr": r["fs_hr"], "hr": r.get("hr") or "0x00000000"})
    for metrics in ("v1", "v2"):
        p = os.path.join(run.dir, "pm_%s.csv" % metrics)
        if os.path.exists(p):
            setattr(run, metrics, pt.read_csv(p)[1])
    p = os.path.join(run.dir, "ring.csv")
    if os.path.exists(p):
        run.ring = pt.read_csv(p)[1]
    return run


# ---------------------------------------------------------------------------------------
# Comparisons
# ---------------------------------------------------------------------------------------

class Aligned:
    """The layer's records paired with the game's Present() calls.

    DXVK turns every Present() call into exactly one vkQueuePresentKHR, in order, so the k-th present record (all
    swapchains, by present start) belongs to the k-th Present() call.  Matching by time instead is ambiguous:
    DXVK's presenter thread presents anywhere from 0.03 ms to a whole refresh interval after the game's call,
    depending on how many frames DXVK's own queue holds.  `lag[k]` is that delay (ms); v1 rows are joined to
    their record by their present start (QPCTime == present_start_ns)."""

    def __init__(self, run):
        self.ring = sorted(run.ring, key=lambda r: int(r["present_start_ns"]))
        self.n = min(len(self.ring), len(run.truth))
        self.start = [int(r["present_start_ns"]) for r in self.ring]
        self.end = [int(r["present_end_ns"]) for r in self.ring]
        self.screen = [int(r["screen_ns"]) for r in self.ring]
        self.lag = [(self.start[k] - run.truth[k]["begin"]) / 1e6 for k in range(self.n)]
        pos = {s: k for k, s in enumerate(self.start)}
        self.v1 = []   # (row, record index or None)
        for r in run.v1:
            t = pt.num(r.get("QPCTime"))
            self.v1.append((r, pos.get(int(t)) if t is not None else None))

    def steady_lag(self):
        return self.lag[5:] if len(self.lag) > 10 else self.lag   # the first frames include start-up


def truth_intervals(run):
    t = run.truth
    return [(t[i]["begin"] - t[i - 1]["begin"]) / 1e6 for i in range(1, len(t))]


def truth_in_present(run):
    return [(t["end"] - t["begin"]) / 1e6 for t in run.truth]


def read_raw(path):
    with open(path, "rb") as f:
        head = f.read(64)
        if head[:8] != b"PMLDUMP1":
            return None
        fmt, cs, w, h, row, bpp, frame = struct.unpack_from("<7I", head, 8)
        return fmt, w, h, row, bpp, f.read(h * row)


def check_gpu(case, run, label):
    """The layer's GPU columns on a translated game (DXVK / vkd3d-proton submit on one thread and present on
    another): GPU data on nearly every frame, GPU start before the frame's ready time, busy within it, and the
    CSV's GPUBusy from the same records."""
    ring = sorted(run.ring, key=lambda r: int(r["present_start_ns"]))[1:-4]
    with_gpu = [r for r in ring if int(r.get("gpu_start_ns") or 0)]
    busy = [int(r["gpu_busy_ns"]) / 1e6 for r in with_gpu]
    v2_busy = [pt.num(r.get("GPUBusy", r.get("MsGPUBusy"))) or 0 for r in run.v2]
    pt.show(label, frames=len(ring), with_gpu=len(with_gpu), gpu_busy_med=pt.med(busy), gpu_busy_max=max(busy or [0]),
            v2_gpu_busy_med=pt.med(v2_busy))
    case.assertGreaterEqual(len(with_gpu), len(ring) - 5, "frames without GPU data\n" + run.describe())
    for r in with_gpu:
        ready = int(r["ready_ns"])
        start = int(r["gpu_start_ns"])
        case.assertLessEqual(start, ready, "GPU start after the frame's ready time: %s" % r)
        case.assertLessEqual(int(r["gpu_busy_ns"]), ready - start)
    case.assertGreater(sum(1 for b in v2_busy if b > 0), len(run.v2) - 6, "v2 rows without GPUBusy")


# ---------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------

class Proton(pt.LiveCase):
    runs = {}

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        why = skip_reason()
        if why:
            raise unittest.SkipTest(why)
        print("[proton] %s (DXVK %s); DXVK 2.x: %s (%s)" % (PROTON, dxvk_version(PROTON), DXVK2_DIR, DXVK2_VERSION),
              flush=True)

    SCENARIOS = {
        "fifo": dict(app_args=["--seconds", "8", "--sync", "1", "--framestats"]),
        "cpu6": dict(app_args=["--seconds", "6", "--sync", "1", "--cpu-ms", "6"]),
        "cap100": dict(app_args=["--seconds", "6", "--sync", "0", "--fps", "100"]),
        "dxvk2": dict(app_args=["--seconds", "6", "--sync", "1", "--framestats"], dxvk2=True),
        "x11": dict(app_args=["--seconds", "5", "--sync", "1"], display="x11"),
        "overlay": dict(app_args=["--seconds", "5", "--sync", "1", "--colour", "0.8,0.1,0.1"], overlay="1"),
    }

    def scenario(self, key):
        if key not in self.runs:
            kw = dict(self.SCENARIOS[key])
            if key == "overlay":
                d = os.path.join(WORK, "dumps")
                os.makedirs(d, exist_ok=True)
                kw["env_extra"] = {"PRESENTMON_OVERLAY_DUMP": os.path.join(d, "overlay.png"),
                                   "PRESENTMON_OVERLAY_DUMP_FRAME": "240"}
                for ext in ("", ".raw"):
                    try:
                        os.unlink(os.path.join(d, "overlay.png" + ext))
                    except FileNotFoundError:
                        pass
            self.runs[key] = run_proton(key, kw.pop("app_args"), **kw)
        run = self.runs[key]
        if pt.displays_asleep():
            self.skipTest("displays went to sleep (DPMS Off) during the run")
        self.assertIsNotNone(run.pid, run.describe())
        self.assertGreater(len(run.truth), 100, run.describe())
        self.assertGreater(len(run.v1), 100, run.describe())
        return run

    def check_alignment(self, run, spread_max=2.0):
        """One present record per Present() call, in order, each v1 row one of those records; DXVK's presenter
        follows the game thread at a steady distance (spread of the lag, p95 - p05, below spread_max ms)."""
        a = Aligned(run)
        self.assertEqual(len(a.ring), len(run.truth), "present records vs Present() calls\n" + run.describe())
        self.assertTrue(all(k is not None for _, k in a.v1), "v1 rows that are not one of the layer's records")
        # v1: one row per present, except each swapchain's first (it has no previous present). v2 also holds, per
        # swapchain, its last displayed present and every present after it (they wait for a later displayed one, as
        # upstream; at exit the last frames may never reach the screen).
        per_swapchain = {}
        for k, r in enumerate(a.ring):
            per_swapchain.setdefault(r["swapchain"], []).append(k)
        held = 0
        for ks in per_swapchain.values():
            shown = [k for k in ks if a.screen[k] > 0]
            if shown:
                held += sum(1 for k in ks if k >= shown[-1])
        self.assertEqual(len(run.v1), len(a.ring) - len(per_swapchain), run.describe())
        self.assertEqual(len(run.v2), len(a.ring) - len(per_swapchain) - held,
                         "v2 rows vs presents minus seeds and held rows\n" + run.describe())
        lag = a.steady_lag()
        interval = pt.med(truth_intervals(run))
        pt.show(run.name, presenter_lag_median=pt.med(lag), lag_p05=pt.pct(lag, 5), lag_p95=pt.pct(lag, 95),
                lag_min=min(lag), interval=interval)
        # Never before the game's call (a little slack for the clock mapping), never more than a frame or so after.
        self.assertGreater(min(lag), -0.5, "a present before its Present() call: frames out of order?")
        self.assertLess(pt.med(lag), 1.5 * interval + 1.0)
        self.assertLess(pt.pct(lag, 95) - pt.pct(lag, 5), spread_max, "DXVK's presenter does not follow the game steadily")
        return a

    def check_frames(self, run, a, per_frame=True):
        """msBetweenPresents is the presenter's present-start delta, row by row (the collector's arithmetic on the
        layer's records), and, where the presenter follows the game steadily (per_frame; not through Xwayland,
        which releases presents in bunches), the game's own Present() intervals frame by frame too.  The game
        comparison is reported in any case."""
        swapchain = [r["swapchain"] for r in a.ring]
        exact = rows = 0
        diffs = []
        for r, k in a.v1:
            if not k or swapchain[k] != swapchain[k - 1]:
                continue   # a swapchain's first present has no row; its second compares with its own first
            rows += 1
            bp = pt.num(r["msBetweenPresents"])
            if abs(bp - (a.start[k] - a.start[k - 1]) / 1e6) < 0.01:
                exact += 1
            if k < len(run.truth):
                diffs.append(bp - (run.truth[k]["begin"] - run.truth[k - 1]["begin"]) / 1e6)
        ti = pt.med(truth_intervals(run))
        bp_med = pt.med([pt.num(r["msBetweenPresents"]) for r in run.v1])
        ft = pt.med([pt.num(r["FrameTime"]) for r in run.v2])
        absd = [abs(x) for x in diffs]
        pt.show(run.name, truth_interval=ti, msBetweenPresents=bp_med, FrameTime=ft, frame_diff_median=pt.med(absd),
                frame_diff_p95=pt.pct(absd, 95))
        self.assertGreater(rows, len(run.v1) - 4)
        self.assertGreaterEqual(exact, rows - 2, "msBetweenPresents vs the records' present starts")
        if per_frame:
            self.assertLess(pt.med(absd), 0.5, "per-frame msBetweenPresents vs the game's Present() interval")
            self.assertAlmostEqual(bp_med, ti, delta=0.05)
            self.assertAlmostEqual(ft, ti, delta=0.05)

    def check_display(self, run, a):
        """Display times on every frame, and the CSV's msUntilDisplayed is the record's screen time."""
        shown = [(r, k) for r, k in a.v1 if (pt.num(r["msUntilDisplayed"]) or 0) > 0]
        self.assertGreaterEqual(len(shown), len(run.v1) - 3, "rows without a display time\n" + run.describe())
        self.assertLessEqual(sum(1 for r in run.v2 if r["DisplayLatency"] == "NA"), 3, run.describe())
        consistent = sum(1 for r, k in shown if abs(pt.num(r["msUntilDisplayed"]) - (a.screen[k] - a.start[k]) / 1e6) < 0.01)
        self.assertGreaterEqual(consistent, len(shown) - 2, "msUntilDisplayed vs the record's screen time")
        change = pt.med([pt.num(r["msBetweenDisplayChange"]) for r, _ in shown if pt.num(r["msBetweenDisplayChange"])])
        g2d = pt.med([(a.screen[k] - run.truth[k]["begin"]) / 1e6 for r, k in shown if k < len(run.truth)])
        pt.show(run.name, between_display_change=change, until_displayed=pt.med([pt.num(r["msUntilDisplayed"]) for r, _ in shown]),
                game_present_to_display=g2d)
        return change

    def check_latency_columns(self, run, a):
        """The latency columns are measured from DXVK's presenter thread, frame by frame: msUntilDisplayed[k] is
        screen[k] - present start[k] (check_display), and v2 DisplayLatency[k] is screen[k] - the presenter's CPU
        start, which is the end of present k-1 (each v2 row is joined to its record through CPUStartQPC).  What
        that means against the game thread is reported, not asserted (it depends on how DXVK paces, see
        docs/ARCHITECTURE.md, Risks): with L[k] the presenter's delay after the game's Present() call k,
        msUntilDisplayed is short by L, and DisplayLatency is long by the game's wait inside Present() minus L."""
        by_cpu_start = {a.end[k - 1]: k for k in range(1, len(a.ring))}
        joined = good = 0
        paired = []   # (k, DisplayLatency) of joined, displayed v2 rows
        for r in run.v2:
            if r["DisplayLatency"] == "NA":
                continue
            k = by_cpu_start.get(int(pt.num(r.get("CPUStartQPC")) or -1))
            if k is None or a.screen[k] == 0:
                continue
            joined += 1
            dl = pt.num(r["DisplayLatency"])
            if abs(dl - (a.screen[k] - a.end[k - 1]) / 1e6) < 0.01:
                good += 1
            if 5 <= k < a.n:
                paired.append((k, dl))
        displayed_v2 = sum(1 for r in run.v2 if r["DisplayLatency"] != "NA")
        self.assertGreaterEqual(joined, displayed_v2 - 3, "v2 rows whose CPU start is not a present's end")
        self.assertGreaterEqual(good, joined - 2, "v2 DisplayLatency vs screen - the presenter's CPU start")
        # The game's view of the same frames (paired, so the medians are of one set of frames).
        until = {k: pt.num(row["msUntilDisplayed"]) for row, k in a.v1 if k is not None and (pt.num(row["msUntilDisplayed"]) or 0) > 0}
        ks = [k for k, _ in paired if k in until]
        if ks:
            pt.show(run.name,
                    L=pt.med([a.lag[k] for k in ks]),
                    game_wait_in_present=pt.med([(run.truth[k - 1]["end"] - run.truth[k - 1]["begin"]) / 1e6 for k in ks]),
                    msUntilDisplayed=pt.med([until[k] for k in ks]),
                    game_present_to_display=pt.med([(a.screen[k] - run.truth[k]["begin"]) / 1e6 for k in ks]),
                    DisplayLatency=pt.med([dl for k, dl in paired if k in until]),
                    game_cpu_start_to_display=pt.med([(a.screen[k] - run.truth[k - 1]["end"]) / 1e6 for k in ks]))

    def test_fifo_frame_and_display_times(self):
        run = self.scenario("fifo")
        a = self.check_alignment(run)
        self.check_frames(run, a)
        change = self.check_display(run, a)
        # FIFO: one image per refresh, the game paced by it.
        self.assertAlmostEqual(change, pt.med(truth_intervals(run)), delta=0.02 * change)

    def test_gpu_columns_under_dxvk(self):
        check_gpu(self, self.scenario("fifo"), "fifo")

    def test_dxvk_frame_statistics_still_work(self):
        """DXVK 3.x implements GetFrameStatistics with VK_EXT_present_timing on the same swapchain the layer
        times: both must get their results."""
        run = self.scenario("fifo")
        ok = sum(1 for t in run.truth if t["fs_hr"] == "0x00000000")
        pt.show(run.name, framestats_ok=ok, frames=len(run.truth))
        self.assertGreaterEqual(ok, len(run.truth) - 3, run.describe())

    def test_cpu_columns_describe_the_presenter_thread(self):
        """Documented limitation (docs/ARCHITECTURE.md, risks): under DXVK the layer sees DXVK's presenter thread.
        If this starts failing, the CPU columns may describe the game thread now: update the docs and this test."""
        run = self.scenario("fifo")
        presenters = {}
        for r in run.ring:
            name = run.threads.get(r.get("thread", ""), "?")
            presenters[name] = presenters.get(name, 0) + 1
        pt.show(run.name, presenting_threads=presenters)
        self.assertTrue(presenters and all(n.startswith("dxvk-") for n in presenters), presenters)
        in_present = pt.med(truth_in_present(run))     # the game thread waits inside Present() (frame pacing)
        ms_in_present = pt.med([pt.num(r["msInPresentAPI"]) for r in run.v1])
        pt.show(run.name, game_in_present=in_present, msInPresentAPI=ms_in_present)
        self.assertGreater(in_present, 0.5 * pt.med(truth_intervals(run)))
        self.assertLess(ms_in_present, 1.0)

    def test_latency_error_model_vsync_bound(self):
        """The game waits inside Present() for most of the frame."""
        run = self.scenario("fifo")
        self.check_latency_columns(run, self.check_alignment(run))

    def test_latency_error_model_cpu_bound(self):
        """The game is busy 6 of 8.3 ms and waits inside Present() for the rest."""
        run = self.scenario("cpu6")
        a = self.check_alignment(run)
        self.check_frames(run, a)
        self.check_latency_columns(run, a)

    def test_frame_capped_immediate(self):
        run = self.scenario("cap100")
        a = self.check_alignment(run)
        self.check_frames(run, a)
        self.check_display(run, a)
        self.check_latency_columns(run, a)
        self.assertAlmostEqual(pt.med(truth_intervals(run)), 10.0, delta=0.05)

    @unittest.skipUnless(DXVK2_DIR, "no DXVK 2.x found (set PML_DXVK2_DIR)")
    def test_dxvk2_present_id_v1(self):
        """DXVK 2.x paces with VK_KHR_present_id + present_wait (v1 ids); the layer keeps the app's ids."""
        run = self.scenario("dxvk2")
        text = ""
        for p in glob.glob(os.path.join(run.dir, "*_dxgi.log")) + glob.glob(os.path.join(run.dir, "*_d3d11.log")):
            with open(p, errors="replace") as f:
                text += f.read()
        self.assertIn("DXVK: v2", text, "DXVK 2.x was not the one loaded")
        self.assertIn("VK_KHR_present_id\n", text.replace("\r", ""), "DXVK 2.x without VK_KHR_present_id?")
        a = self.check_alignment(run)
        self.check_frames(run, a)
        self.check_display(run, a)
        self.check_latency_columns(run, a)
        ok = sum(1 for t in run.truth if t["fs_hr"] == "0x00000000")
        self.assertGreaterEqual(ok, len(run.truth) - 3)

    def test_xwayland_no_display_times(self):
        """Through Xwayland the driver reports only queue-end times: every display column is NA, and nothing
        stalls waiting for display times."""
        if pt.SHARED_DESKTOP:
            self.skipTest("PML_E2E_SHARED_DESKTOP=1: under X11 Proton names the window steam_proton, which the KWin rule for test windows does not match")
        run = self.scenario("x11")
        pt.show(run.name, window=run.window)
        # Xwayland releases presents in bunches, so DXVK's lag spreads over up to a couple of refreshes.
        a = self.check_alignment(run, spread_max=3.0 * pt.med(truth_intervals(run)) + 2.0)
        self.assertTrue(all((pt.num(r["msUntilDisplayed"]) or 0) == 0 for r in run.v1), "display times on Xwayland?")
        self.assertTrue(all(r["DisplayLatency"] == "NA" for r in run.v2))
        # Xwayland FIFO intervals are bimodal (presents bunch up), so the medians of the two sides are not
        # comparable; the frame-by-frame comparison is.
        self.check_frames(run, a, per_frame=False)

    def test_overlay_draws_on_dxvk(self):
        run = self.scenario("overlay")
        self.check_frames(run, self.check_alignment(run))
        raw = read_raw(os.path.join(WORK, "dumps", "overlay.png.raw"))
        self.assertIsNotNone(raw, "no overlay dump\n" + run.describe())
        fmt, w, h, row, bpp, data = raw
        order = {37: (0, 1, 2), 43: (0, 1, 2), 44: (2, 1, 0), 50: (2, 1, 0)}.get(fmt)
        if order is None or bpp != 4:
            self.skipTest("dump format %d not checked here" % fmt)

        def light(x0, y0, x1, y1):
            # Light grey text: the app clears to a fixed saturated red (--colour), so G and B are high only where the
            # overlay drew.
            n = 0
            for y in range(y0, y1):
                base = y * row
                for x in range(x0, x1):
                    p = base + x * 4
                    if data[p + order[1]] > 150 and data[p + order[2]] > 150:
                        n += 1
            return n
        panel = light(8, 8, min(w, 300), min(h, 210))
        elsewhere = light(w - 300, h - 210, w, h)
        drawn = sum(1 for r in run.ring if "O" in r.get("flag_text", ""))
        cpu = [int(r.get("overlay_cpu_ns") or 0) / 1e6 for r in run.ring]
        # The first present creates the backend and uploads the atlas: tens of ms with a cold shader cache (a
        # fresh prefix), well under 1 ms later. Nothing in it may wait for the GPU, so it stays bounded.
        pt.show(run.name, panel_light_px=panel, corner_light_px=elsewhere, drawn=drawn, records=len(run.ring),
                first_present_overlay_ms=cpu[0] if cpu else -1, overlay_ms_median=pt.med(cpu[1:]) if len(cpu) > 1 else -1)
        self.assertGreater(panel, 600)
        self.assertLess(elsewhere, 20)
        self.assertGreaterEqual(drawn, len(run.ring) - 2)
        self.assertLess(pt.med(cpu[1:]), 1.0, "overlay CPU time per present")
        self.assertLess(cpu[0], 100.0, "overlay CPU time of the first present (backend creation + atlas upload)")


# ---------------------------------------------------------------------------------------
# D3D12 through vkd3d-proton
# ---------------------------------------------------------------------------------------

EXE12 = os.environ.get("PML_D3D12_TRUTH") or os.path.join(pt.BIN, "d3d12_truth.exe")


def vkd3d_has_present_timing(proton_dir):
    """True when the Proton's vkd3d-proton uses VK_EXT_present_timing (and so PRESENT_ID_2 swapchains)."""
    for sub in (("files", "lib", "wine", "vkd3d-proton"), ("files", "lib", "vkd3d-proton")):
        dll = os.path.join(proton_dir, *sub, "x86_64-windows", "d3d12core.dll")
        if os.path.isfile(dll):
            with open(dll, "rb") as f:
                return b"VK_EXT_present_timing" in f.read()
    return False


def find_proton_d3d12():
    """PML_PROTON_D3D12, else proton-wineland (the user's Proton), else the newest Proton whose vkd3d-proton uses
    present timing; from /mnt/alpha/Protons and Steam's compatibilitytools.d."""
    p = os.environ.get("PML_PROTON_D3D12")
    if p:
        return p if os.path.isfile(os.path.join(p, "proton")) else None
    cands = []
    for root in ("/mnt/alpha/Protons", COMPAT_DIR):
        cands += [c for c in glob.glob(os.path.join(root, "*")) if os.path.isfile(os.path.join(c, "proton"))]
    cands.sort(key=lambda c: ("wineland" in os.path.basename(c), _version_key(c)), reverse=True)
    for c in cands:
        if vkd3d_has_present_timing(c):
            return c
    return None


PROTON12 = find_proton_d3d12()


class ProtonD3D12(pt.LiveCase):
    """vkd3d-proton creates its swapchains with PRESENT_ID_2 + PRESENT_TIMING (the app owns the ids) but sends a
    present id only on FIFO presents (V-Sync on); with tearing it presents IMMEDIATE without one. The layer must
    still time every frame: it injects its own ids there, below the ones vkd3d-proton sends later. Seen in Lies of P
    (2026-11-09): every display column empty with V-Sync off."""
    runs = {}

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        if not PROTON12:
            raise unittest.SkipTest("no Proton with a present-timing vkd3d-proton found (set PML_PROTON_D3D12)")
        if not os.path.isfile(EXE12):
            raise unittest.SkipTest("missing %s (needs x86_64-w64-mingw32-g++ when configuring)" % EXE12)
        print("[proton-d3d12] %s" % PROTON12, flush=True)

    SCENARIOS = {
        "d3d12_tearing": ["--seconds", "5", "--sync", "0", "--fps", "120"],
        "d3d12_switch": ["--seconds", "6", "--sync", "0", "--switch-every", "90", "--fps", "120"],
    }

    def scenario(self, key):
        global PROTON
        if key not in self.runs:
            saved, PROTON = PROTON, PROTON12
            try:
                self.runs[key] = run_proton(key, self.SCENARIOS[key], exe=EXE12)
            finally:
                PROTON = saved
        run = self.runs[key]
        if pt.displays_asleep():
            self.skipTest("displays went to sleep (DPMS Off) during the run")
        self.assertIsNotNone(run.pid, run.describe())
        self.assertGreater(len(run.truth), 300, run.describe())
        self.assertEqual(sum(1 for t in run.truth if t.get("hr", "0x00000000") != "0x00000000"), 0, run.describe())
        return run

    def layer_log(self, run):
        try:
            with open(os.path.join(run.dir, "launcher.err"), errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def check_timed(self, run):
        ring = sorted(run.ring, key=lambda r: int(r["present_start_ns"]))
        self.assertGreaterEqual(len(ring), len(run.truth) - 3, run.describe())
        body = ring[:-4]   # the last presents may never reach the screen before exit
        # A complete report on every frame: displayed (D) or reported as never shown (N: the first frames after the
        # window maps are replaced), with the GPU-done time either way.
        untimed = [r["frame"] for r in body if int(r["ready_ns"]) == 0 or not int(r["flags"], 16) & 0x18]
        shown = sum(1 for r in body if int(r["screen_ns"]) > 0)
        by_sc = {}
        for r in ring:
            by_sc.setdefault(r["swapchain"], []).append(int(r["present_id"]))
        app_ids = sum(1 for r in ring if int(r["flags"], 16) & 0x20)
        pt.show(run.name, records=len(ring), untimed=len(untimed), displayed=shown, app_ids=app_ids,
                layer_ids=len(ring) - app_ids)
        self.assertLessEqual(len(untimed), 3, "presents without a timing report: %s\n%s" % (untimed[:20], run.describe()))
        # How many tearing frames KWin shows depends on what else loads the GPU (a game on the other screen:
        # 46-100 % in A/B runs, with or without the layer's GPU timing), so this only catches display times
        # missing altogether; the complete report on every frame above is the real check.
        self.assertGreaterEqual(shown, len(body) // 4, "presents without a display time\n" + run.describe())
        for sc, ids in by_sc.items():   # vkd3d-proton recreates its swapchain; ids are per swapchain
            self.assertTrue(all(i > 0 for i in ids), "a present went down without an id (swapchain %s)" % sc)
            self.assertTrue(all(b > a for a, b in zip(ids, ids[1:])), "present ids must increase: %s" % ids[:40])
        self.assertNotIn("stops adding ids", self.layer_log(run))
        check_gpu(self, run, run.name)
        latency = [r for r in run.v2 if r["DisplayLatency"] != "NA"]
        self.assertGreaterEqual(len(latency), len(run.v2) // 4, "v2 rows without DisplayLatency (see shown above)")
        return ring, app_ids

    def test_tearing_frames_get_display_times(self):
        run = self.scenario("d3d12_tearing")
        ring, app_ids = self.check_timed(run)
        self.assertEqual(app_ids, 0, "vkd3d-proton sent ids with tearing: the layer's injection is not exercised")

    def test_vsync_toggle_keeps_ids_in_order(self):
        """V-Sync toggled every 90 frames: vkd3d-proton's own ids (max(previous + 1, present count)) and the layer's
        alternate on one swapchain."""
        run = self.scenario("d3d12_switch")
        ring, app_ids = self.check_timed(run)
        self.assertGreater(app_ids, 60, "no FIFO presents with vkd3d-proton's own ids")
        self.assertGreater(len(ring) - app_ids, 60, "no tearing presents with the layer's ids")


if __name__ == "__main__":
    unittest.main()
