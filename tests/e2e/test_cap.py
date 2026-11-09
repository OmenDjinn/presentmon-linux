# SPDX-License-Identifier: MIT
"""Scenario 13: the layer's frame cap (src/layer/frame_limiter.hpp, docs/ARCHITECTURE.md "Frame caps").

Checks (any desktop):
  - native Vulkan, late and early placement: the cap holds the rate on the app's own clock, and the wait sits
    inside the present call;
  - `presentmon cap` changes the cap of a running app within a frame, `presentmon cap status` reports it;
  - Reflex (vk_truth --ll2): the cap goes into the driver's frame interval (VK_NV_low_latency2), nothing waits at
    the present, the app's own sleep does the waiting.

Comparisons (an otherwise idle machine; print a table, assert only what the design promises), every method at the
same target rate:
  - native vk_truth, windowed (fixed refresh): app cap, layer late/early, MangoHud late/early, uncapped;
  - Reflex vk_truth: the app's own LL2 interval, the layer through LL2, the layer at the present, MangoHud;
  - D3D11 under Proton/DXVK (needs what test_proton needs): the game's cap, layer late/early, DXVK_FRAME_RATE,
    MangoHud late/early;
  - VRR, opt-in with PML_E2E_CAP_VRR=1 (takes over the VRR screen for about a minute, fullscreen and focused;
    PML_E2E_APP_ID sets the window's app id for a window rule): screen pacing at the layer's auto cap against
    MangoHud and the app at the same rate.
Metrics: rate error (mean interval of the app's present calls vs the target), jitter of those intervals (sd and
p99 - p1), frame start to first pixel on screen (median, p95; frame start = where a game samples input), and CPU
time per frame (native: the process's user + system time over its frames).
"""
import bisect
import math
import os
import threading
import time
import unittest

import pmltest as pt
from pmltest import col, med, num, pct, show, trim

TARGET = 100.0
NATIVE_APP = ["--present-mode", "mailbox", "--cpu-ms", "1", "--size", "1280x720"]
REPS = 2


def mango(method, fps=TARGET):
    """MangoHud's limiter alone: MANGOHUD_CONFIG replaces the user's config file, no_display hides the HUD."""
    return {"MANGOHUD": "1", "MANGOHUD_CONFIG": "no_display,fps_limit=%g,fps_limit_method=%s" % (fps, method)}


def layer_cap(fps, placement="late", **extra):
    env = {"PRESENTMON_FPS_CAP": str(fps), "PRESENTMON_CAP_PLACEMENT": placement, "PRESENTMON_LOG": "info"}
    env.update(extra)
    return env


def stats(xs):
    if not xs:
        nan = float("nan")
        return {"n": 0, "mean": nan, "sd": nan, "p1": nan, "p50": nan, "p95": nan, "p99": nan}
    m = sum(xs) / len(xs)
    return {"n": len(xs), "mean": m, "sd": math.sqrt(sum((x - m) ** 2 for x in xs) / len(xs)), "p1": pct(xs, 1),
            "p50": med(xs), "p95": pct(xs, 95), "p99": pct(xs, 99)}


def rusage_ms(stdout):
    """user + system CPU ms of the whole vk_truth process (its '[vk_truth] rusage:' line)."""
    for line in stdout.splitlines():
        if "rusage:" in line:
            kv = dict(tok.split("=", 1) for tok in line.split() if "=" in tok)
            try:
                return float(kv["user_ms"]) + float(kv["sys_ms"])
            except (KeyError, ValueError):
                return None
    return None


def ll2_avg_sleep_ms(stdout):
    for line in stdout.splitlines():
        if "ll2: sleeps=" in line:
            try:
                return float(line.split("avg_sleep=")[1].split("ms")[0])
            except (IndexError, ValueError):
                return None
    return None


