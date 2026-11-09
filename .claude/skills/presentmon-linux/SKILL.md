---
name: presentmon-linux
description: Work on presentmon-linux (Vulkan layer, presentmon CLI, overlay, Qt GUI, frame cap, VRR and experience scores). Build it, run the unit and live tests, record measurement scenarios with the vk_truth / d3d11_truth / d3d12_truth test apps, debug empty or wrong columns in a game, and avoid the known pitfalls of present timing, DXVK, vkd3d-proton and KWin. Use it for any change, bug or measurement in this repository, even if the request only names a symptom ("display latency shows NA", "the cap adds latency", "VRR score is low").
---

# presentmon-linux

What the project is: README.md. Design: docs/ARCHITECTURE.md. Hard-won facts: docs/FINDINGS.md. **Read FINDINGS.md
before changing timing, IDs, GPU timing, the frame cap or a score.** Most of its items look like bugs in this code
until you know them.

## Map

| Path | What |
|---|---|
| `src/layer/` | Implicit Vulkan layer: `layer.cpp` (hooks, present path, frame cap plan/holds, NV_low_latency2), `timing_broker.*` (shares VK_EXT_present_timing and present IDs with the app), `gpu_timer.*` (GPU timestamps), `frame_limiter.*` / `frame_cap.*` (cap), `process_segment.*` (shm ring) |
| `src/layer/overlay/` | Dear ImGui overlay: `stats.*` (rolling windows), `ui.cpp` (panels) |
| `src/metrics/` | Intel's metrics code adapter (`engine`, `adapter`), `summary` (avg/percentiles/lows/GQI), `vrr`, `experience` |
| `src/common/` | `protocol.hpp` (shm layout, FrameRecord, Overlay/CapControl), `shm.*`, `drm_outputs.*` |
| `src/collector/` | `presentmon` CLI: capture (CSV), `list`, `overlay`, `cap` |
| `src/gui/` | Qt 6 / Kirigami app: `backend/` (ProcessModel, LiveSession, CaptureAnalysis, csvreader), `qml/` |
| `tests/apps/` | `vk_truth` (Vulkan, known timing, logs its own timestamps), `d3d11_truth.exe`, `d3d12_truth.exe` (mingw, run under Proton) |
| `tests/e2e/` | Python live tests (`pmltest.py` helpers, `test_*.py`, run by ctest label `live`) |
| `third_party/` | Submodules: PresentMon (metrics, unmodified), pyrofling (Granite: Vulkan headers, stb), imgui. Keep their licenses and the credits in THIRD_PARTY_NOTICES.md |

## Build

```bash
git submodule update --init
git -C third_party/pyrofling submodule update --init --recursive Granite
cmake -S . -B build/all -G Ninja
ninja -C build/all
```

C++23 (GCC 16 needs it for PresentMon's code). The GUI needs Qt 6.5+ and KF6 Kirigami (`-DPML_BUILD_GUI=OFF`
skips it). `x86_64-w64-mingw32-g++` at configure time builds the Proton test programs. Never commit `build*/`.

## Test

```bash
ctest --test-dir build/all -LE live -j8      # unit, CSV gold files, CLI, layer, GUI smoke: ~45 s, no windows
ctest --test-dir build/all -L live           # opens windows, measures timing: ~5+ min, one at a time
```

Run one e2e module or test directly (faster to iterate):
```bash
cd tests/e2e && PML_BUILD_DIR=$PWD/../../build/all python3 runtests.py test_cap.Cap
```

- The live tests need the screens awake (DPMS off: KWin reports no display times; `PML_E2E_WAKE=1` wakes them).
- `PML_E2E_SHARED_DESKTOP=1` skips the tests that disturb a desktop in use (fullscreen, VRR, GPU-bound, overhead,
  comparisons).
- `PML_E2E_KEEP=<dir>` keeps each run's files: the CSVs, `truth.csv`, the ring dump and the Proton/DXVK logs. Use it
  whenever a run surprises you.
- Proton tests need a GE-Proton with DXVK 3.x (`PML_PROTON`, `PML_PROTON_ROOTS`, `PML_PROTON_D3D12`). Steam does
  not need to run.
