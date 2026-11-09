# presentmon-linux

A Linux port of [Intel PresentMon](https://github.com/GameTechDev/PresentMon)'s frame-timing metrics, with
PresentMon-compatible CSV output and a PresentMon-style in-game overlay. It targets Vulkan (native games, and
DXVK/VKD3D under Proton) on Wayland, using `VK_EXT_present_timing`.

How it works: an implicit Vulkan layer (`VkLayer_presentmon`) records one entry per present into a
shared-memory ring (`/dev/shm/presentmon-linux.<pid>`), draws the overlay into the presented image, and the
`presentmon` CLI reads the rings and runs Intel's own metrics code (`third_party/PresentMon`, compiled
unmodified) to produce the CSV. Design: `docs/ARCHITECTURE.md`. What we learned about frame timing on Linux (present timing,
DXVK and vkd3d-proton pacing, frame caps, VRR, scoring): `docs/FINDINGS.md`. Working on the code with Claude Code:
the project skill in `.claude/skills/presentmon-linux/`.

Status: M1 (CLI + overlay) and M2's GPU columns (GPU busy / wait / latency / time, from GPU timestamps the layer
adds around each queue submission; `PRESENTMON_GPU=0` turns them off). Input/click-to-photon columns are NA.
Tested under Proton with D3D11 and D3D12 test programs (GE-Proton 11 with DXVK 3.1.1, DXVK 2.5.1, and
proton-wineland's vkd3d-proton), and live in Lies of P (vkd3d-proton). Also: a frame rate cap that waits where the
game is paced (`presentmon cap`), and two 0-100 scores, VRR stability and an experience score.

Credits: built on [Intel PresentMon](https://github.com/GameTechDev/PresentMon) (its metrics code runs unmodified),
[PyroFling](https://github.com/Themaister/pyrofling) by Hans-Kristian Arntzen (the present-timing
measurement approach the layer's timing is based on), and [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut (overlay). MIT licensed:
`LICENSE`; third-party licenses: `THIRD_PARTY_NOTICES.md`. Not affiliated with Intel.

## Build

    git submodule update --init
    cmake -S . -B build/all -G Ninja -DPML_BUILD_METRICS=ON -DPML_BUILD_LAYER=ON -DPML_BUILD_OVERLAY=ON \
          -DPML_BUILD_COLLECTOR=ON -DPML_BUILD_GUI=ON -DPML_BUILD_TESTS=ON -DPML_BUILD_TEST_APPS=ON -DPML_BUILD_E2E=ON
    ninja -C build/all

Needs GCC (C++23), CMake, Ninja, glslangValidator, SDL3 (test app), GTest; for the GUI Qt 6.5+ (Quick, Quick
Controls 2, Widgets, Concurrent), KF6 Kirigami and KGlobalAccel, and at run time kirigami-addons and
qqc2-desktop-style (all part of Plasma 6; `-DPML_BUILD_GUI=OFF` skips it); optionally mingw-w64
(`x86_64-w64-mingw32-g++`) for the D3D11 and D3D12 test programs of the Proton test. Outputs: `build/all/bin/presentmon`,
`build/all/bin/presentmon-gui`, `build/all/layer/libVkLayer_presentmon.so` + `VkLayer_presentmon.json`,
`build/all/bin/vk_truth`, `build/all/bin/d3d11_truth.exe` and `build/all/bin/d3d12_truth.exe` (test apps). `cmake --install build/all --prefix ~/.local`
installs `presentmon`, `presentmon-gui` with its menu entry and icon, the layer and its manifest
(`~/.local/share/vulkan/implicit_layer.d`, found by the Vulkan loader without `VK_ADD_IMPLICIT_LAYER_PATH`; still
inert unless `PRESENTMON_LAYER=1`).

## Test

    ctest --test-dir build/all -LE live -j8   # no windows: unit, CSV gold, CLI, layer and GUI tests (~45 s)
    ctest --test-dir build/all -L live        # opens windows: layer smoke, overlay, e2e suite (~5 min)

The live tests open small windows and measure timing (ctest runs them one at a time):

- The screens must be awake. With a monitor in DPMS Off, KWin throttles windows and reports no display times, so
  the live tests skip; `PML_E2E_WAKE=1` lets them run `kscreen-doctor --dpms on` first.
- `PML_DESKTOP_LOCK=<file>` makes every e2e scenario hold an flock while it runs, for several test drivers on one
  desktop.
- `e2e.proton` runs a D3D11 test program through Proton and DXVK. It needs `x86_64-w64-mingw32-g++` at configure
  time (for `bin/d3d11_truth.exe`) and a GE-Proton with DXVK 3.x in `~/.local/share/Steam/compatibilitytools.d`
  (or `PML_PROTON=<dir>`); a Proton 10 there also gives it DXVK 2.x (or `PML_DXVK2_DIR=<dir with the dlls>`).
  Steam does not need to run. It skips when these are missing.

### Testing next to a game

To keep using the PC (or playing on the main screen) while the live tests run:

1. A KWin window rule (System Settings > Window Management > Window Rules) for the test programs: window class
   (regular expression) `^(vk_truth|vkcube|d3d11_truth[.]exe)$`, Position forced onto a spare screen (without it
   a test window opens where the cursor is), Keep below forced to Yes, Accept focus forced to No, Focus stealing
   prevention forced to Extreme. Test windows then open on that screen under your windows and never take focus;
   fullscreen tests still go fullscreen on the screen they ask for. Covered windows still present at full rate,
   but on NVIDIA their display times can be up to ~0.35 ms off the vblank grid (docs/ARCHITECTURE.md, Risks), which
   is too much for `e2e.csv_truth`'s refresh-multiple check, so keep the spare screen's test area free.
2. `PML_E2E_SHARED_DESKTOP=1`, which skips the tests that would disturb the game: fullscreen and VRR, GPU-bound
   loads, overhead benchmarks, the uncapped 19000 fps run, and the Proton test's Xwayland run (Proton names that
   window `steam_proton`, which the rule does not match).

        PML_E2E_SHARED_DESKTOP=1 nice -n 15 ctest --test-dir build/all -L live

Timing is a little noisier while the game shares the GPU; the assertions allow for that.

VRR needs the opposite. KWin (VRR policy Automatic) runs an output in VRR only while the active window is
fullscreen on it, so a test window without focus never gets VRR, and any click on another window switches VRR off
in the middle of a run. The VRR check in `e2e.fullscreen` reads the output's real VRR state (the CRTC's
`VRR_ENABLED`, through libdrm, read-only) and judges each frame against it (except frames right at a switch:
within 40 ms of one, or less than 120 ms after VRR turns on), so it passes either way and prints
what happened ("VRR off (VRR_ENABLED stayed 0)" under such a rule). To see VRR in it, run `e2e.fullscreen` without
the rule and leave the PC alone for the 15 s it takes.

## Run

    # Terminal 1: capture to CSV for the named process, stop 30 s after starting or when the game exits
    build/all/bin/presentmon --process_name vk_truth --output_file out.csv --timed 30 --terminate_on_proc_exit

    # Terminal 2: any Vulkan app with the layer enabled (build-tree layer shown)
    VK_ADD_IMPLICIT_LAYER_PATH=$PWD/build/all/layer PRESENTMON_LAYER=1 build/all/bin/vk_truth --seconds 10

The layer is inert unless `PRESENTMON_LAYER=1` is set (`DISABLE_PRESENTMON_LAYER=1` forces it off): the loader
does not even load the library otherwise. Nothing is installed by the build; after `cmake --install` (see Build) the
installed manifest is found without `VK_ADD_IMPLICIT_LAYER_PATH`, and a build-tree layer added with that variable
takes precedence over it (same layer name, the added path is searched first).

### Steam / Proton

Wayland is required for display-time metrics (Xwayland only reports queue-end times, so display columns and
overlay display items show NA). Launch options:

    PRESENTMON_LAYER=1 PROTON_ENABLE_WAYLAND=1 %command%

for GE-Proton / CachyOS Proton, or use `proton-wineland`. The layer must be installed for Steam's process to
find it (`cmake --install ... --prefix ~/.local`, or set `VK_ADD_IMPLICIT_LAYER_PATH` to the layer directory).
Verified: GE-Proton 11 run directly finds the installed manifest (capture and overlay work). Not tested yet: games
Steam runs inside the Steam Linux Runtime container (pressure-vessel, e.g. `proton-cachyos-*-slr`), which has to
pick the layer up from the host.

## GUI

`presentmon-gui` (in the KDE menu as PresentMon after `cmake --install`) works on the same data as the CLI and the
overlay, in-process: it reads the same shared-memory segments, and its live numbers come from the overlay's own
statistics code.

- **Live**: the running layer-enabled processes with their present rate. For the selected one: frame rate (presented
  and displayed), frame time and its 99th percentile, 1% and 0.1% lows, display latency, CPU busy/wait and dropped
  frames (the in-game overlay's numbers), a scrolling frame-time chart (5-60 s, with the refresh interval as a
  reference line), recording, and the in-game overlay's controls: shown/hidden, corner, size, panels (the same control
  block as `presentmon overlay ...`).