def native_latency_ms(run):
    """Frame start (vk_truth's cpu_start, after any app-side wait, where a game samples input) to first pixel on
    screen, per displayed frame.  A CSV row belongs to the truth frame whose vkQueuePresentKHR call contains the
    layer's present start: the layer runs inside the app's call whatever layer sits above it (MangoHud)."""
    pre = [t["present_pre_ns"] for t in run.truth]
    out = []
    for r in run.rows:
        t, until = num(r.get("TimeInQPC")), num(r.get("MsUntilDisplayed"))
        if t is None or until is None:
            continue
        t = int(t)
        i = bisect.bisect_right(pre, t) - 1
        if i < 0:
            continue
        f = run.truth[i]
        if f["present_pre_ns"] <= t <= f["present_post_ns"]:
            out.append((t + until * 1e6 - f["cpu_start_ns"]) / 1e6)
    return out


def native_metrics(run, target):
    s = stats(trim(run.truth_intervals_ms(), 0.1))
    lat = stats(trim(native_latency_ms(run), 0.1))
    cpu = rusage_ms(run.stdout)
    n = len(run.truth)
    return {"fps": 1000.0 / s["mean"], "err_pct": (1000.0 / s["mean"] / target - 1.0) * 100.0 if target else float("nan"),
            "sd_ms": s["sd"], "spread_ms": s["p99"] - s["p1"], "lat_ms": lat["p50"], "lat95_ms": lat["p95"],
            "cpu_ms_frame": cpu / n if cpu and n else float("nan"),
            "in_present_ms": med(trim(run.truth_in_present_ms(), 0.1)), "shown": float(lat["n"])}


def aggregate(samples):
    """Median of each metric over the repetitions."""
    keys = samples[0].keys()
    return {k: med([s[k] for s in samples if not math.isnan(s[k])]) if any(not math.isnan(s[k]) for s in samples)
            else float("nan") for k in keys}


def table(title, results, cols):
    lines = ["[measure] %s" % title, "[measure]   %-20s" % "method" + "".join("%13s" % c for c in cols)]
    for name, m in results:
        lines.append("[measure]   %-20s" % name + "".join("%13.3f" % m[c] for c in cols))
    print("\n".join(lines), flush=True)


def interval_window(run, t0, t1):
    """Present intervals (ms) of truth frames whose present call starts in [t0, t1) (CLOCK_MONOTONIC ns)."""
    t = run.truth
    return [(t[i]["present_pre_ns"] - t[i - 1]["present_pre_ns"]) / 1e6 for i in range(1, len(t))
            if t0 <= t[i - 1]["present_pre_ns"] and t[i]["present_pre_ns"] < t1]


