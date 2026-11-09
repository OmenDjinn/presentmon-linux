#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real-GPU tests of the in-game overlay with vk_truth (needs a graphical session, no validation layers).

  overlay_live.py --build-dir build/d-overlay               run the functional tests
  overlay_live.py --build-dir build/d-overlay --bench       also measure overhead (frame time, overlayCpuNs)

Dumps (PNG + .raw) are written to <build-dir>/dumps/. Exit code 77 = skipped (missing binary, no display, or displays asleep; PML_E2E_WAKE=1 wakes them).
"""
import argparse, os, shutil, signal, struct, subprocess, sys, time, glob, statistics
import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("--build-dir", required=True)
ap.add_argument("--bench", action="store_true")
ap.add_argument("--only", default="")
args = ap.parse_args()

BUILD = os.path.abspath(args.build_dir)
BIN = os.path.join(BUILD, "bin")
TRUTH = os.path.join(BIN, "vk_truth")
RING = os.path.join(BIN, "ring_dump")
CTL = os.path.join(BIN, "overlay_ctl")
DUMPS = os.path.join(BUILD, "dumps")
for p in (TRUTH, RING, CTL, os.path.join(BUILD, "layer", "libVkLayer_presentmon.so")):
    if not os.path.exists(p):
        print("skip: missing", p)
        sys.exit(77)


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


if not os.environ.get("WAYLAND_DISPLAY"):
    print("skip: no WAYLAND_DISPLAY")
    sys.exit(77)
if not screens_awake():
    print("skip: displays are asleep (DPMS Off); set PML_E2E_WAKE=1 to wake them")
    sys.exit(77)
os.makedirs(DUMPS, exist_ok=True)

VK_FORMAT = {50: "B8G8R8A8_SRGB", 44: "B8G8R8A8_UNORM", 64: "A2B10G10R10", 58: "A2R10G10B10", 97: "R16G16B16A16_SFLOAT"}
failures = []
results = []


def check(cond, msg):
    results.append((bool(cond), msg))
    print(("  ok   " if cond else "  FAIL ") + msg, flush=True)
    if not cond:
        failures.append(msg)


def env_for(layer=True, **kw):
    e = dict(os.environ)
    for k in list(e):
        if k.startswith("PRESENTMON"):
            del e[k]
    if layer:
        e["VK_ADD_IMPLICIT_LAYER_PATH"] = os.path.join(BUILD, "layer")
        e["PRESENTMON_LAYER"] = "1"
    for k, v in kw.items():
        e[k] = str(v)
    return e


def run_truth(extra, env, timeout=40):
    p = subprocess.run([TRUTH] + extra, env=env, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def read_raw(path):
    with open(path + ".raw", "rb") as f:
        head = f.read(64)
        assert head[:8] == b"PMLDUMP1", head[:8]
        fmt, cs, w, h, row, bpp, frame = struct.unpack_from("<7I", head, 8)
        data = f.read()
    return fmt, cs, w, h, bpp, frame, data


def channels(raw):
    """Returns (float32 array h x w x 3 of the encoded values in their native scale, white_code_description)."""
    fmt, cs, w, h, bpp, frame, data = raw
    if fmt in (50, 44):                     # BGRA8 -> RGB
        a = np.frombuffer(data, np.uint8).reshape(h, w, 4)[:, :, [2, 1, 0]].astype(np.float32)
    elif fmt in (64, 58):                   # 10-bit
        v = np.frombuffer(data, np.uint32).reshape(h, w)
        c = [(v & 0x3ff), ((v >> 10) & 0x3ff), ((v >> 20) & 0x3ff)]
        if fmt == 58:
            c = c[::-1]
        a = np.stack(c, axis=-1).astype(np.float32)
    elif fmt == 97:
        a = np.frombuffer(data, np.float16).reshape(h, w, 4)[:, :, :3].astype(np.float32)
    else:
        raise ValueError(fmt)
    return a


def count_code(a, region, lo, hi):
    x0, y0, x1, y1 = region
    r = a[y0:y1, x0:x1]
    m = np.all((r >= lo) & (r <= hi), axis=-1)
    return int(m.sum())


def expected_white(fmt, cs):
    if fmt == 97:
        return 2.5371 - 0.003, 2.5371 + 0.003     # 203 nits / 80, nearest half float
    if fmt in (64, 58) and cs == 1000104008:               # HDR10: PQ(203 nits) * 1023
        return 592.0, 596.0
    return 255.0, 255.0                            # SDR 8-bit (sRGB-encoded values)


TOP_LEFT = (12, 80, 320, 230)      # panel at scale 0.75 (auto at 720p)
BIG = (24, 80, 780, 600)           # panel at scale 2.0: text strokes are wide enough to reach full white


def dump_run(name, truth_args, overlay_env=None, frame=90, layer_env=None, seconds=3, rc_ok=(0,)):
    path = os.path.join(DUMPS, name + ".png")
    for f in glob.glob(path + "*"):
        os.remove(f)
    kw = dict(PRESENTMON_OVERLAY_DUMP=path, PRESENTMON_OVERLAY_DUMP_FRAME=frame, PRESENTMON_LOG="info")
    kw.update(overlay_env or {})
    e = env_for(**kw)
    rc, out = run_truth(["--seconds", str(seconds), "--size", "1280x720", "--fps", "120"] + truth_args, e)
    return path, rc, out


def t_formats():
    print("formats: white text value per colour space")
    for name in ("srgb", "unorm", "hdr10", "scrgb"):
        base, rc, out = dump_run("hidden_" + name, ["--format", name], {"PRESENTMON_OVERLAY": "0", "PRESENTMON_OVERLAY_SCALE": "2"})
        check(rc == 0, f"{name}: vk_truth rc=0 with overlay hidden (rc={rc})")
        path, rc, out = dump_run(name, ["--format", name], {"PRESENTMON_OVERLAY_SCALE": "2"})
        check(rc == 0, f"{name}: vk_truth rc=0 with overlay (rc={rc})")
        check(os.path.exists(path) and os.path.exists(path + ".raw"), f"{name}: dump files written")
        if not os.path.exists(path + ".raw"):
            print(out)
            continue
        raw = read_raw(path)
        rawh = read_raw(base)
        fmt, cs = raw[0], raw[1]
        lo, hi = expected_white(fmt, cs)
        on = count_code(channels(raw), BIG, lo, hi)
        off = count_code(channels(rawh), BIG, lo, hi)
        what = {255.0: "255 (1.0)", 592.0: "PQ code 592..596 (0.58 of 1023)"}.get(lo, "2.537 linear")
        check(on >= 100 and off <= 10, f"{name} ({VK_FORMAT.get(fmt, fmt)}): white text = {what}: {on} pixels with overlay, {off} hidden")
        # The panel must actually be translucent-dark, not a flat fill: the app pattern shows through.
        a = channels(raw)[100:300, 30:700]
        check(a.std() > 0.0, f"{name}: panel region has content")


def t_present_modes():
    print("present modes with app timing + present ids (the overlay must not disturb them)")
    for mode, fps in (("fifo", 120), ("mailbox", 1000), ("immediate", 1000)):
        path = os.path.join(DUMPS, f"mode_{mode}.png")
        for f in glob.glob(path + "*"):
            os.remove(f)
        e = env_for(PRESENTMON_OVERLAY_DUMP=path, PRESENTMON_OVERLAY_DUMP_FRAME=200, PRESENTMON_LOG="warn", PRESENTMON_OVERLAY_SCALE=2)
        rc, out = run_truth(["--seconds", "3", "--size", "1280x720", "--fps", str(fps), "--present-mode", mode,
                             "--app-timing", "--app-present-id"], e)
        anomalies = [l for l in out.splitlines() if "present-timing anomalies" in l]
        check(rc == 0, f"{mode}: vk_truth --app-timing --app-present-id exits 0 with the overlay (rc={rc}) {anomalies}")
        check(os.path.exists(path), f"{mode}: dump written")
        warn = [l for l in out.splitlines() if "[presentmon]" in l and "overlay" in l]
        check(not warn, f"{mode}: no overlay warnings {warn[:2]}")
        if os.path.exists(path + ".raw"):
            raw = read_raw(path)
            lo, hi = expected_white(raw[0], raw[1])
            on = count_code(channels(raw), BIG, lo, hi)
            check(on >= 100, f"{mode}: overlay visible in the dump ({on} white pixels)")


def t_xwayland():
    print("Xwayland: display items show NA")
    path, rc, out = dump_run("xwayland", ["--wsi", "x11", "--app-timing", "--app-present-id"],
                             {"PRESENTMON_LOG": "debug"}, frame=300, seconds=4)
    check(rc == 0, f"x11: vk_truth rc=0 (rc={rc})")
    lines = [l for l in out.splitlines() if "overlay: swapchain" in l and "present_fps" in l]
    check(lines, "x11: overlay summary logged")
    if lines:
        l = lines[-1]
        print("     ", l)
        check("queue-end only" in l, "x11: timing line says queue-end only")
        for key in ("shown_fps=NA", "display_latency_ms=NA", "until_displayed_ms=NA", "dropped_pct=NA"):
            check(key in l, f"x11: {key}")
        check("present_fps=NA" not in l and "avg_ms=NA" not in l and "cpu_busy_ms=NA" not in l, "x11: CPU/present numbers are present")
    # and Wayland shows numbers
    path, rc, out = dump_run("wayland_debug", ["--app-timing", "--app-present-id"], {"PRESENTMON_LOG": "debug"}, frame=300, seconds=4)
    lines = [l for l in out.splitlines() if "overlay: swapchain" in l and "present_fps" in l]
    if lines:
        print("     ", lines[-1])
        check("shown_fps=NA" not in lines[-1] and "display_latency_ms=NA" not in lines[-1], "wayland: display numbers present")
        check("pixel-out" in lines[-1], "wayland: timing line says pixel-out")
    else:
        check(False, "wayland: overlay summary logged")


def t_off():
    print("PRESENTMON_OVERLAY=off")
    e = env_for(PRESENTMON_OVERLAY="off", PRESENTMON_LOG="info", PRESENTMON_OVERLAY_DUMP=os.path.join(DUMPS, "off.png"))
    rc, out = run_truth(["--seconds", "2", "--size", "640x360", "--fps", "60"], e)
    sw = [l for l in out.splitlines() if "[presentmon] swapchain" in l]
    check(rc == 0, "off: vk_truth rc=0")
    check(sw and all("overlay" not in l for l in sw), "off: layer reports no overlay on the swapchain")
    usage = [l.split("usage ")[1] for l in sw if "usage " in l]
    same = [u.split(" ")[0] == u.split("(app ")[1].rstrip(")") for u in usage]
    check(usage and all(same), f"off: swapchain usage unchanged ({usage[:1]})")
    check(not any("overlay:" in l for l in out.splitlines()), "off: overlay code never ran")
    check(not os.path.exists(os.path.join(DUMPS, "off.png")), "off: no dump written")
    # on: usage gains the bits the overlay needs
    e = env_for(PRESENTMON_LOG="info", PRESENTMON_OVERLAY_DUMP=os.path.join(DUMPS, "on_usage.png"))
    rc, out = run_truth(["--seconds", "2", "--size", "640x360", "--fps", "60"], e)
    sw = [l for l in out.splitlines() if "[presentmon] swapchain" in l and "usage " in l]
    u = sw[-1].split("usage ")[1] if sw else ""
    sent = int(u.split(" ")[0], 16) if u else 0
    app = int(u.split("(app ")[1].rstrip(")"), 16) if u else 0
    check(sw and (sent & 0x10) and (sent & 0x1) and not (app & 0x1), f"on: usage 0x{app:x} -> 0x{sent:x} (+COLOR_ATTACHMENT, +TRANSFER_SRC for the dump hook)")
    e = env_for(PRESENTMON_LOG="info")
    rc, out = run_truth(["--seconds", "2", "--size", "640x360", "--fps", "60"], e)
    sw = [l for l in out.splitlines() if "[presentmon] swapchain" in l and "usage " in l]
    u = sw[-1].split("usage ")[1] if sw else ""
    sent = int(u.split(" ")[0], 16) if u else 0
    app = int(u.split("(app ")[1].rstrip(")"), 16) if u else 0
    check(sw and sent == (app | 0x10), f"on without dump: usage 0x{app:x} -> 0x{sent:x} (no TRANSFER_SRC)")


def spawn_truth(extra, env):
    return subprocess.Popen([TRUTH] + extra, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def wait_segment(pid, t=8):
    end = time.time() + t
    while time.time() < end:
        if os.path.exists(f"/dev/shm/presentmon-linux.{pid}"):
            return True
        time.sleep(0.05)
    return False


def ring_flags(pid, seconds):
    p = subprocess.run([RING, "--pid", str(pid), "--seconds", str(seconds)], capture_output=True, text=True, timeout=seconds + 10)
    rows = [l.split(",") for l in p.stdout.splitlines() if l and not l.startswith("#") and not l.startswith("swapchain")]
    total = len(rows)
    drawn = sum(1 for r in rows if "O" in r[10])
    return total, drawn


def ctl(pid, *a):
    p = subprocess.run([CTL, "--pid", str(pid)] + list(a), capture_output=True, text=True, timeout=10)
    return p.stdout.strip()


def t_toggle():
    print("runtime control through OverlayControl")
    path = os.path.join(DUMPS, "toggled.png")
    for f in glob.glob(path + "*"):
        os.remove(f)
    # The dump frame is far enough away that every control change below has happened by then.
    e = env_for(PRESENTMON_OVERLAY_DUMP=path, PRESENTMON_OVERLAY_DUMP_FRAME=780, PRESENTMON_LOG="info")
    p = spawn_truth(["--seconds", "10", "--size", "1280x720", "--fps", "120"], e)
    try:
        check(wait_segment(p.pid), "segment appears")
        time.sleep(1.2)
        st = ctl(p.pid, "status")
        check("available=1" in st and "visible=1" in st, f"overlay available and visible at start: {st}")
        total, drawn = ring_flags(p.pid, 1.0)
        check(total > 50 and drawn >= total - 3, f"visible: records carry the overlay-drawn flag ({drawn}/{total})")
        ctl(p.pid, "hide")
        time.sleep(0.6)
        total, drawn = ring_flags(p.pid, 1.0)
        check(total > 50 and drawn == 0, f"hidden: no overlay drawn ({drawn}/{total})")
        ctl(p.pid, "toggle")
        time.sleep(0.6)
        total, drawn = ring_flags(p.pid, 1.0)
        check(total > 50 and drawn >= total - 3, f"toggled back on: drawn again ({drawn}/{total})")
        ctl(p.pid, "position", "bottom-right")
        ctl(p.pid, "scale", "1.5")
        st = ctl(p.pid, "panels", "default")
        print("     ", st)
        out, _ = p.communicate(timeout=30)
    finally:
        if p.poll() is None:
            p.kill()
    check(p.returncode == 0, f"vk_truth rc=0 after toggling (rc={p.returncode})")
    if os.path.exists(path + ".raw"):
        raw = read_raw(path)
        lo, hi = expected_white(raw[0], raw[1])
        a = channels(raw)
        br = count_code(a, (1280 - 12 - 590, 280, 1268, 705), lo, hi)
        tl = count_code(a, TOP_LEFT, lo, hi)
        check(br >= 150, f"bottom-right at scale 1.5: {br} white pixels in the bottom-right region")
        check(tl <= 15, f"top-left is empty now ({tl})")
    else:
        check(False, "toggle: dump written")
    # hidden at dump time -> dump is overlay free
    path2 = os.path.join(DUMPS, "hidden_at_dump.png")
    e = env_for(PRESENTMON_OVERLAY_DUMP=path2, PRESENTMON_OVERLAY_DUMP_FRAME=300, PRESENTMON_LOG="info", PRESENTMON_OVERLAY="0")
    p = spawn_truth(["--seconds", "4", "--size", "1280x720", "--fps", "120"], e)
    try:
        wait_segment(p.pid)
        time.sleep(0.8)
        st = ctl(p.pid, "status")
        check("available=1" in st and "visible=0" in st, f"PRESENTMON_OVERLAY=0: loaded but hidden: {st}")
        total, drawn = ring_flags(p.pid, 0.8)
        check(drawn == 0, f"PRESENTMON_OVERLAY=0: nothing drawn ({drawn}/{total})")
        p.communicate(timeout=30)
    finally:
        if p.poll() is None:
            p.kill()
    check(p.returncode == 0 and os.path.exists(path2 + ".raw"), "hidden: dump still works (copy only)")


def t_resize():
    print("swapchain recreation")
    e = env_for(PRESENTMON_LOG="info", PRESENTMON_OVERLAY_DUMP=os.path.join(DUMPS, "resize.png"), PRESENTMON_OVERLAY_DUMP_FRAME=400)
    rc, out = run_truth(["--seconds", "5", "--size", "800x500", "--resize-test", "--app-timing", "--app-present-id", "--fps", "120"], e, 60)
    n = sum(1 for l in out.splitlines() if "overlay: swapchain" in l and "ready" in l)
    check(rc == 0, f"resize-test with overlay + app timing: vk_truth rc=0 (rc={rc})")
    check(n >= 5, f"overlay created on {n} swapchains")
    bad = [l for l in out.splitlines() if "[presentmon]" in l and ("warn" in l.lower() or "failed" in l.lower() or "disabled" in l.lower())]
    check(not bad, f"no layer warnings {bad[:2]}")
    leftovers = subprocess.run(["pgrep", "-x", "vk_truth"], capture_output=True, text=True).stdout.strip()
    check(not leftovers, "no vk_truth left behind")


def overhead_cpu(label, extra, fps=240):
    e = env_for(PRESENTMON_LOG="warn")
    p = spawn_truth(["--seconds", "4", "--fps", str(fps)] + extra, e)
    wait_segment(p.pid)
    r = subprocess.Popen([RING, "--pid", str(p.pid), "--oldest"], stdout=subprocess.PIPE, text=True)
    out, _ = r.communicate(timeout=30)
    vout, _ = p.communicate(timeout=30)
    size = [l for l in vout.splitlines() if l.startswith("[vk_truth] swapchain:")]
    rows = [l.split(",") for l in out.splitlines() if l and not l.startswith("#") and not l.startswith("swapchain")]
    vals = [int(r_[14]) for r_ in rows if "O" in r_[10]]
    check(len(vals) > 300, f"{label}: {len(vals)} records with overlay")
    if vals:
        v = sorted(vals)
        med = v[len(v) // 2] / 1000
        mean = statistics.mean(vals[1:]) / 1000
        p99 = v[int(len(v) * 0.99)] / 1000
        print(f"      {label} {size[-1][19:] if size else ''}: overlayCpuNs first {vals[0]/1000:.0f} us (backend creation), median {med:.0f} us, mean {mean:.0f} us, p99 {p99:.0f} us, max {v[-1]/1000:.0f} us")
        check(med < 300, f"{label}: median overlay CPU {med:.0f} us < 300 us")


def t_overhead_cpu():
    print("overlay CPU time per present (FrameRecord.overlayCpuNs)")
    overhead_cpu("1280x720 @240 fps", ["--size", "1280x720"])


def bench():
    print("frame time impact, uncapped IMMEDIATE (vk_truth --fps 0), 3 s per run")
    configs = {
        "layer off": env_for(layer=False),
        "overlay hidden (PRESENTMON_OVERLAY=0)": env_for(PRESENTMON_OVERLAY="0"),
        "overlay off (PRESENTMON_OVERLAY=off)": env_for(PRESENTMON_OVERLAY="off"),
        "overlay visible": env_for(),
    }
    overhead_cpu("2560x1440 @240 fps", ["--size", "2560x1440"])
    for size in ("1280x720", "2560x1440"):
        res = {k: [] for k in configs}
        for rep in range(3):
            for name, e in configs.items():
                rc, out = run_truth(["--seconds", "3", "--size", size, "--fps", "0", "--present-mode", "immediate"], e)
                swap = [l for l in out.splitlines() if l.startswith("[vk_truth] swapchain:")]
                for l in out.splitlines():
                    if "present interval: avg" in l:
                        ms = float(l.split("avg ")[1].split(" ms")[0])
                        res[name].append(ms)
        print(f"  size requested {size} ({swap[-1] if swap else '?'})")
        base = statistics.mean(res["layer off"])
        for name, v in res.items():
            m = statistics.mean(v)
            print(f"    {name:42s} avg frame {m*1000:8.1f} us  ({1000/m:8.0f} fps)  delta vs layer off {(m-base)*1000:+8.1f} us   runs {[round(x*1000,1) for x in v]}")


tests = [t_formats, t_present_modes, t_xwayland, t_off, t_toggle, t_resize, t_overhead_cpu]
for t in tests:
    if args.only and args.only not in t.__name__:
        continue
    try:
        t()
    except Exception as ex:  # noqa
        import traceback
        traceback.print_exc()
        check(False, f"{t.__name__} raised {ex!r}")
if args.bench:
    bench()
print(f"\n{sum(1 for ok, _ in results if ok)}/{len(results)} checks passed")
sys.exit(1 if failures else 0)