- Ask the user before running live tests on their desktop: they open windows and, for VRR tests, take a screen
  fullscreen. Give test windows a window rule (classes `vk_truth`, `d3d11_truth.exe`, `steam_proton`, and the
  `SDL_APP_ID` you set) so they don't land on the user's main screen.

## Use it on a game

```bash
PRESENTMON_LAYER=1 %command%                              # Steam launch options (layer installed in ~/.local)
presentmon --process_name Game.exe --output_file run.csv --qpc_time --timed 30 --terminate_after_timed
presentmon list            # live processes, overlay and cap state, swapchains
presentmon cap 141         # also: auto, off, status, --placement late|early
PRESENTMON_LOG=info        # layer log on stderr, one line per decision ("frame cap: ... via ...")
build/all/bin/ring_dump --pid N --info   # raw records: present_id, flags, screen_ns, cap_wait_ns
```

Install for real games: `cmake --install build/all --prefix ~/.local`. **If a game is running with the layer,
move the old `.so` aside first instead of overwriting it** (`mv lib.so lib.so.old`, install, then delete the
`.old`). A running game keeps the old layer until it restarts.

## Record a measurement scenario

1. Pick a cause you know. `vk_truth` options:
   - Load: `--fps N` (its own cap), `--cpu-ms`, `--gpu-ms`, `--stutter-every N --stutter-ms M`, `--jitter-ms J`.
   - Present: `--present-mode fifo|mailbox|immediate`.
   - Window: `--fullscreen --display D`.
   - DXVK-like behaviour: `--ll2` (Reflex), `--app-present-id`, `--app-timing`.
   - `--log truth.csv` writes its own timestamps for every frame.
2. Run it through `pmltest.run_scenario(...)` (CSV and truth come back matched), or copy the pattern in
   `tests/e2e/test_cap.py`. Under Proton use `test_proton.run_proton(...)` and `Aligned`: the k-th present record is
   the game's k-th `Present()`.
3. Compare **medians of 3 runs**: latency on a composited fixed-refresh window varies by up to ~12 ms between
   runs.
4. VRR: fullscreen and focused on the VRR screen only. Watch the CRTC's `VRR_ENABLED` with `pmltest.VrrWatch` and
   judge only frames while it was 1. KWin turns VRR off as soon as another window is active.
5. Comparisons with MangoHud: `MANGOHUD=1 MANGOHUD_CONFIG=no_display,fps_limit=N,fps_limit_method=late`.

## Pitfalls

| Symptom | Cause | Fix / check |
|---|---|---|
| Display columns NA in one game | No present IDs on its presents (vkd3d-proton with V-Sync off), or Xwayland, or screens asleep | `ring_dump --info`: present_id, timing_flags; Wine Wayland; FINDINGS 1-2 |
| GPU start one frame early | Calibration skew between GPU timestamps and present timing | GPU-timeline attribution with slack (FINDINGS 3) |
| CPU columns odd under DXVK | They describe DXVK's presenter thread | Expected (FINDINGS 4) |
| A cap adds a frame of latency under DXVK/vkd3d | Wait sits on the submit thread | Frame-thread holds; DXVK + V-Sync on can't be reached (FINDINGS 5-6) |
| Cap "holds" but the game runs fast | Hold engaged on a non-pacing thread, or a backstop wait became the pacer | Steady-pattern detection, average-interval check (FINDINGS 6) |
| VRR score drops mid-run | KWin switched VRR off (focus moved), or another screen renders animations | `VRR_ENABLED` timeline; FINDINGS 7 |
| A metric looks great on a broken run | It ignores screen times (GQI) or one kind of stutter | Experience score uses screen intervals and animation error (FINDINGS 8) |
| Proton test window on the wrong screen | Class `steam_proton` not covered by a window rule | Add a rule for it during the run |
| GitHub reports the license as NOASSERTION | Extra text in LICENSE | Keep LICENSE the plain MIT text |

## Before you finish a change

- `ctest -LE live` passes. Run the live tests that cover what you touched.
- If a number or behaviour in docs/ARCHITECTURE.md or docs/FINDINGS.md changed, update it.
- New metric or threshold: say where it comes from (study, standard, measurement) and what is a design choice.