class Cap(pt.LiveCase):
    def run_native(self, app, env, mangohud=False, seconds=3, capture=True):
        env = dict(env)
        is_mango = env.pop("MANGOHUD", None) == "1" or mangohud
        run = pt.run_scenario(["--seconds", str(seconds)] + app, capture=capture, env_extra=env, mangohud=is_mango)
        self.need(run)
        self.assertRun(run)
        return run

    def test_native_cap_late_and_early(self):
        """90 fps on a mailbox window: the rate within 0.1 % on the app's clock, steady intervals, the wait inside the
        present call (MsInPresentAPI), the status line in the layer's log."""
        for placement in ("late", "early"):
            run = self.run_native(NATIVE_APP, layer_cap(90, placement))
            m = native_metrics(run, 90.0)
            bp = med(trim(col(run.rows, "MsBetweenPresents"), 0.1))
            in_api = med(trim(col(run.rows, "MsInPresentAPI"), 0.1))
            show("cap 90 %s" % placement, fps=m["fps"], err_pct=m["err_pct"], sd_ms=m["sd_ms"],
                 spread_ms=m["spread_ms"], bp_ms=bp, in_present_api_ms=in_api, latency_ms=m["lat_ms"])
            self.assertIn("frame cap: 90.000 fps on swapchain", run.stderr, run.describe())
            self.assertIn("via present wait (%s)" % placement, run.stderr, run.describe())
            self.assertLess(abs(m["err_pct"]), 0.1, "rate on the app's clock")
            self.assertLess(m["sd_ms"], 0.15, "interval jitter")
            self.assertAlmostEqual(bp, 1000.0 / 90.0, delta=0.05)
            self.assertGreater(in_api, 7.0, "the wait is inside the present call")

    def test_live_change_from_the_cli(self):
        """60 fps from the environment, then `presentmon cap 120`, `status`, `off` while the app runs."""
        before = set(pt.pgrep("vk_truth"))
        marks = {}

        def drive():
            pid, end = None, time.time() + 20
            while pid is None and time.time() < end:
                for p in pt.pgrep("vk_truth"):
                    if p not in before and os.path.exists(pt.SEGMENT_FMT % p):
                        pid = p
                time.sleep(0.05)
            if pid is None:
                return
            time.sleep(1.5)
            for step, args, pause in (("120", ["120"], 1.0), ("status", ["status"], 0.5), ("off", ["off"], 0.0)):
                t = time.monotonic_ns()
                rc, out, err = pt.run_pm(["cap"] + args + ["--process_id", str(pid)])
                marks[step] = (t, time.monotonic_ns(), rc, out + err)
                time.sleep(pause)

        th = threading.Thread(target=drive, daemon=True)
        th.start()
        run = self.run_native(NATIVE_APP, layer_cap(60), seconds=6, capture=False)
        th.join(10)
        self.assertEqual(set(marks), {"120", "status", "off"}, "the CLI ran while the app did\n" + run.describe())
        for step, (_, _, rc, out) in marks.items():
            self.assertEqual(rc, 0, "presentmon cap %s: %s" % (step, out))
        self.assertIn("in force: 120.000 fps via present wait (late)", marks["status"][3])
        start = run.truth[0]["present_pre_ns"]
        a = interval_window(run, start + 300_000_000, marks["120"][0] - 50_000_000)
        b = interval_window(run, marks["120"][1] + 100_000_000, marks["off"][0] - 50_000_000)
        c = interval_window(run, marks["off"][1] + 100_000_000, run.truth[-1]["present_pre_ns"])
        show("live change", before_ms=med(a), after_120_ms=med(b), after_off_ms=med(c), frames=(len(a), len(b), len(c)))
        self.assertGreater(len(a), 30)
        self.assertGreater(len(b), 30)
        self.assertGreater(len(c), 30)
        self.assertAlmostEqual(med(a), 1000.0 / 60.0, delta=0.1)
        self.assertAlmostEqual(med(b), 1000.0 / 120.0, delta=0.1)
        self.assertLess(med(c), 5.0, "uncapped after `cap off`")
        self.assertIn("frame cap: off on swapchain", run.stderr)

    def test_reflex_interval(self):
        """vk_truth --ll2 (Reflex on, no interval of its own) with a 100 fps layer cap: the driver paces the app's
        vkLatencySleepNV, so the app sleeps before its frame and the present call does not wait."""
        run = self.run_native(["--ll2"] + NATIVE_APP, layer_cap(TARGET))
        m = native_metrics(run, TARGET)
        sleep = ll2_avg_sleep_ms(run.stdout)
        show("cap 100 via Reflex", fps=m["fps"], err_pct=m["err_pct"], sd_ms=m["sd_ms"], in_present_ms=m["in_present_ms"],
             app_sleep_ms=sleep, latency_ms=m["lat_ms"])
        self.assertIn("via Reflex interval", run.stderr, run.describe())
        # NVIDIA's interval runs a little long (10.055 ms for 10 ms measured on 615): 1 % tolerance.
        self.assertLess(abs(m["err_pct"]), 1.0)
        self.assertLess(m["in_present_ms"], 2.0, "no wait inside vkQueuePresentKHR")
        self.assertIsNotNone(sleep)
        self.assertGreater(sleep, 5.0, "the app's latency sleep holds the frame")


    def test_early_placement_never_uses_reflex(self):
        """A Reflex app with placement early: the cap waits at the present (evenly spaced presents), not in the
        Reflex interval, which spaces only the frames' starts."""
        run = self.run_native(["--ll2"] + NATIVE_APP, layer_cap(TARGET, "early"))
        m = native_metrics(run, TARGET)
        show("cap 100 early with Reflex in the app", fps=m["fps"], sd_ms=m["sd_ms"], in_present_ms=m["in_present_ms"])
        self.assertIn("via present wait (early)", run.stderr, run.describe())
        self.assertNotIn("via Reflex interval", run.stderr)
        self.assertLess(abs(m["err_pct"]), 0.1, "the layer's own grid holds the rate exactly")


