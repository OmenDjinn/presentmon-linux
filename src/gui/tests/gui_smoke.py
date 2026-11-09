#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# pml_gui_smoke: runs the real presentmon-gui headless (QT_QPA_PLATFORM=offscreen, so no window
# ever opens) on every page, with a fake layer process and a capture to show, saves a screenshot
# each time and fails on any unexpected stderr line (QML or Qt warnings). HOME is a temporary directory and PML_GUI_TEST
# is set, so the user's settings, trash and global shortcuts are never touched.
#
#   gui_smoke.py --build-dir <build> [--keep <dir>]   (--keep: write the screenshots there)
# Exit 0 = passed, 77 = skipped (app not built), 1 = failed.
import argparse
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
GOLD = os.path.join(REPO, "third_party", "PresentMon", "Tests", "Gold", "test_case_0.csv")
GOLD_V2 = os.path.join(REPO, "third_party", "PresentMon", "Tests", "Gold", "test_case_0_v2.csv")

# Every line the app prints on stderr fails the test, except these: harmless and not this app's to fix.
KNOWN = [
    re.compile(r"^\[presentmon-gui\] screenshot saved"),   # our own test hook
    # The user's icon theme from the real session, absent under the temporary HOME.
    re.compile(r'^kf\.iconthemes: Icon theme ".*" not found\.$'),
    # Printed when the Settings page loads (Qt 6.12, KF 6.31); not from this app's code.
    re.compile(r"^QObject::installEventFilter\(\): Cannot filter events for objects in a different thread\.$"),
]


def png_info(path):
    """(width, height, distinct colours in a sample of rows) of an 8-bit RGB/RGBA PNG, stdlib only."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, w, h, ctype = 8, b"", 0, 0, 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            if depth != 8 or ctype not in (2, 6):
                return w, h, -1
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    bpp = 4 if ctype == 6 else 3
    stride = w * bpp
    colours, prev = set(), bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):   # undo the PNG filter
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        if y % 8 == 0:
            for x in range(0, stride, bpp * 4):
                colours.add(bytes(line[x:x + 3]))
        prev = line
    return w, h, len(colours)


def run_gui(gui, home, shot, args, timeout=60):
    # QT_FORCE_STDERR_LOGGING: under a systemd session (JOURNAL_STREAM set) Qt sends its messages to the
    # journal, and the warning check below would see nothing.
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", PML_GUI_TEST="1", HOME=home,
               PML_GUI_APP_ID="presentmon-gui-test", QT_FORCE_STDERR_LOGGING="1")
    for var in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"):
        env.pop(var, None)
    cmd = [gui, "--screenshot", shot, "--screenshot-delay", "2500"] + args
    p = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--keep")
    a = ap.parse_args()
    gui = os.path.join(a.build_dir, "bin", "presentmon-gui")
    producer = os.path.join(a.build_dir, "bin", "pml_fake_producer")
    if not os.access(gui, os.X_OK):
        print("skip: %s not built" % gui)
        return 77

    work = tempfile.mkdtemp(prefix="pml_gui_smoke-")
    home = os.path.join(work, "home")
    os.makedirs(home)
    out = a.keep or work
    os.makedirs(out, exist_ok=True)
    failures = []
    fake = None
    try:
        cases = [("live-empty", ["--page", "live"]),
                 ("captures", ["--page", "captures", "--capture", GOLD]),
                 ("compare", ["--page", "captures", "--capture", GOLD, "--capture", GOLD_V2]),
                 ("compare-narrow", ["--page", "captures", "--capture", GOLD, "--capture", GOLD_V2, "--size", "760x560"]),
                 ("settings", ["--page", "settings"]),
                 ("settings-wide", ["--page", "settings", "--size", "1700x1050"])]
        if os.access(producer, os.X_OK):
            # A fake layer process (this test's child) with a few seconds of frames, shown on the Live page.
            fake = subprocess.Popen([producer, "--frames", "3000", "--period-us", "8333", "--linger-ms", "30000",
                                     "--process-name", "smoke_game.exe"],
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, start_new_session=True)
            line = fake.stdout.readline()
            if not line.startswith("published"):
                failures.append("fake producer did not start: %r" % line)
            else:
                cases.insert(0, ("live", ["--page", "live", "--pid", str(fake.pid)]))
                cases.insert(1, ("live-narrow", ["--page", "live", "--pid", str(fake.pid), "--size", "760x560"]))
        else:
            print("note: %s not built, Live page shown without a process" % producer)

        for name, args in cases:
            shot = os.path.join(out, "gui-%s.png" % name)
            code, log = run_gui(gui, home, shot, args)
            bad = [l for l in log.splitlines() if l.strip() and not any(k.search(l) for k in KNOWN)]
            info = "?"
            if code == 0 and os.path.exists(shot):
                w, h, colours = png_info(shot)
                info = "%dx%d, %d colours" % (w, h, colours)
                if w < 300 or h < 200 or 0 <= colours < 8:
                    failures.append("%s: screenshot looks empty (%s)" % (name, info))
            else:
                failures.append("%s: exit %s, no screenshot\n%s" % (name, code, log[-2000:]))
            if bad:
                failures.append("%s: unexpected output (QML or Qt warnings):\n  %s" % (name, "\n  ".join(bad[:20])))
            print("%-15s exit %s  %s  %s" % (name, code, info, "%d warnings" % len(bad) if bad else "clean"))
    finally:
        if fake and fake.poll() is None:
            os.killpg(fake.pid, signal.SIGTERM)
            try:
                fake.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(fake.pid, signal.SIGKILL)
        if not a.keep:
            shutil.rmtree(work, ignore_errors=True)
        else:
            shutil.rmtree(home, ignore_errors=True)

    for f in failures:
        print("FAIL: " + f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