- **Recording** runs the CLI (`presentmon --process_id <pid> --output_file <file> ...`), so the files are exactly the
  CLI's: `<Documents>/PresentMon/pmcap-<exe>-YYMMDD-HHMMSS.csv` (folder, length, PresentMon 1.x/2.x columns and
  stop-when-the-game-exits in Settings). The CLI must be next to the GUI or in `PATH`.
- **Global shortcuts** (KDE, work while a game has focus): Meta+F11 starts/stops recording, Meta+F10 shows/hides the
  overlay, for the process selected on the Live page or else the one presenting the most frames. Change them in
  System Settings > Keyboard > Shortcuts > PresentMon, or turn them off in Settings.
- **Captures**: the CSVs in the capture folder. One selected: average fps, 1% / 0.1% lows, 95th/99th percentile,
  slowest frame, displayed fps and dropped frames, frame times over the whole capture and their distribution. Tick
  several to compare them against the first (changes in percent, green where better). Reads every PresentMon CSV
  layout (ours, Intel's 1.x and 2.x); a file with several processes or swapchains shows the one with the most frames.
  The numbers use the overlay's definitions (`pml/metrics/summary.hpp`): lows are the frame rate of the mean of the
  slowest 1% / 0.1% of the frames, percentiles are nearest-rank.
- **Settings**: the above, whether the Vulkan layer is installed where the loader finds it, and launch options to copy.

Options: `--page live|captures|settings`, `--pid <pid>`, `--capture <file.csv>` (repeat to compare), and for tests
`--screenshot <file.png>` (save the window after `--screenshot-delay` ms and quit). `PML_GUI_TEST=1` never registers
global shortcuts and keeps settings out of `~/.config`; `PML_GUI_APP_ID` sets the Wayland app id (test runs use
`presentmon-gui-test`, see "Testing next to a game").

## CLI

    presentmon [capture] [options]       PresentMon-style flags: --process_name, --exclude, --process_id,
                                         --output_file, --output_stdout, --no_csv, --multi_csv, --v1_metrics,
                                         --v2_metrics, --qpc_time, --timed S, --delay S, --terminate_after_timed,
                                         --terminate_on_proc_exit, --no_console_stats, ...
    presentmon list [--process_name N] [--process_id P]
    presentmon overlay show|hide|toggle|status [--process_name N] [--process_id P]
                       [--position top-left|top-right|bottom-left|bottom-right] [--scale F|auto]
    presentmon cap <fps>|auto|off|status [--process_name N] [--process_id P] [--placement late|early]
                   [--max F] [--reflex|--no-reflex]          (see "Frame cap" below)

Full flag table: `src/collector/README.md`. Windows-only flags (ETW, hotkeys) are rejected with a message. In v2
mode the last displayed frame is not written (as in PresentMon). `Application` is the process name (the exe name
under Wine), `PresentRuntime` is Vulkan/DXVK/VKD3D, `PresentMode` is the Vulkan mode.

- `list` shows live layer-enabled processes; ones that are gone are only counted. Their segments are removed by
  the next capture or by the next layer-enabled game that starts.
- `--delay`/`--timed` select frames by their present time; a timed capture closes 250 ms after its end so frames
  presented just before the end still land.
- Lost records (ring overwritten before they were read) are reported as a warning and in the final summary.
- Ctrl+C, SIGTERM, SIGHUP (terminal closed) and SIGQUIT flush and close the CSV. A signal the parent left
  ignored stays ignored: `nohup presentmon ...` survives a closed terminal, and a capture a script started with
  `&` ignores SIGINT (non-interactive shells start background jobs that way), so stop it with SIGTERM.
- Under DXVK, `MsInPresentAPI`, `MsCPUBusy` and `MsCPUWait` describe DXVK's presenter thread, not the game thread,
  and when the game is vsync-bound one latency column is about one refresh off: `MsUntilDisplayed` reads short
  with DXVK 3.x (it presents each frame about one refresh after the game's `Present()`), `DisplayLatency` reads
  long with DXVK 2.x. Frame times and display times are unaffected. Details: `docs/ARCHITECTURE.md`, Risks.

## Overlay

Environment (set on the game):

| Variable | Meaning |
|---|---|
| `PRESENTMON_OVERLAY=1` / `0` / `off` | visible (default) / loaded but hidden / not loaded. Only `off` leaves the swapchain exactly as the game created it; `0` still adds colour-attachment usage so the overlay can be shown later |
| `PRESENTMON_OVERLAY_POSITION` | `top-left`, `top-right`, `bottom-left`, `bottom-right` |
| `PRESENTMON_OVERLAY_SCALE` | float; default automatic (shorter edge/1440, clamped 0.75-3) |
| `PRESENTMON_OVERLAY_DUMP=<path.png>` + `_DUMP_FRAME=<n>` | write one frame with the overlay, for testing |
| `PRESENTMON_LOG=error\|warn\|info\|debug` | layer log to stderr, prefix `[presentmon] ` |

Runtime control from another terminal: `presentmon overlay toggle` (or show/hide/status, `--position`, `--scale`).

Two scores on the overlay and in the GUI, both out of 100 with a verdict and a main cause: **VRR** (how evenly frames
reach a VRR screen) and **Exp** (experience: frame rate shown, judder, stutters and latency together; how it is built
and validated: `docs/ARCHITECTURE.md` 3.4.3). The FPS section also shows Blackbird PC Tech's GQI next to the lows.

### Global shortcut in KDE (instructions, not configured)

System Settings > Keyboard > Shortcuts > Add New > Command or Script. Enter the command
`/home/<you>/.local/bin/presentmon overlay toggle` (after `cmake --install ... --prefix ~/.local`; or the build tree's
`bin/presentmon`), then assign the key (for example Meta+F10). Works while a game is focused since KDE handles the
key; with several layer-enabled processes it toggles all of them, add `--process_name <exe>` to target one.

## Frame cap

A frame rate limiter inside the layer (design and measurements: `docs/ARCHITECTURE.md`, 3.4.2). Set on the game:

| Variable | Meaning |
|---|---|
| `PRESENTMON_FPS_CAP=<fps>` / `auto` / `off` | cap at that rate (1-10000, e.g. `141` or `59.94`); `auto`: just under the VRR maximum of the output with VRR on (223 at 239.99 Hz), nothing while VRR is off |
| `PRESENTMON_CAP_PLACEMENT=late` / `early` | `late` (default): lowest latency; `early`: waits before the present, most even spacing, about a frame more latency |
| `PRESENTMON_CAP_REFLEX=0` | never use the game's Reflex (NV_low_latency2) interval |

Live, from another terminal (or the GUI's Live page, "Frame cap"):

    presentmon cap 141 [--process_name N] [--process_id P] [--placement late|early] [--no-reflex|--reflex]
    presentmon cap auto [--max 200]
    presentmon cap off
    presentmon cap status

Where the layer waits, by what the game offers: through the game's Reflex interval (Reflex games); on DXVK's or
vkd3d-proton's frame-pacing thread, as their own limiters do (DXVK with V-Sync off, vkd3d-proton with V-Sync on);
else after the present, like MangoHud. Measured at 100 fps: as fast as the game's own cap in native Vulkan games, in
DXVK games with V-Sync off (about 10 ms against MangoHud's 18-21) and vkd3d-proton games with V-Sync on (8-9
against 28-29);
in DXVK games with V-Sync on the game's own limiter or `DXVK_FRAME_RATE` is a frame faster than both. The overlay's
"Cap" line and `presentmon cap status` show the rate and method in force.