@pt.not_on_shared_desktop("cap comparisons measure timing and need an otherwise idle machine")
class CapCompare(pt.LiveCase):
    def compare(self, title, configs, app_extra, target, reps=REPS, seconds=4):
        samples = {name: [] for name, _, _ in configs}
        for _ in range(reps):
            for name, app, env in configs:
                env = dict(env)
                is_mango = env.pop("MANGOHUD", None) == "1"
                run = pt.run_scenario(["--seconds", str(seconds)] + app + app_extra, env_extra=env, mangohud=is_mango)
                self.need(run)
                self.assertRun(run)
                samples[name].append(native_metrics(run, target))
        results = [(name, aggregate(samples[name])) for name, _, _ in configs]
        table("%s, target %.0f fps (median of %d runs)" % (title, target, reps), results,
              ("fps", "err_pct", "sd_ms", "spread_ms", "in_present_ms", "lat_ms", "lat95_ms", "cpu_ms_frame"))
        return dict(results)

    def test_native(self):
        r = self.compare("native Vulkan, mailbox window", [
            ("uncapped", [], {}),
            ("app cap", ["--fps", "%g" % TARGET], {}),
            ("layer late", [], layer_cap(TARGET)),
            ("layer early", [], layer_cap(TARGET, "early")),
            ("MangoHud late", [], mango("late")),
            ("MangoHud early", [], mango("early")),
        ], NATIVE_APP, TARGET)
        late, app = r["layer late"], r["app cap"]
        self.assertLess(abs(late["err_pct"]), 0.1)
        self.assertLess(late["sd_ms"], 0.1)
        # A single-threaded app starts its next frame when the present call returns: a late wait costs no latency
        # against the app's own cap.
        self.assertLess(late["lat_ms"] - app["lat_ms"], 1.0, "late placement adds no latency on native Vulkan")

    def test_reflex(self):
        self.compare("native Vulkan with Reflex (VK_NV_low_latency2)", [
            ("app LL2 interval", ["--ll2-interval-us", "%d" % round(1e6 / TARGET)], {}),
            ("layer via LL2", ["--ll2"], layer_cap(TARGET)),
            ("layer at present", ["--ll2"], layer_cap(TARGET, PRESENTMON_CAP_REFLEX="0")),
            ("MangoHud late", ["--ll2"], mango("late")),
        ], NATIVE_APP, TARGET)

    def dxvk_compare(self, title, sync, configs, d3d12=False, reps=None):
        """D3D11 (DXVK) or D3D12 (vkd3d-proton) truth app under Proton, `reps` runs per config (PML_E2E_CAP_REPS,
        default 1), medians.  Latency is from the game thread's frame start (D3D11: after the game's own cap wait;
        D3D12, which has no CPU work: its Present() call) to the first pixel.  Returns {name: metrics} with
        "method" = the layer's last 'frame cap: ... via ...' log line of that config."""
        import test_proton as tp
        if d3d12:
            if not tp.PROTON12 or not os.path.isfile(tp.EXE12):
                self.skipTest("no Proton with a present-timing vkd3d-proton, or no d3d12_truth.exe")
        else:
            why = tp.skip_reason()
            if why:
                self.skipTest(why)
        reps = reps or int(os.environ.get("PML_E2E_CAP_REPS", "1"))
        samples = {name: [] for name, _, _ in configs}
        methods = {}
        window_shown = False
        for rep in range(reps):
            for name, app, env in configs:
                tag = "cap-%s-%s-sync%d" % ("d3d12" if d3d12 else "d3d11", name.replace(" ", "-").lower(), sync)
                if d3d12:
                    saved, tp.PROTON = tp.PROTON, tp.PROTON12
                    try:
                        run = tp.run_proton(tag, ["--seconds", "6", "--sync", str(sync)] + app, env_extra=env,
                                            exe=tp.EXE12)
                    finally:
                        tp.PROTON = saved
                else:
                    run = tp.run_proton(tag, ["--seconds", "6", "--sync", str(sync), "--cpu-ms", "1"] + app,
                                        env_extra=env)
                if pt.displays_asleep():
                    self.skipTest("displays went to sleep (DPMS Off) during the run")
                self.assertIsNotNone(run.pid, run.describe())
                self.assertGreater(len(run.truth), 100, run.describe())
                if not window_shown:
                    window_shown = True
                    show("proton window", window=run.window.replace(" ", "_") or "unknown")
                a = tp.Aligned(run)
                game = stats(trim(tp.truth_intervals(run), 0.1))
                pres = stats(trim([(a.start[k] - a.start[k - 1]) / 1e6 for k in range(1, a.n)], 0.1))
                base = "begin" if d3d12 else "work"
                lat = stats(trim([(a.screen[k] - run.truth[k][base]) / 1e6 for k in range(a.n) if a.screen[k] > 0],
                                 0.1))
                for line in open(os.path.join(run.dir, "launcher.err"), errors="replace"):
                    if "frame cap:" in line and " via " in line:
                        methods[name] = line.split(" via ", 1)[1].strip()
                samples[name].append({"fps": 1000.0 / game["mean"],
                                      "err_pct": (1000.0 / game["mean"] / TARGET - 1) * 100,
                                      "game_sd_ms": game["sd"], "present_sd_ms": pres["sd"],
                                      "in_present_ms": med(trim(tp.truth_in_present(run), 0.1)),
                                      "lat_ms": lat["p50"], "lat95_ms": lat["p95"]})
        results = [(name, aggregate(samples[name])) for name, _, _ in configs]
        for name, method in methods.items():
            show("%s method" % name, via=method.replace(" ", "_"))
        where = os.path.basename(tp.PROTON12) if d3d12 else "DXVK %s" % tp.dxvk_version(tp.PROTON)
        table("%s, %s, target %.0f fps (median of %d runs)" % (title, where, TARGET, reps), results,
              ("fps", "err_pct", "game_sd_ms", "present_sd_ms", "in_present_ms", "lat_ms", "lat95_ms"))
        out = dict(results)
        for name, method in methods.items():
            out[name]["method"] = method
        return out

    def test_dxvk(self):
        """V-Sync off (sync 0). DXVK's frame thread polls the present timing (VK_EXT_present_timing, which the layer
        offers on NVIDIA) once per frame before it releases the game thread: the layer's late cap holds that poll,
        so it has the game's own latency. MangoHud (and the layer's early placement) wait on DXVK's submit thread
        and cost a frame. Needs a DXVK with present-timing support (3.1.1+); older ones fall back to the late wait."""
        r = self.dxvk_compare("D3D11 under Proton/DXVK, V-Sync off", 0, [
            ("game cap", ["--fps", "%g" % TARGET], {}),
            ("layer late", ["--allow-uncapped"], layer_cap(TARGET)),
            ("layer early", ["--allow-uncapped"], layer_cap(TARGET, "early")),
            ("DXVK_FRAME_RATE", ["--allow-uncapped"], {"DXVK_FRAME_RATE": "%g" % TARGET}),
            ("MangoHud late", ["--allow-uncapped"], mango("late")),
            ("MangoHud early", ["--allow-uncapped"], mango("early")),
        ])
        late = r["layer late"]
        self.assertLess(abs(late["err_pct"]), 0.1)
        if "present-timing poll" in late.get("method", ""):
            frame = 1000.0 / TARGET
            self.assertLess(late["lat_ms"], r["MangoHud late"]["lat_ms"] - frame / 2,
                            "holding DXVK's frame thread saves about a frame against a present-thread wait")
            self.assertLess(late["lat_ms"], r["game cap"]["lat_ms"] + frame / 2, "about the game's own latency")
        else:
            print("[measure] the frame-thread hold did not engage (DXVK without present timing?): %s"
                  % late.get("method"), flush=True)

    def test_dxvk_vsync(self):
        """V-Sync on (sync 1, FIFO). DXVK 3.1.1+ with present timing does not wait for FIFO presents, and its timing
        poll holds a lock its submit thread needs for FIFO presents, so the layer stays on the late wait here: a
        frame more latency than DXVK_FRAME_RATE, as MangoHud. (Older DXVK waits for every FIFO present, which the
        layer holds instead: see test_vkd3d_vsync.)"""
        r = self.dxvk_compare("D3D11 under Proton/DXVK, V-Sync on", 1, [
            ("game cap", ["--fps", "%g" % TARGET], {}),
            ("layer late", [], layer_cap(TARGET)),
            ("DXVK_FRAME_RATE", [], {"DXVK_FRAME_RATE": "%g" % TARGET}),
            ("MangoHud late", [], mango("late")),
        ])
        self.assertLess(abs(r["layer late"]["err_pct"]), 0.1)

    def test_vkd3d_vsync(self):
        """D3D12 under vkd3d-proton with V-Sync on: its wait worker waits for every FIFO present (vkWaitForPresent2KHR)
        before it releases the game thread; the layer holds that wait, with the game's own latency. The window's
        class is steam_proton: a KWin window rule for that class keeps it off the main screen."""
        r = self.dxvk_compare("D3D12 under Proton/vkd3d-proton, V-Sync on", 1, [
            ("game cap", ["--fps", "%g" % TARGET], {}),
            ("layer late", [], layer_cap(TARGET)),
            ("VKD3D_FRAME_RATE", [], {"VKD3D_FRAME_RATE": "%g" % TARGET}),
            ("MangoHud late", [], mango("late")),
        ], d3d12=True)
        late = r["layer late"]
        self.assertLess(abs(late["err_pct"]), 0.1)
        self.assertIn("vkWaitForPresent", late.get("method", ""), "the layer holds vkd3d-proton's present wait")
        frame = 1000.0 / TARGET
        self.assertLess(late["lat_ms"], r["MangoHud late"]["lat_ms"] - frame / 2)
        self.assertLess(late["lat_ms"], r["game cap"]["lat_ms"] + frame / 2)

    def test_vrr_auto(self):
        """Fullscreen FIFO on the VRR screen: the layer's auto cap (just under the maximum) against MangoHud and the
        app at the same rate.  Screen pacing = the spacing of first-pixel-out times while VRR_ENABLED was 1."""
        if os.environ.get("PML_E2E_CAP_VRR") != "1":
            self.skipTest("set PML_E2E_CAP_VRR=1: takes over the VRR screen fullscreen for about a minute")
        displays = pt.list_displays()
        want = os.environ.get("PML_E2E_CAP_VRR_DISPLAY")
        disp = next((d for d in displays if d["name"] == want), None) if want else max(displays, key=lambda d: d["hz"], default=None)
        if not disp:
            self.skipTest("no display found")
        hz = disp["hz"]
        cap = math.floor(hz - hz * hz / 3600.0)
        target, why = pt.drm_connector_for_display(disp["name"])
        app = ["--present-mode", "fifo", "--cpu-ms", "1", "--fullscreen", "--display", str(disp["index"])]
        app_id = os.environ.get("PML_E2E_APP_ID")
        base = {"SDL_APP_ID": app_id} if app_id else {}
        configs = [
            ("app cap", ["--fps", str(cap)], {}),
            ("layer auto late", [], layer_cap("auto")),
            ("layer auto early", [], layer_cap("auto", "early")),
            ("MangoHud late", [], mango("late", cap)),
            ("MangoHud early", [], mango("early", cap)),
        ]
        samples = {name: [] for name, _, _ in configs}
        for _ in range(REPS):
            for name, extra, env in configs:
                env = dict(base, **env)
                is_mango = env.pop("MANGOHUD", None) == "1"
                with pt.VrrWatch(target, why) as watch:
                    run = pt.run_scenario(["--seconds", "6"] + app + extra, env_extra=env, mangohud=is_mango)
                self.need(run)
                self.assertRun(run)
                m = vrr_window_metrics(run, watch, cap)
                if m is None:
                    show("vrr %s" % name, skipped="VRR never on long enough",
                         vrr_enabled=pt.describe_steps(watch.steps, run.truth[0]["present_pre_ns"]) if run.truth else "")
                    continue
                show("vrr %s" % name, frames=m["frames"], fps=m["fps"], screen_sd_ms=m["screen_sd_ms"],
                     vrr_enabled=pt.describe_steps(watch.steps, run.truth[0]["present_pre_ns"]))
                if name.startswith("layer"):
                    self.assertIn("frame cap: %d.000 fps" % cap, run.stderr, "auto picked floor(hz - hz^2/3600)")
                samples[name].append(m)
        results = [(name, aggregate(samples[name])) for name, _, _ in configs if samples[name]]
        table("VRR fullscreen FIFO on %s %.2f Hz, cap %d fps, steady VRR-on window (median of %d runs)" % (
              disp["name"], hz, cap, REPS), results,
              ("fps", "err_pct", "sd_ms", "spread_ms", "screen_sd_ms", "screen_spread_ms", "lat_ms", "lat95_ms", "frames"))


