#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real-GPU smoke test of VkLayer_presentmon with vkcube (stdlib only).

Runs vkcube under the build-tree layer, reads the shared-memory ring with ring_dump and checks the records:
order, sane CPU times, display times on Wayland, intervals in FIFO, no display info on Xwayland, segment
unlinked after exit. Also runs vkcube without PRESENTMON_LAYER to confirm the layer stays out.

  smoke_vkcube.py --build-dir build/c-layer [--frames 300] [--wsi wayland,xcb]
Exit code 77 (ctest "skipped") when vkcube or a display is missing, or the displays are asleep (DPMS Off;
PML_E2E_WAKE=1 wakes them first).
"""
import argparse
import csv
import io
import os
import shutil
import statistics
import subprocess
import sys
import time

FLAG_TIMING_REQUESTED = 1 << 0
FLAG_TIMING_COMPLETE = 1 << 1
FLAG_TIMING_LOST = 1 << 2
FLAG_DISPLAYED = 1 << 3
FLAG_NOT_DISPLAYED = 1 << 4

failures = []


def displays_asleep():
    """Connected DRM connectors in DPMS Off (empty when unknown)."""
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


def screens_awake():
    """False when a connected display is in DPMS Off: KWin then throttles windows and reports no display times, so
    the timing checks would fail for no product reason.  With PML_E2E_WAKE=1 the screens are woken first."""
    if displays_asleep() and os.environ.get("PML_E2E_WAKE") == "1" and shutil.which("kscreen-doctor"):
        subprocess.run(["kscreen-doctor", "--dpms", "on"], capture_output=True, timeout=20)
        time.sleep(1.0)
    return not displays_asleep()


def check(cond, message):
    print(("  ok   " if cond else "  FAIL ") + message)
    if not cond:
        failures.append(message)


def run_vkcube(build, wsi, mode, frames, layer):
    env = dict(os.environ)
    env["VK_ADD_IMPLICIT_LAYER_PATH"] = os.path.join(build, "layer")
    env.pop("PRESENTMON_LAYER", None)
    if layer:
        env["PRESENTMON_LAYER"] = "1"
        env["PRESENTMON_OVERLAY"] = "off"
    cmd = ["vkcube", "--wsi", wsi, "--c", str(frames), "--width", "320", "--height", "240", "--suppress_popups"]
    if mode is not None:
        cmd += ["--present_mode", str(mode)]
    proc = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    dump = None
    if layer:
        dump = subprocess.Popen([os.path.join(build, "bin", "ring_dump"), "--pid", str(proc.pid), "--oldest",
                                 "--wait-for-segment", "10"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        err = proc.communicate(timeout=60)[1].decode(errors="replace")
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()
        check(False, "vkcube finished within 60 s")
        err = ""
    rows, dump_err = [], ""
    if dump:
        try:
            out, derr = dump.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            dump.kill()
            out, derr = dump.communicate()
        dump_err = derr.decode(errors="replace")
        rows = list(csv.DictReader(io.StringIO(out.decode())))
    return proc, rows, err, dump_err


def segment_gone(pid):
    return not os.path.exists("/dev/shm/presentmon-linux.%d" % pid)


def analyse(rows, name):
    by_swapchain = {}
    for r in rows:
        by_swapchain.setdefault(int(r["swapchain"]), []).append(r)
    return by_swapchain


def check_cpu(rows, frames, name):
    check(len(rows) >= frames * 0.9, "%s: %d records for %d frames" % (name, len(rows), frames))
    for sc, recs in analyse(rows, name).items():
        idx = [int(r["frame"]) for r in recs]
        check(idx == list(range(1, len(idx) + 1)), "%s: swapchain %d frame indices contiguous and in order (%d)" % (name, sc, len(idx)))
        starts = [int(r["present_start_ns"]) for r in recs]
        ends = [int(r["present_end_ns"]) for r in recs]
        check(all(a <= b for a, b in zip(starts, starts[1:])), "%s: swapchain %d presentStart non-decreasing" % (name, sc))
        check(all(e >= s > 0 for s, e in zip(starts, ends)), "%s: swapchain %d presentEnd >= presentStart > 0" % (name, sc))
        check(max(e - s for s, e in zip(starts, ends)) < 50_000_000, "%s: swapchain %d time in present < 50 ms" % (name, sc))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--wsi", default="wayland,xcb")
    args = ap.parse_args()
    build = os.path.abspath(args.build_dir)

    if subprocess.call(["which", "vkcube"], stdout=subprocess.DEVNULL) != 0:
        print("vkcube not found, skipping")
        return 77
    if not os.environ.get("WAYLAND_DISPLAY") and not os.environ.get("DISPLAY"):
        print("no display, skipping")
        return 77
    if not screens_awake():
        print("displays are asleep (DPMS Off), skipping; set PML_E2E_WAKE=1 to wake them")
        return 77
    if not os.path.exists(os.path.join(build, "layer", "VkLayer_presentmon.json")):
        print("layer not built")
        return 1

    for wsi in args.wsi.split(","):
        modes = [0, 1, 2, 3] if wsi == "wayland" else [2]
        for mode in modes:
            # FIFO_RELAXED is not offered on Wayland by this driver: vkcube itself exits with an error.
            if subprocess.call(["vkcube", "--wsi", wsi, "--c", "3", "--present_mode", str(mode)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) != 0:
                print("== %s mode %d: not supported by vkcube/driver here, skipped" % (wsi, mode))
                continue
            name = "%s mode %d" % (wsi, mode)
            print("== %s, %d frames" % (name, args.frames))
            proc, rows, err, dump_err = run_vkcube(build, wsi, mode, args.frames, True)
            check(proc.returncode == 0, "%s: vkcube exit code %d" % (name, proc.returncode))
            if proc.returncode != 0:
                print(err[-400:])
            check_cpu(rows, args.frames, name)
            check(segment_gone(proc.pid), "%s: segment unlinked after exit" % name)
            if dump_err.strip():
                print("  ring_dump:", dump_err.strip())

            complete = [r for r in rows if int(r["flags"], 16) & FLAG_TIMING_COMPLETE]
            lost = [r for r in rows if int(r["flags"], 16) & FLAG_TIMING_LOST]
            check(len(complete) >= len(rows) * (0.9 if mode in (2, 3) else 0.8), "%s: %d of %d records have complete timing (%d lost)" % (name, len(complete), len(rows), len(lost)))
            if wsi == "wayland":
                disp = [r for r in complete if int(r["flags"], 16) & FLAG_DISPLAYED]
                notdisp = [r for r in complete if int(r["flags"], 16) & FLAG_NOT_DISPLAYED]
                check(len(disp) + len(notdisp) == len(complete), "%s: every complete record is displayed or not-displayed" % name)
                check(all(int(r["screen_ns"]) > int(r["ready_ns"]) > 0 for r in disp), "%s: screen > ready > 0 on displayed records" % name)
                check(all(int(r["ready_ns"]) > int(r["present_start_ns"]) for r in disp[10:]), "%s: ready after presentStart" % name)
                print("  displayed %d, not displayed %d" % (len(disp), len(notdisp)))
                if mode == 2:
                    check(len(disp) >= len(complete) * 0.6, "%s: most FIFO frames displayed" % name)
                    # Interval between consecutive displayed frames of the same swapchain, in ms.
                    deltas = []
                    for sc, recs in analyse(rows, name).items():
                        screens = [int(r["screen_ns"]) for r in recs if int(r["flags"], 16) & FLAG_DISPLAYED]
                        deltas += [(b - a) / 1e6 for a, b in zip(screens, screens[1:])]
                    if deltas:
                        med = statistics.median(deltas)
                        print("  median screen interval %.3f ms" % med)
                        near = [abs(med - p) / p for p in (1000 / 240, 1000 / 120)]
                        check(min(near) < 0.12, "%s: median interval %.3f ms is one refresh period (4.17 ms DP-2 or 8.33 ms DP-1)" % (name, med))
            else:
                check(all(int(r["screen_ns"]) == 0 for r in rows), "%s: no screen time on Xwayland" % name)
                check(not any(int(r["flags"], 16) & (FLAG_DISPLAYED | FLAG_NOT_DISPLAYED) for r in rows), "%s: no displayed/not-displayed flags on Xwayland" % name)
                check(all(int(r["ready_ns"]) > 0 for r in complete), "%s: readyNs set on Xwayland" % name)

    print("== without PRESENTMON_LAYER")
    proc, rows, err, _ = run_vkcube(build, "wayland", 2, 120, False)
    check(proc.returncode == 0, "vkcube without the layer: exit code %d" % proc.returncode)
    check(segment_gone(proc.pid), "no segment created without PRESENTMON_LAYER")

    print("\n%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