def vrr_window_metrics(run, watch, cap):
    """Metrics over the steady part of a VRR run: from 1 s after VRR_ENABLED turned 1 (the auto cap polls DRM every
    500 ms) to 0.3 s before the end, frames shown while VRR was on.  None when that window is under a second."""
    if not run.truth:
        return None
    on_ns = None
    if watch.available:
        for t, v in watch.steps:
            if v == 1:
                on_ns = t
                break
        if on_ns is None:
            return None
    else:
        on_ns = run.truth[0]["present_pre_ns"]
    t0, t1 = on_ns + 1_000_000_000, run.truth[-1]["present_pre_ns"] - 300_000_000
    if t1 - t0 < 1_000_000_000:
        return None
    iv = interval_window(run, t0, t1)
    shown = [f for f in pt.display_frames(run.rows) if t0 <= f[0] <= t1 and
             (not watch.available or pt.vrr_state_at(watch.steps, f[0]) == 1)]
    pre = [t["present_pre_ns"] for t in run.truth]
    lat = []
    for r in run.rows:
        t, until = num(r.get("TimeInQPC")), num(r.get("MsUntilDisplayed"))
        if t is None or until is None or not (t0 <= t <= t1):
            continue
        i = bisect.bisect_right(pre, int(t)) - 1
        if i >= 0 and run.truth[i]["present_pre_ns"] <= t <= run.truth[i]["present_post_ns"]:
            lat.append((t + until * 1e6 - run.truth[i]["cpu_start_ns"]) / 1e6)
    s, sc, l = stats(iv), stats([f[1] for f in shown]), stats(lat)
    if s["n"] < 100:
        return None
    return {"fps": 1000.0 / s["mean"], "err_pct": (1000.0 / s["mean"] / cap - 1.0) * 100.0, "sd_ms": s["sd"],
            "spread_ms": s["p99"] - s["p1"], "screen_sd_ms": sc["sd"], "screen_spread_ms": sc["p99"] - sc["p1"],
            "lat_ms": l["p50"], "lat95_ms": l["p95"], "frames": float(s["n"])}


if __name__ == "__main__":
    unittest.main(verbosity=2)
