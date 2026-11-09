# presentmon-linux: architecture proposal

Status: **M0/M1 in progress on branch `m1-cli-overlay`** (CLI collector + in-game overlay first; a separate GUI app is not planned for now). Upstream pins: PresentMon `362f2d93`, pyrofling `acde35d6`
(Granite with Vulkan headers v1.4.363; the ffmpeg submodule is not initialised), Dear ImGui `v1.93.0` (overlay).

## 1. What this machine gives us (checked 2026-11-08)

| Item | Value |
|---|---|
| OS / kernel | CachyOS, 7.3.1-1-cachyos |
| GPU / driver | RTX 5070 Ti, NVIDIA open kernel module 615.78.08, Vulkan 1.4.351 |
| Desktop | Plasma / KWin 6.8.2, Wayland |
| DP-2 (primary) | 2560x1440 @ 240 Hz, VRR Automatic, **HDR on** |
| DP-1 | 2560x1440 @ 120 Hz rotated (portrait, 1440x2560 logical), VRR Never, HDR off |
| `/dev/uinput` | already rw for `taha` through an ACL, so no udev rule or `usermod` is needed |
| `/dev/input/event*` (real input) | not readable (user is not in `input`) |
| Proton | GE-Proton11-11, proton-cachyos-11.0, **proton-wineland-11.0**, UMU-Proton-10.0-4 |
| Tooling present | cmake, ninja, glslang, spirv-tools, gtest, mingw-w64-gcc, lib32 Vulkan loader + NVIDIA, python-evdev |
| Missing | `wayland-protocols` (not needed by the layer; only needed if we build a Wayland test app without SDL) |

`VK_EXT_present_timing` (rev 3), `VK_KHR_present_id2`/`present_wait2` and `VK_KHR_calibrated_timestamps` are all exposed.
**The present stages you can query depend on the surface:**

| Surface | Stages | Absolute/relative target time |
|---|---|---|
| `VK_KHR_wayland_surface` | `QUEUE_OPERATIONS_END` + **`IMAGE_FIRST_PIXEL_OUT`** | no |
| xcb/xlib (Xwayland) | `QUEUE_OPERATIONS_END` only | no |
| `VK_KHR_display` | `QUEUE_OPERATIONS_END` + `IMAGE_FIRST_PIXEL_OUT` | no |

Consequence: **only native-Wayland swapchains get a real screen time.** Proton games must use Wine's
Wayland driver (proton-wineland, or `PROTON_ENABLE_WAYLAND=1` on GE/cachyos). Under Xwayland we fall back
to `QUEUE_OPERATIONS_END` and leave every display column `NA`, rather than inventing numbers.

## 2. What we reuse, what we don't

**PresentMon** (MIT):
- **Reuse** the unified metrics library `IntelPresentMon/CommonUtilities/mc/`. Both the console app
  and the service use it. Entry point: `UnifiedSwapChain::ProcessPresent`. Input: `FrameData`. Output:
  `FrameMetrics`. It includes no Windows code itself. Each `.cpp` pulls in
  `PresentMonTraceConsumer.hpp` (which drags in `windows.h`/ETW), but only `MetricsTypes.cpp`
  (`CopyFrameData`) actually uses it. We compile the `mc/*.cpp` files unmodified against a small shim include
  dir (`src/metrics/shim/`: an empty `PresentMonTraceConsumer.hpp`, and a `qpc.h` that forwards to `Qpc.h` for
  Linux's case-sensitive file system) and leave `MetricsTypes.cpp` out. No patches to `third_party/`.
- **Reuse** `QpcConverter`: only 4 small methods from `Qpc.cpp`.
- **Port, don't compile:** `PresentMon/CsvOutput.cpp`. It relies on MSVC wide-printf, `SYSTEMTIME` and
  `HANDLE`. Keep exact column names, order and number formatting for the default, `--v1_metrics` and `--v2_metrics` sets.
- **Port** the per-present loop of `OutputThread.cpp` (`:403-560`) and nothing else from it.
- **Drop** all ETW collection (`PresentData/`), the service, PresentMonAPI2 and the Windows overlay.

**pyrofling** (MIT):
- **Reuse** `layer-util/` (dispatch tables, chain helpers). It needs only Vulkan headers, so we
  do **not** build Granite, pyro-client, ipc or the virtual gamepad.
- **Reuse** from `latency_measurement_layer.cpp`:
  - device/swapchain setup for present_timing + present_id2 (`CreateDevice`, `CreateSwapchainKHR`)
  - `GetPastPresentationTimingEXT` polling and the calibrated-timestamp conversion to `CLOCK_MONOTONIC` (`updateFeedback`)
  - the uinput fake mouse
  - optionally, the MSE change detector, for click-to-photon
- **Don't copy its invasive behaviour.** It is a fine latency probe, but as a PresentMon data source it
  would change what it measures:
  - It **hides** `VK_KHR_present_id`, `VK_KHR_present_wait`, `VK_EXT_present_timing` and
    `VK_GOOGLE_display_timing` from the app. DXVK 3.1 uses present_wait/wait2 and present_timing for
    frame pacing, so hiding them changes the pacing we are trying to measure.
  - It filters surface formats down to 8-bit/10-bit. DP-2 runs HDR, so this would remove the FP16 scRGB formats games pick.
  - Every frame, it reads back pixels, draws a debug overlay into the game's image, wraps each present in two extra
    `vkQueueSubmit`s, and uses `vkQueueWaitIdle` when there are no semaphores.

## 3. Components

```
 game (native Vulkan / DXVK in Proton)
   │  Vulkan calls
   ▼
 VkLayer_presentmon (implicit, opt-in by env)      ── writes POD records ──▶  /dev/shm/presentmon.<pid>
   • present hooks, present_timing feedback                                   (lock-free SPSC ring,
   • GPU timestamps around submits                                             never blocks the game)
   • NV_low_latency2 marker taps                                                       │
   • [optional] synthetic-input click-to-photon                                        ▼
                                                                    presentmon (collector CLI)
                                                                     • discovers rings (inotify)
                                                                     • record → FrameData
                                                                     • PresentMon mc/ UnifiedSwapChain
                                                                     • PresentMon-format CSV / console
```

### 3.1 `VkLayer_presentmon` (`src/layer/`)
- Implicit layer installed to `~/.local/share/vulkan/implicit_layer.d`.
  - Enable env: `PRESENTMON_LAYER=1`. Disable env: `DISABLE_PRESENTMON_LAYER=1`.
  - Tests load it from the build tree with `VK_ADD_IMPLICIT_LAYER_PATH`, so they don't depend on the installed copy.
  - Ship both a 64-bit and a 32-bit build, for 32-bit Proton games (M3).
- **Non-invasive rules:**
  - Hide nothing from the app and leave surface formats alone.
  - Add no work to the present queue except tiny timestamp writes, **plus the overlay draw when the overlay is
    visible** (`PRESENTMON_OVERLAY=off` turns it off completely; then the swapchain is not touched either).
- **Hooks:**

| Hook | Records |
|---|---|
| `vkQueuePresentKHR` | `CLOCK_MONOTONIC` at entry and exit (`presentStartTime`, `timeInPresent`). Attaches a `VkPresentTimingsInfoEXT` request. Reuses the app's present_id2 if it has one, otherwise injects our own ID. |
| `vkCreateSwapchainKHR` | Adds `PRESENT_TIMING_BIT`/`PRESENT_ID_2_BIT`. Records present mode, image count, format, extent and the engine name (`VkApplicationInfo.pEngineName == "DXVK"` gives Runtime = DXVK). |
| `vkQueueSubmit` / `vkQueueSubmit2` (+`KHR`) | Wraps each batch with one pre-recorded command buffer of the layer before (resets its two queries, `TOP_OF_PIPE` timestamp) and one after (`BOTTOM_OF_PIPE` timestamp), on every graphics/compute queue (DXVK uses async compute too). The app's command buffers are never touched. See "GPU timestamps" below. |
| `vkSetLatencyMarkerNV`, `vkLatencySleepNV` | Reflex markers forwarded by DXVK/dxvk-nvapi: sim start/end, render submit, input sample, PC-latency ping, sleep start/end. |
| `vkGetPastPresentationTimingEXT`, `vkSetSwapchainPresentTimingQueueSizeEXT` | The app uses present_timing itself: DXVK 3.1 sets a queue size, attaches `VkPresentTimingInfoEXT` (one stage) to every present and polls `vkGetPastPresentationTimingEXT` every frame for its frame pacing and DXGI frame statistics. The layer owns the driver's feedback queue and serves the app from a cache, so neither side steals the other's results (see below). |

- **No worker thread in M1.** Feedback is polled at present time (inside our `vkQueuePresentKHR` hook, and inside
  the app's own `vkGetPastPresentationTimingEXT` calls), under a per-swapchain mutex:
  - converts stage times to `CLOCK_MONOTONIC` with `vkGetCalibratedTimestampsKHR`, re-calibrated about once a second
    (no conversion when the time domain already is `CLOCK_MONOTONIC`). When the swapchain's time domains change
    (the window moved to another display), the layer picks its domain again at the next present (never from the
    app's own `vkGetPastPresentationTimingEXT`, which does not hold the swapchain), retrying until that works;
    frames still reported in a retired domain keep its last calibration (a zero reading for a retired id is not
    taken as one)
  - emits finished records **in present order** into the ring; a present whose feedback never arrives is emitted
    with the "timing lost" flag after a timeout, so one lost report never stalls the stream
  - the same finished records feed the overlay's in-process metrics
- **GPU timestamps** (`gpu_timer.cpp`, attribution in `gpu_intervals.hpp`; `PRESENTMON_GPU=0` turns them off):
  - Per queue family, chunks of 64 slots (at most 8): a query pool of 128 timestamps and, per slot, the two
    command buffers above, recorded once (`SIMULTANEOUS_USE`). A new chunk's queries are reset once by a small
    submission of its own on the queue that needed it; its slots are used after that finished (the batches in
    between go untimed). Not timed: batches without command buffers, protected ones, ones with a
    `VkDeviceGroupSubmitInfo`, families without timestamps or without graphics/compute, and batches while every
    slot is in flight. Needs `VK_KHR_calibrated_timestamps` with the `DEVICE` and `CLOCK_MONOTONIC` domains (the
    layer enables the extension).
  - Read back without waiting when a frame record is finished (the broker's sink): a slot whose end tick equals
    its previous one has not run its new reset yet. Per queue, a batch starts no earlier than the previous one
    on its queue ended (`TOP_OF_PIPE` is written as soon as the command processor reaches it). Ticks go to
    `CLOCK_MONOTONIC` through a `DEVICE`/`CLOCK_MONOTONIC` calibration renewed every second.
  - Attribution on the GPU timeline, not by submission: a frame's work is every batch that ended by its present's
    ready time (`QUEUE_OPERATIONS_END`), plus batches submitted before its present call that ended up to 0.5 ms
    after it (the two clocks' calibrations disagree by 10-20 us; without that slack every frame got the previous
    frame's work). DXVK and vkd3d-proton submit the next frame's work before the present call of this one (other
    threads), so submission order would put it in the wrong frame. A frame without a ready time gets no GPU data.
    Work that ended by the previous frame's ready time (read back too late for it) or more than 1 s before this
    one (a loading screen, work before the first frame) is dropped rather than charged to a frame. One swapchain
    per device gets GPU numbers (the first with ready times; the next one when it is destroyed or silent for
    250 ms), since two would take each other's work. A batch still not done after 5 s (a paused game waiting on a
    semaphore) is no longer attributed and stops holding up its queue; its slot comes back once it has run.
    Reviewed 2026-11-09 (one-chunk-at-a-time setup, per-queue start clamp only for in-order ends, the extension
    only on Vulkan 1.1+ instances).
    `gpuStartNs` = the first start, `gpuBusyNs` = the union of the intervals (idle gaps and overlap between
    queues count once), and PresentMon derives GPULatency (CPU start -> GPU start), GPUWait ((ready - GPU start)
    - busy) and GPUTime.
  - Measured: against `vk_truth`'s own timestamps of its frame (3 ms load at 60 fps) every frame matches, busy
    within 5 us at the 95th percentile and start within 25 us (`e2e.load`); DXVK and vkd3d-proton runs get GPU
    data on every frame (`e2e.proton`). In A/B runs of vkd3d-proton next to a game the present-to-ready time was
    the same with and without GPU timing. Idle machine, 2026-11-09 (`e2e.overhead`,
    `test_gpu_timing_on_a_gpu_bound_app`, budget 0.05 ms per frame): on a GPU-bound `vk_truth` (4 ms of GPU
    work per frame, mailbox) the median frame interval was 4.108 ms with GPU timing and 4.078 ms without, +0.03
    ms (0.7 %), about the run-to-run noise (the overlay run came out 0.025 ms faster than no layer at all). CPU
    cost inside the present: +0.0003 ms. GPU-bound `e2e.load` runs (8 ms mailbox, 10 ms FIFO): MsGPUBusy within
    7 us of `vk_truth`'s own timestamps at the 95th percentile, no frame without GPU data.
- **Sharing present timing with the app** (DXVK case):
  - Present IDs: if the app created the swapchain with `PRESENT_ID_2_BIT`, its own IDs are used unchanged wherever it
    sends one. On its presents without one the layer injects its own, each one above every ID sent so far on that
    swapchain: vkd3d-proton creates every swapchain with `PRESENT_ID_2_BIT` but sends IDs only on FIFO presents
    (V-Sync on), none with tearing (IMMEDIATE), so a game with V-Sync off had no display timing at all (Lies of P,
    2026-11-09). An ID must be larger than every earlier one of its swapchain, and the app's waits
    (`vkWaitForPresent2KHR`) only work if the layer's IDs never get ahead of the app's. vkd3d-proton numbers its
    presents `max(previous + 1, DXGI present count)`, which stays above the layer's (one per present). Should an app ID
    ever come at or below one the layer injected, the layer stops injecting on that swapchain for good and logs a
    warning. Covered by `e2e.proton` (`ProtonD3D12`, `tests/apps/d3d12_truth.cpp`: tearing only, and V-Sync toggled
    every 90 frames). An app that uses `VK_KHR_present_id` (v1: DXVK 2.x,
    vkd3d-proton, and DXVK 3.1 on surfaces without present_id2) also keeps its own IDs, and the layer matches its
    timing reports by them. Otherwise the layer adds the bit and injects its own IDs. Swapchains without any IDs
    get no layer timing at all (matching reports by order alone misattributes them after a lost report).
  - Measured on this driver (615.78.08), which the v1 case relies on: `VkPastPresentationTimingEXT::presentId`
    carries v1 IDs too (the spec only promises IDs given through `VkPresentId2KHR`); v1 and v2 IDs share one
    namespace per swapchain, and both wait functions work on IDs given either way; when a present carries both
    nodes, the `VkPresentId2KHR` value wins and the v1 value is ignored, so the layer must never add its own
    `VkPresentId2KHR` next to an app's v1 IDs. If a driver reported 0 for v1 IDs, the reports would go unmatched
    and the layer would switch its timing off for that swapchain after 64 lost entries.
  - Per present, the stage mask sent to the driver is the union of the app's stages and ours; the app's time domain,
    target time and flags are kept as they are.
  - The app's `vkGetPastPresentationTimingEXT` returns only presents the app asked about, only the stages it asked
    for, in its time domain, in the format it would get from the driver. The queue size sent to the driver is
    the app's size plus our headroom; the app's own limit is emulated.
  - The emulated limit behaves like the driver's own (measured natively): the queue holds requests whose results
    the app has not retrieved yet; the default size is 0, so timed presents are rejected until the app sets a
    size; shrinking below the outstanding count returns `VK_NOT_READY`. A rejected present
    (`VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT`) is not sent down: its image stays acquired and is not shown, its
    present ID stays unused, and its semaphore waits still execute (an empty `vkQueueSubmit` on the present queue
    does that). One deliberate difference: the layer signals the present's `VkSwapchainPresentFenceInfoKHR`
    fences, as the spec's "still considered enqueued" rule says; this driver leaves them unsignaled, and DXVK 3.1,
    which counts the fence as signaled and recreates the swapchain after QUEUE_FULL, would then wait on it forever.
  - A present to several swapchains where only some hit the emulated limit cannot be rejected for just those in
    one driver call, so their requests go down over the app's size (the driver has the headroom) and the app gets
    the driver's result. The layer adds its own `VkPresentId2KHR` only when every swapchain of the present has
    `PRESENT_ID_2`.
  - If the driver itself refuses the layer's extra request (`QUEUE_FULL` on a present the layer added a stage to),
    a single-swapchain present is repeated once without the layer's request: no semaphore waits (the refused
    attempt executed them), but with the app's present fences, which the refused attempt does not signal on this
    driver. A present to several swapchains is not repeated (that would present the accepted ones again): each
    swapchain keeps the driver's result, and the layer stops asking for its own timing on the refused one for a
    while. Neither has been seen on this driver; the broker keeps the driver's queue from filling.
  - When a swapchain is destroyed, the layer waits up to 30 ms (without blocking other threads) for the reports of
    its last presents, so the tail of a capture is not lost; FIFO reports arrive two to three refreshes late. It
    does not wait once the driver returns an error, nor for presents more than 30 ms old.
- **Never write into the app's structures.** To enable the present-timing features at device creation, the
  `VkDeviceCreateInfo` chain is copied (up to the last node that needs patching) and the copy is changed; an
  unknown node in the way means the device is created exactly as the app asked and timing is off for it. The
  present-info chain is rebuilt the same way. The struct sizes come from `src/layer/chain_sizes.inc`, generated
  from the pinned `vk.xml` by `src/layer/tools/gen_chain_sizes.py`; the `pml_chain_table_check` test fails when
  the headers change and the table was not regenerated.
- **Shared-memory segment lifetime:** created on the first swapchain, sized with `posix_fallocate` (a full
  `/dev/shm` makes creation fail instead of killing the game with SIGBUS later), unlinked at exit. Before it
  creates its segment, the layer removes the segments of processes that are gone and have been silent for
  5 minutes (and half-created or other-version files that old), so killed games do not pile up; if `/dev/shm`
  is still full, it removes those silent for 10 s once and retries. The collector removes them after 10 s; the layer waits longer because a live game in
  another pid namespace that shares `/dev/shm` (a sandbox, not supported) looks dead from here. After `fork()`,
  the child does not write the parent's ring or swapchain table (the parent's state stays inert there); it creates
  its own segment if it uses Vulkan.
- **Synthetic input mode** (off by default): pyrofling's MSE detector and uinput mouse supply
  `inputTime`/`mouseClickTime`. This fills `MsClickToPhotonLatency`/`MsAllInputToPhotonLatency` without
  needing access to real input devices. Readback runs only while a capture is armed.

### 3.2 Record → `FrameData` mapping

| `FrameData` field | Linux source |
|---|---|
| `presentStartTime`, `timeInPresent` | `CLOCK_MONOTONIC` around `vkQueuePresentKHR` |
| `readyTime` | present stage `QUEUE_OPERATIONS_END`, calibrated |
| `gpuStartTime`, `gpuDuration` | first GPU timestamp of the frame's work; union of its busy intervals (idle gaps excluded, as PresentMon does); 0 (NA) without GPU timing. Which work is the frame's: 3.1, GPU timestamps |
| `gpuVideoDuration` | 0 (NA). Video queues are out of scope. |
| `displayed` | `{Application, FIRST_PIXEL_OUT}` when `reportComplete`. An empty list means dropped (MAILBOX replaced it). |
| `finalState` | `Presented` or `Discarded`, from the above |
| `presentMode`, `syncInterval`, `supportsTearing` | the swapchain's `VkPresentModeKHR` (FIFO → sync 1, IMMEDIATE/FIFO_RELAXED → tearing allowed) |
| `runtime`, `presentMode` | left at PresentMon's defaults (the math never reads them). The collector prints Linux values itself: Runtime `Vulkan` / `DXVK` / `VKD3D` (from the engine name), PresentMode = the Vulkan present mode name. No upstream enum changes. |
| `appSim*`, `appRenderSubmit*`, `appSleep*`, `appInputSample` | Reflex markers |
| `pclSimStartTime`, `pclInputPingTime` | Reflex `SIMULATION_START` and `PC_LATENCY_PING` markers |
| `inputTime`, `mouseClickTime` | synthetic-input mode only. Real input needs the `input` group, so it is deferred. |
| `flipDelay`, `interFrameEventStats` | 0 / empty (NA) |
| `swapChainAddress`, `processId`, Application | the `VkSwapchainKHR` handle, `getpid()`, and the Windows exe name from `/proc/<pid>/cmdline` for Wine (instead of `wine64-preloader`) |

Timebase: `CLOCK_MONOTONIC` ns, shared by every process. The collector uses `QpcConverter(1e9, sessionStart)`.
Zero means "missing" throughout PresentMon's code, so a real timestamp must never be 0.

### 3.3 `presentmon` collector (`src/collector/`)
- Command line mirrors PresentMon's console app:
  - `--process_name`, `--process_id`
  - `--output_file`/`--output_stdout`, `--no_csv`
  - `--timed`, `--terminate_after_timed`
  - `--v1_metrics`/`--v2_metrics`
  - `--track_frame_type`, `--qpc_time`, `--date_time`
- Writes the CSV with the same headers and formatting, including the UTF-8 BOM, so files from
  Windows and Linux can be compared directly.
- Behaviour when the collector isn't running:
  - The layer writes into its ring anyway; the ring wraps and old records are lost.
  - The layer creates no files and does no I/O on the game thread.
- Extra subcommands: `presentmon list` (live layer-enabled processes and swapchains; segments of processes that
  are gone are hidden and only counted) and
  `presentmon overlay show|hide|toggle|status [--process_name N] [--process_id P]`.
- Robustness rules:
  - `--delay`/`--timed` select frames by their present time, not by when their record arrives (records come after
    the display feedback); the timed capture closes 250 ms after the end so late records of frames presented
    before the end still land.
  - Lost records (ring overwritten before the collector read them) are reported as a warning (again when the
    count has doubled, or at most every 10 s while it grows) and in the final summary, which also counts
    processes that exited before the stop; also with `--no_console_stats` or CSV on stdout.
  - SIGINT, SIGTERM, SIGHUP and SIGQUIT all flush and close the CSV, except one the parent left ignored
    (`nohup` keeps a capture alive when the terminal closes).
  - A segment that is replaced on disk while its process lives (the process re-executed itself) is followed; an
    engine whose swapchain is idle for 4 s after being destroyed is dropped. A live swapchain keeps its engine
    however long it pauses (as upstream does for a lone presenter), so the rows around the pause are written.
  - Display-info stalls: PresentMon's V2 engine holds every row until the next displayed frame. When an unbroken
    run of presents without display times lasts 500 ms of present time (the layer's timing budget is exhausted at
    very high frame rates), the held rows are released with NA display columns, so the CSV never stops. A gap of
    500 ms or more between two presents (a hitch, a 1 fps menu, a pause) starts a new run, so it never counts as
    a stall, whatever was held around it: the next displayed present releases the held rows with their real
    display times, as in upstream. Below 2 fps without any display times, rows likewise wait for the next
    displayed present. Unit tests compare these cases with upstream's UnifiedSwapChain.
  - For DXVK, `vkQueuePresentKHR` runs on DXVK's `dxvk-submit` thread, not the game thread, so `MsInPresentAPI`,
    `MsCPUBusy` and `MsCPUWait` describe that thread, and the latency columns can be off by up to one refresh
    interval (error model and measured numbers in 6. Risks).

### 3.4 In-game overlay (inside the layer, `src/layer/overlay/`)
- Drawn by the layer into the image being presented, MangoHud-style, with Dear ImGui (`third_party/imgui`, v1.93.0)
  and its Vulkan backend loaded through the layer's dispatch table (`IMGUI_IMPL_VULKAN_NO_PROTOTYPES`).
- Numbers come from the **same PresentMon metrics code as the CSV**: the layer runs `UnifiedSwapChain` on its own
  finished records, in-process, and keeps rolling windows (fps, frame time graph, 1%/0.1% lows, CPU busy/wait,
  display latency, dropped frames, GPU busy/wait and GPU latency on a line of its own; GPU numbers show NA
  when fewer than half the frames of the last second have GPU data; the VRR verdict, 3.4.1).
- Visible by default when the layer is enabled. Env: `PRESENTMON_OVERLAY=1` (visible, default), `0` (loaded but
  hidden, can be shown from the CLI) or `off` (not loaded, swapchain untouched). Only `off` leaves the swapchain
  exactly as the app created it: `0` still adds `COLOR_ATTACHMENT` usage, because usage is fixed at creation and
  the CLI must be able to show the overlay later. If a swapchain cannot be created with the layer's additions
  (timing flags, overlay usage), it is created again exactly as the app asked, without the overlay.
  `PRESENTMON_OVERLAY_POSITION`; `PRESENTMON_OVERLAY_SCALE` (default automatic: shorter edge/1440 clamped to
  0.75-3, 16 px base font, so a portrait 1440x2560 game gets 1.0). Live control from the CLI through the control area of each process's shared-memory
  segment; a KDE global shortcut can run `presentmon overlay toggle` (games on Wayland don't share keyboard input
  with layers, so no in-game hotkey).
- SDR (UNORM/sRGB 8-bit) and HDR (HDR10 PQ 10-bit, scRGB FP16) swapchains, since DP-2 runs HDR: overlay colours are
  encoded for the swapchain's colour space so white text sits around 200 nits in HDR.
- Two metrics engines per swapchain: V1 for the per-present numbers (fps, frame time, CPU), V2 for the display
  numbers. V2 alone would stall when almost no frames carry display times (uncapped rates beyond the timing budget).
  Display numbers lag one displayed frame, as in PresentMon's own V2 output.
- On a swapchain without present timing (a driver without `VK_EXT_present_timing`, or a failed setup), the overlay
  still shows frame rate and frame times from CPU-side present times; display numbers stay NA.
- Font: Cousine Regular from ImGui's `misc/fonts` (SIL OFL 1.1), embedded in the layer. Every printable ASCII
  glyph is baked when the backend is created or the scale changes, and all text the overlay shows comes from fixed
  tables (runtime, present mode, format, colour space) and numbers, so no glyph is added mid-frame.
- Nothing in the present path waits for the GPU. The font atlas upload is recorded into the overlay's own command
  buffer for that present, ahead of its render pass (as MangoHud and Mesa's overlay layer do); a changed atlas
  becomes a new image, and replaced images and staging buffers are freed once the per-image fences show the GPU
  is done with them. Dear ImGui's own upload path (`vkQueueSubmit` + `vkQueueWaitIdle`) is switched off. The one
  wait left: a swapchain presented from two queues (legal, rare) draws on the second queue only after the upload's
  fence, once per atlas change.
- Test hook: `PRESENTMON_OVERLAY_DUMP=<file>` reads back one presented image (with the overlay) so tests can check it
  without screenshots.
- Costs: swapchain gets `COLOR_ATTACHMENT` usage, and one small render pass per present while visible. The overlay's
  GPU work lands after the app's work, so it is inside `readyTime` but outside the app's GPU busy time (its
  submissions go down below the layer's hooks).

- **GQI** (Gaming Quality Index, Blackbird PC Tech, 2025; shown on the overlay's FPS panel, the GUI's Live and
  Captures pages): 1% low^2 / (60 x average fps) = (1% low / 60) x (1 - (average - 1% low) / average), from
  `pml/metrics/summary.hpp`. Its 1% low is time-weighted (slowest frames summed until 1 % of the time), not the
  mean-of-slowest-1 % shown next to it. >= 1: 1% lows of 60 fps or more with little spread. It has no upper
  bound (it grows with frame rate), and the 60 fps reference ignores the refresh rate.

### 3.4.1 VRR stability verdict (`src/metrics/include/pml/metrics/vrr.hpp`)
One analyzer, used by the overlay (last 10 s of displayed frames), the GUI's Live page (the same, plus the
driver's VRR state) and its Captures page (a whole CSV). Per pair of consecutive displayed frames it looks at the
time between screen updates and the game's own frame interval:
- **Uneven**: the screen spacing jumps by more than max(2 ms, 25 % of its median); "on screen only" when the
  game's interval stayed within half that. Under VRR each jump is a refresh-rate jump (the cause of VRR flicker
  on OLED/VA panels). **Stutter**: a frame on screen more than 2.5x the median (weighs 3). **LFC crossing**:
  the spacing crosses the panel's lowest VRR rate (weighs 3; needs the EDID range).
- Score = 100 * exp(-weighted events per pair / 0.25); >= 85 stable, >= 65 minor jitter, >= 40 unstable.
  No published standard exists; the 2 ms floor keeps the game's normal sub-millisecond variation out (Lies of P
  at 165 fps: its own interval jumped > 1.2 ms 13 % of the time, the screen's > 2 ms 21 % against 4 %).
- State first: **off** when the driver says so or when screen updates snap to a refresh grid while the game's
  intervals do not (the lowest common rate that explains it when the output's is unknown, so the overlay finds
  "fixed 120 Hz" by itself); **idle** when the game runs at or above the refresh rate; **active**; **unknown**
  when it cannot be told (the game itself on the grid, e.g. FIFO at the refresh rate): then the score is shown
  with "?".
- The GUI reads, read-only through libdrm (`backend/drmvrr.cpp`), each connected output's CRTC `VRR_ENABLED`,
  mode refresh and EDID range descriptor. KWin enables VRR only for the active fullscreen window, so the one
  output with VRR on is the game's; none on = VRR off (e.g. the game is not the active window).
- In the overlay the driver's state is unknown (the game process would need DRM ioctls on its present thread);
  it shows on / ? / idle / off from the data.

### 3.4.2 Frame caps (`src/layer/frame_limiter.*`, `frame_cap.*`, the cap code in `layer.cpp`)
Research (2026-11-09; papers, DXVK, vkd3d-proton, MangoHud 0.8.4, Special K, LatencyFleX, low_latency_layer,
dxvk-nvapi): no published algorithm beats the open ones. The best CPU-side limiter is an absolute deadline grid with
a sleep-then-spin wait (DXVK's `FpsLimiter`, vkd3d-proton, Special K). What decides latency is **where** the wait
sits: on the thread that starts the next frame, before it samples input. MangoHud waits inside
`vkQueuePresentKHR`, which under DXVK and vkd3d-proton is their submit thread, so the game runs a frame ahead.

**Limiter** (`FrameLimiter`): next deadline = previous deadline + T (no drift from wake-up lateness); a frame later
than min(500 us, T/8) re-anchors the grid instead of catching up with a short frame. `clock_nanosleep(TIMER_ABSTIME)`
to deadline - margin with the thread's timer slack at 1 ns for that sleep (restored), then PAUSE until the deadline.
The margin follows the p99 of wake-up lateness (re-derived every 32 sleeps, 30 us .. min(1 ms, T/4)). A refused sleep
(seccomp) falls back to `nanosleep`, else spins and is logged; an interval is never longer than 1 s.

**Where it waits** (one plan per `vkQueuePresentKHR`, for the call's largest swapchain), in this order:
1. **Reflex** (`VK_NV_low_latency2`): the app called `vkLatencySleepNV` in the last 500 ms, so its own sleep paces
   the game thread. The layer sends the app's sleep mode with `minimumIntervalUs = max(app, ceil(1e6 / cap))`
   (the app's own lower cap stays) and waits nowhere else. Committed only when the driver accepts it (else the
   present-side wait covers and the layer retries every second); the app's mode is restored when the cap goes.
   NVIDIA 615 enforces the interval even with `lowLatencyMode` false, about 0.5 % long (10.05 ms for 10 ms).
2. **Frame-thread hold** (late placement): the app has a frame-pacing thread that releases its game thread, and the
   layer holds that thread instead of the present (`SwapchainState::FrameHold`), as DXVK's and vkd3d-proton's own
   limiters do:
   - the app blocks in `vkWaitForPresentKHR`/`2KHR` (timeout >= 100 ms) for 8+ presents in a row
     (vkd3d-proton's wait worker with V-Sync on; DXVK up to 3.1.1 with V-Sync on): held after the driver's wait
     returns, once per present id, never past the app's timeout;
   - another thread than the presenting one polls `vkGetPastPresentationTimingEXT` once per frame, 8+ in a row,
     and the present is not FIFO (DXVK 3.1.1+ with present timing, which the layer offers on NVIDIA: its
     `dxvk-frame` thread, V-Sync off): held before the poll is answered, once per present, for caps of 20 fps and
     more (the app's lock around the poll is held that long). Not in FIFO: DXVK holds a lock across that poll which
     its submit thread needs for FIFO presents (measured: 29 ms instead of 17, uneven rate).
   - Safeguards: a hold only for a swapchain planned in the last max(250 ms, 3T); the hold must pace the game:
     after the first window of 32 presents (queued frames drain in it), a window averaging under 0.9 T means the
     held thread does not pace it, and the present-side wait caps for 2 s. No backstop wait at the present while
     holding: on DXVK's submit thread it became the pacer itself (presents every 0.95 T, the hold re-anchoring).
     The library is linked `-z nodelete` (the loader unloads layers with the last instance; the DRM thread may
     still run its code).
3. **Late** (default placement): after the driver's present returned, inside the app's call.
4. **Early**: before the driver's present (most even present spacing, about a frame more latency).
No two of them cap one frame. Records carry `capWaitNs` and `kFrameCapWait` / `kFrameCapReflex` (a hold counts into
the next present's record); `presentStartNs`/`presentEndNs` stay the app's view of its call (a wait inside it is
time in Present(), as in PresentMon on Windows).

**Auto** (`CapRuntime`): floor(hz - hz^2/3600) (224 at 240 Hz, 223 at 239.99, 138 at 144; a community estimate of
NVIDIA's Reflex + G-SYNC cap, no NVIDIA source) for the output whose CRTC has `VRR_ENABLED` = 1, picked by swapchain
size, then orientation, then refresh; no cap while VRR is off. A thread reads DRM through `dlopen("libdrm.so.2")`
every 500 ms while Auto is selected and idles otherwise. Limitation: a windowed game is not matched to its own
output; with VRR on another output (KWin "Always", or another fullscreen app) Auto uses that output's rate.

**Control**: `PRESENTMON_FPS_CAP=<fps>|auto|off`, `PRESENTMON_CAP_PLACEMENT=late|early`, `PRESENTMON_CAP_REFLEX=0`
seed the segment's 64-byte `CapControl` block; `presentmon cap` and the GUI rewrite it live. Its settings half is a
seqlock (generation odd while a reader writes; concurrent writers take turns; a writer that died is overridden after
50 ms); the layer reads it with one atomic load per present and validates 1-10000 fps. The status half (method,
rate in force, mean wait, p99 release lateness, VRR state) is written at most every 250 ms and on the first present
after a change, with that change's generation (`appliedGeneration`), which `presentmon cap` waits for (500 ms).
Never capped in the process: one atomic load per present, nothing else.

**Measured** (tests/e2e/test_cap.py, NVIDIA 615, KDE Wayland; target 100 fps unless noted; latency = the app's frame
start, after any wait of its own, to first pixel; medians):

| Case | Game's own cap | Layer | MangoHud late | Translation layer's limiter |
|---|---|---|---|---|
| Native Vulkan, window, 120 Hz | 8.2-9.8 ms | 8.7-8.8 ms (late) | 8.4-9.3 ms | - |
| DXVK 3.1.1+, D3D11, V-Sync off | 10.1-10.3 ms | 9.7-10.6 ms (frame-thread hold) | 18.1-21.3 ms | `DXVK_FRAME_RATE` 9.5-9.9 ms |
| DXVK 3.1.1+, D3D11, V-Sync on | 10.1 ms | 19.5 ms (late) | 18.5 ms | `DXVK_FRAME_RATE` 10.1 ms |
| vkd3d-proton, D3D12, V-Sync on | 7.2-9.0 ms | 8.3-9.1 ms (present-wait hold) | 28.1-29.5 ms | `VKD3D_FRAME_RATE` 8.0-9.8 ms |
| VRR 239.99 Hz fullscreen FIFO, 223 fps | 1.6 ms | 1.6 ms (auto, late) | 1.6 ms | - |

- Early placement adds about a frame everywhere (native 17.8-18.1 ms, DXVK 28 ms; MangoHud's early: 18.9-21, 28.4).
- Rate on the app's clock: the layer 0.000 to -0.05 %, MangoHud -0.003 to -0.007 %, NVIDIA's Reflex interval -0.5 %.
- Frame-time jitter (sd of the app's present intervals): the layer 0.011-0.028 ms, MangoHud 0.026-0.031 ms, the
  app's own cap 0.011-0.017 ms; on screen, run-to-run noise (0.14-0.69 ms under VRR) is larger than any difference.
- CPU per frame (vk_truth, 1 ms of its own work): uncapped 1.05 ms, layer 1.41-1.46, MangoHud 1.57-1.63, the app's
  500 us spin 1.75-1.78.
- Not reachable from a layer: DXVK 3.1.1+ with V-Sync on (no per-frame present wait, and the timing poll cannot be
  held in FIFO) and vkd3d-proton with V-Sync off (its wait worker waits on a timeline semaphore, not a present).
  There the layer is as good as MangoHud; the game's own cap, `DXVK_FRAME_RATE`/`VKD3D_FRAME_RATE` or Reflex are
  a frame better.

### 3.4.3 Experience score (`src/metrics/include/pml/metrics/experience.hpp`)
One 0-100 number for how a game plays, from what reached the screen; presented like the VRR verdict (same bands:
>= 85 smooth, >= 65 minor issues, >= 40 rough, below: very rough; the largest term as the cause under 85). Overlay
panel "Exp" (last 10 s), GUI Live and Captures pages, the capture compare table. It replaces GQI as the summary;
GQI stays next to the 1% lows for comparison with Blackbird PC Tech's charts.

    score = 100 - rate - motion - stutter - latency       (additive impairments, as ITU-T G.1072), clipped 0..100

| Term | From | Formula | Evidence |
|---|---|---|---|
| rate | mean screen interval t (MsBetweenDisplayChange) | min(60, d^2/(d+3)), d = max(0, t - 4 ms): 120 fps 2.6, 90 fps 5.0, 60 fps 10.2, 30 fps 26.6 | QoE about linear in frame time, flattening at 75-90 fps, slight gains to 500 fps ([Tokey et al. 2025](https://web.cs.wpi.edu/~claypool/papers/fps-500-25/paper.pdf)); collapse below 15-30 fps (Claypool & Claypool 2007) |
| motion | RMS of MsAnimationError over non-hitch frames | min(30, 3 x max(0, rms - 1 ms)) | ~2 ms swings unnoticed, 6 ms bias perception to the long frames ([Klein et al. 2024](https://arxiv.org/abs/2306.01691)); frame-time SD slope -0.056 QoE/ms ([Xu & Claypool 2024](https://web.cs.wpi.edu/~claypool/papers/frame-variation-mmsys-24/paper.pdf)) |
| stutter | per frame max(screen interval - median, abs(animation error)); a hitch exceeds max(2 x median, median + 8 ms) + 1 ms | min(50, 0.1 x hitch ms per second + 25 x clamp((worst - 50 ms)/100 ms, 0, 1.75)) | interruption magnitude predicts QoE (R2 0.93), frequency does not (0.18) (Xu & Claypool 2024); spikes under ~50 ms acceptable, 150 ms+ hurt ([Tokey et al. 2026](https://web.cs.wpi.edu/~claypool/papers/spikes-fdg-26/paper.pdf)) |
| latency | median display latency (frame CPU start to screen; DisplayLatency, or MsUntilDisplayed + MsCPUBusy) | 0.848 x (I(L) - I(0)), I = ITU-T G.1072 input-delay impairment, high-sensitivity games: 20 ms 3, 40 ms 6.3, 100 ms 16.6 | G.1072 ([ITU](https://www.itu.int/rec/T-REC-G.1072/en)); FPS aiming degrades from ~40 ms (Ivkovic et al. 2015), 25 vs 55 vs 85 ms matters most for skilled players (NVIDIA) |

Why both screen intervals and animation error: under V-Sync a full queue hides a 30 ms CPU hitch from the screen
cadence (only the motion jumps), while a 100 ms freeze moves screen and simulation together (no animation error):
each signal misses one kind. One missed refresh (exactly 2 x the median) is judder, not a hitch.

Validation (scores from analyzeExperience on the recorded CSVs; tests/apps/vk_truth with `--stutter-every/--stutter-ms/--jitter-ms`, 8 s each; DP-1 fixed 120 Hz windowed,
DP-2 240 Hz VRR fullscreen):

| Scenario | Score | GQI | Main cost |
|---|---|---|---|
| VRR, cap 223 fps | 99 | 3.66 | - |
| mailbox uncapped (975 presented, 120 shown) | 96 | 15.9 | rate (120 Hz screen) |
| VRR, uncapped with jitter | 96 | 1.44 | judder |
| mailbox, app cap 120 | 95 | 1.98 | latency 20 ms |
| FIFO 120 (3-frame queue) | 92 | 1.90 | latency 38 ms |
| VRR, cap 141, 25 ms hitch every second | 93 | 0.12 | stutter |
| FIFO, deep queue + GPU 7 ms | 90 | 1.31 | latency 49 ms |
| VRR cap 141 with VRR switching on/off | 90 | 2.31 | stutter |
| FIFO 120, 30 ms CPU hitch every second (hidden by the queue) | 89 | 0.14 | stutter (animation error) |
| mailbox, jitter 4-12 ms | 86 | 0.93 | judder |
| FIFO, cap 90 on 120 Hz | 84 | 1.49 | judder |
| mailbox 120, 100 ms freeze every 4 s | 79 | 0.01 | stutter |
| FIFO, cap 60 on 120 Hz | 75 | 1.00 | judder |
| FIFO, cap 40 | 64 | 0.66 | rate |

Limits: the weights between terms are a design choice (no study puts smoothness and latency on one scale); latency
excludes the mouse/OS input path; no study covers VRR judder, dropped frames or tearing (immediate mode tears; not
scored); with too few frames (< 50) the score is -1. Animation error uses the frame's CPU start as its simulation time,
as PresentMon does without app markers.

### 3.5 `presentmon-gui` (`src/gui/`)
A Qt 6 Quick + Kirigami app (C++23) in the same tree: one QML module `PresentMon` (static library `pml_gui`: the C++
types and the QML pages) and the executable. No existing GUI fitted (Intel's PresentMon app, CapFrameX, FrameView and
OCAT are Windows-only; the Linux tools configure or read MangoHud). CapFrameX's Linux attempt (layer + daemon +
Avalonia, MIT, removed from its repository in 2026-09) was the closest design and lent ideas for the screens.
- **Data in-process**, from the code the CLI and the overlay use:
  - `ProcessModel` polls `/dev/shm` every 500 ms (`listSegments`, `Segment::open`): present rate from the header's
    `writeSeq`, the newest swapchain from the table, the overlay state from the control block. Its setters write the
    control block like `presentmon overlay ...`.
  - `LiveSession` maps one process's segment and reads its ring with its own `RingReader` cursor (readers do not
    disturb each other or a running CLI capture), with one `OverlayStats` per swapchain, so the numbers are the
    in-game overlay's; it follows the busiest swapchain (with hysteresis). `OverlayStats`' optional
    `onPresentRow`/`onDisplayRow` observers (the layer leaves them empty) feed the chart's per-frame history.
  - Capture statistics use `pml::metrics::summarizeFrameTimes()`, which the overlay uses too: average, nearest-rank
    percentiles, 1 % / 0.1 % lows as the frame rate of the mean of the slowest frames.
- **Recording runs the CLI** as a subprocess (`Recorder`): identical files and crash isolation; SIGTERM stops it
  cleanly; a file without frames is removed and reported.
- **`csvreader`** (plain C++) reads every PresentMon CSV layout (ours, Intel's 1.x/2.x, v1/v2 columns, NA, BOM,
  CRLF, quotes); a file with several processes or swapchains is reduced to the busiest swapchain, with a warning.
  Frame and display times over one hour are dropped with a warning (a damaged file, or one not in milliseconds).
- **`FrameTimeChart`**: a QQuickItem drawing triangle strips into the scene graph (a real line width on every RHI
  backend), min/max decimation per pixel column (a one-frame spike stays visible among 1M points), an automatic y
  range that ignores rare spikes and does not jump, and a QPainter → image-node path for the Qt Quick software
  backend (the offscreen platform, machines without a GPU driver), which cannot draw custom geometry.
- **`Hotkeys`**: KGlobalAccel actions (Meta+F11 record, Meta+F10 overlay); never registered when `PML_GUI_TEST` is set.
- The menu entry is written at install time (like the layer manifest), so its `Exec` path follows `--prefix`.
- Tests: `pml_gui_tests` (GTest on the offscreen platform with a temporary HOME: fake producers for the live types,
  synthetic and gold CSVs, recordings through the real CLI against the fake producer, chart pixels from a CPU
  rasterizer of the GPU node tree and from real grabs of the software path) and `pml_gui_smoke` (the app itself,
  headless, on every page with a fake process and gold captures: screenshots must not be blank, and every stderr
  line fails the test unless it is on a short list of known harmless ones).

### 3.6 Repo layout
```
CMakeLists.txt
src/common/                record format, shm segment + ring, clock helpers (the layer↔collector contract)
src/metrics/               CMake target compiling PresentMon mc/*.cpp unmodified (shim include dir, no patches to
                           third_party/), QpcConverter, record → FrameData adapter, per-swapchain metrics engine
src/layer/                 VkLayer_presentmon (+ .json.in)
src/layer/overlay/         in-game overlay (ImGui)
src/collector/             presentmon CLI, CSV writer port, console stats, overlay control
src/gui/                   presentmon-gui (Qt Quick + Kirigami): backend/ C++ QML types, qml/ pages, data/ desktop
                           entry and icon, tests/
tests/unit/                PresentMon MetricsCore + SwapChain tests via a CppUnitTest→gtest shim, plus our own tests
tests/apps/                vk_truth (native Wayland Vulkan app with known CPU/GPU load), d3d11_truth.exe (D3D11
                           game loop for Proton + DXVK runs, cross-built with mingw-w64 when available)
tests/e2e/                 python (stdlib unittest) drivers registered with ctest
docs/
```
`third_party/` submodules are never modified; anything that has to differ lives in our tree.

## 4. Automated testing on this machine

Everything runs from `ctest` inside the Plasma session, with no prompts and no sudo.

1. **No windows** (`ctest -LE live`, parallel-safe):
   - PresentMon's own `MetricsCore.cpp` tests (207) compiled unchanged through a small macro shim mapping
     `TEST_METHOD`/`Assert::*` to gtest; they prove the metric math survived the port. (`SwapChainTests.cpp`'s 11
     tests are commented out upstream because they need Windows code.)
   - CSV format: headers and number formatting compared byte for byte with `Tests/Gold/test_case_*{,_v1,_v2}.csv`.
   - Our own tests: shared memory and ring (including a 1 MB tmpfs in a private namespace, fork), the record →
     `FrameData` adapter and engine timelines, the CLI against a fake producer, the layer's timing broker and
     pNext-chain handling, overlay UI and statistics, and the generated struct-size table.
   - The GUI, headless: `pml_gui_tests` and `pml_gui_smoke` (3.5).
2. **Live** (`ctest -L live`; ctest runs them one at a time through `RESOURCE_LOCK desktop`):
   - `pml_layer_smoke`: vkcube under the layer in every present mode, Wayland and Xwayland.
   - `pml_overlay_live`: the overlay's pixels in every swapchain format (sRGB, UNORM, HDR10 PQ, scRGB FP16), runtime
     show/hide/position/scale, and the overlay together with a DXVK-like app.
   - `tests/e2e` (`vk_truth` writes its own ground-truth log; every CSV row is matched to its frame):
     `csv_truth` (each present mode, v1/v2 metrics, rows = frames minus 2), `fullscreen` (each output's refresh,
     and VRR on DP-2: a 150 fps run whose frames are judged against the output's real VRR state, the CRTC's
     `VRR_ENABLED` property read through libdrm while the run goes on, because KWin switches VRR whenever the
     active window changes; frames near a switch are skipped; see 6. Risks), `load` (CPU- and GPU-bound; the GPU columns frame by
     frame against `vk_truth`'s own GPU timestamps),
     `dxvk_share` (DXVK-like apps that use
     present timing, present_id2
     or present_id v1 themselves: same pacing and reports with and without the layer, queue sizes 1/4/64, time
     domains), `xwayland` (display columns NA), `lifecycle` (kill -9, stale segments, swapchain recreation),
     `multiprocess`, `mangohud` (both layers at once), `overhead` (layer, overlay and GPU timing cost budgets).
   - `tests/e2e/test_proton.py`: a real D3D11 game loop, `d3d11_truth.exe` (`tests/apps`, cross-built with
     mingw-w64), run by GE-Proton's `proton` script (Steam not running) with DXVK 3.x, and with a DXVK 2.x
     (v1 present IDs) loaded app-locally next to a copy of the exe. It logs QueryPerformanceCounter around every
     `Present()` call, so the CSV is compared with the game thread rather than with DXVK's presenter thread: frame
     and display times, DXVK's own `GetFrameStatistics` next to the layer, Xwayland (display columns NA), the
     overlay drawn on DXVK's swapchain, and the presenter-thread limitation (6. Risks). The Wine prefix is kept in
     `<build>/e2e-proton/`; `PML_PROTON` / `PML_DXVK2_DIR` pick other builds. `ProtonD3D12` runs
     `d3d12_truth.exe` under vkd3d-proton (proton-wineland by default, `PML_PROTON_D3D12`): tearing presents
     without app present IDs, V-Sync toggled every 90 frames, and GPU data on every frame.
   - The screens must be awake: with an output in DPMS Off, KWin throttles windows and reports no display times,
     so the live tests skip; `PML_E2E_WAKE=1` lets them run `kscreen-doctor --dpms on` first.
     `PML_DESKTOP_LOCK=<file>` makes each scenario hold an flock, for several test drivers on one desktop.
   - Sharing the desktop (using the PC or playing while tests run): a KWin window rule keeps the test programs'
     windows (`vk_truth`, `vkcube`, `d3d11_truth.exe`) at a fixed place on a spare screen, below all other
     windows and without focus, and `PML_E2E_SHARED_DESKTOP=1` skips what would disturb the user (fullscreen and
     VRR, GPU-bound loads, overhead benchmarks, the uncapped 19000 fps run, the Proton Xwayland run, whose window
     Proton names `steam_proton`). Covered windows still present at full rate, but their display times can be off
     the vblank grid (6. Risks): on the 240 Hz HDR screen, with the user's windows on top, `e2e.csv_truth` failed
     its refresh-multiple check in the FIFO_LATEST_READY run (0.83 of 8-frame sums on a multiple, 0.9 needed; only
     2% of single intervals, and 41% in the FIFO run that passed). Without focus the test window never becomes
     KWin's active window, so VRR never engages; the VRR check then reports "VRR off".
     See README.md.
3. **Planned**: click-to-photon (M4), more commercial games under Proton (Lies of P checked live).

## 5. Milestones
- **M0**: build skeleton, metrics lib compiles on Linux, upstream unit tests pass.
- **M1**: layer present hooks + present_timing (shared with the app) → ring → collector → CSV (CPU + display
  columns) **and the in-game overlay**, tested with `vk_truth`.
- **M2**: GPU timestamps (GPUBusy/GPUWait/GPULatency), overhead test: done 2026-11-09.
- **M3**: DXVK/Proton (Wine Wayland), Reflex markers → instrumented/PC-latency columns, exe naming, 32-bit layer,
  shader/pipeline compile stats (PresentMon's PSO columns).
- **M4**: synthetic-input click-to-photon.
- A separate GUI app is not planned; the overlay plus the CLI cover it for now.

## 6. Risks
- **Proton defaults to Xwayland**: no screen time there on NVIDIA. Mitigation: Wine Wayland, with a fallback to NA.
- **GPU timestamp cost**: DXVK and vkd3d-proton submit many batches per frame, and each one gets two extra
  command buffers (a query reset and two timestamp writes). Measured +0.03 ms per 4 ms GPU-bound frame (within
  the noise, 3.1). If a game with many more batches shows more: time only the first and last batch of a frame,
  or reset queries on the host (`hostQueryReset`).
- **App and layer both using present_timing** (DXVK 3.1 does, every frame): handled by the sharing rules in 3.1.
  A mistake there changes DXVK's frame pacing, so `vk_truth` has a DXVK-like mode that uses present timing and
  present IDs itself, and the tests compare what it sees with and without the layer.
- **PresentMon's metric code needs C++23 with GCC 16**: `cnr/FixedVector.h` uses `std::from_range_t`, which GCC 16
  only provides in C++23 mode. The whole tree builds as C++23 (the layer too, since the overlay links the metrics
  code).
- **DXVK presents from its own thread** (`dxvk-submit`). Measured by `test_proton` with GE-Proton 11 (DXVK 3.1.1)
  and DXVK 2.5.1, `d3d11_truth` windowed at 120 Hz on Wine Wayland, against the game thread's own clock (the k-th
  present record is the game's k-th `Present()` call: DXVK presents each call once, in order):
  - Unaffected: frame times (median within 0.005 ms of the game's `Present()` intervals) and display times
    (`MsBetweenDisplayChange` 8.334 ms, every frame displayed).
  - The CPU columns describe the presenter thread. A vsync-bound game waits 8.3 ms inside `Present()`, but
    `MsInPresentAPI` shows 0.04 ms and `CPUBusy`/`CPUWait` read 8.3/0.04 ms instead of 0.004/8.3 ms.
  - The latency columns are computed from the presenter's records too. With L = how long after the game's
    `Present()` call DXVK's `vkQueuePresentKHR` comes, `MsUntilDisplayed` is short by L, and v2 `DisplayLatency`
    (counted from the CPU start) is long by the game's wait inside `Present()` minus L. Which one is off depends on
    how the DXVK version paces a vsync-bound game (presenter / game side, 120 Hz):
    - DXVK 3.1.1 presents each frame about one refresh after the game's `Present()` call (L = 8.36 ms):
      `DisplayLatency` is right (37.6 / 37.7 ms) and `MsUntilDisplayed` is one refresh short (29.3 / 37.7 ms).
    - DXVK 2.5.1 presents at once (L = 0.05 ms): `MsUntilDisplayed` is right (24.9 / 24.9 ms) and
      `DisplayLatency` is one refresh long (33.2 / 24.9 ms).
    - A CPU-heavy game under DXVK 3.1.1 (6 of 8.3 ms busy; L = 2.4 ms, wait 2.3 ms): `DisplayLatency` is right,
      `MsUntilDisplayed` is 2.4 ms short. Frame-capped without vsync (no wait, L under 0.1 ms): both are right
      within 0.2 ms under either version.
    `test_proton` checks on every run that both columns are computed from the presenter's records, and prints the
    game-side comparison.
  - Fix in M3: take the game thread's timing from the Reflex / latency markers DXVK forwards (`appPropagated*`
    fields).
- **Driver-specific behaviour the layer relies on** (NVIDIA 615.78.08, see 3.1): v1 present IDs in timing reports,
  and the shared v1/v2 ID namespace. Both were measured; other drivers may differ.
- **VRR follows KWin's active window, not the game.** With the output's VRR policy Automatic, KWin 6.8.2 decides
  for every frame: VRR is on only while the *active* window is fullscreen on that output
  (`src/compositor.cpp:877-889`; no timer or frame-rate rule). A click on any other window, even on another
  screen, switches it off in the middle of a game, and a window that cannot take focus never gets it. KWin also
  falls back to plain VSync without a log line when the driver rejects a VRR commit (`drm_output.cpp:316-325`).
  The CSV shows whichever happened: measured on DP-2 (240 Hz) with a 150 fps cap, through mid-run switches, in
  16 runs (2026-11-08), 84.6-97.7% of the judged frames per run followed the frame times (within 0.6 ms) while
  the CRTC's `VRR_ENABLED` was 1, and 90.6-100% sat on refresh multiples while it was 0 (frames near a switch not
  judged). `e2e.fullscreen` checks this whenever it can read `VRR_ENABLED`. In those runs VRR also dropped
  0.6-2.7 s into runs while nobody had knowingly clicked. In one run with KWin's activations logged, VRR went off
  at the moment the Claude desktop app became the active window (whether by a click or by itself is not known).
- **Display times of a covered window** (NVIDIA 615.78.08, KWin 6.8.2). The driver offers only the
  present-stage-local time domain and only the first-pixel-out stage (no `FIRST_PIXEL_VISIBLE`, no
  `REQUEST_DEQUEUED`); the layer converts with a calibration refreshed once a second. On an uncovered fullscreen
  window at 240 fps (probably scanned out directly; not verified), 98% of display changes are within 0.1 ms of a
  refresh multiple and about 95% within 0.05 ms. With another window on top (composited) and a frame rate that
  does not divide the refresh rate (150 fps at 240 Hz), two of three covered runs had 2 of every 5 frames (37-38%)
  arrive 0.15-0.35 ms off the grid, in step with the frame cadence: one present stage, one calibration offset per
  second, so the values come from the driver that way. The third (a ring dump) had no such pattern; 3 s in, one
  frame was displayed half a refresh off the grid and every later frame +0.17 ms off (calibration sample or a real
  refresh-phase shift; not settled). Display-change intervals of covered windows reach about 0.45 ms off a refresh
  multiple. nvidia-drm does not answer `drmWaitVBlank` or `CRTC_GET_SEQUENCE` (EOPNOTSUPP), so there is no kernel
  vblank time to compare against.
- **No Vulkan validation layers installed** (`vulkan-validation-layers` needs sudo). Tests use them when present.
- **HDR on DP-2**: synthetic-input MSE must handle FP16/10-bit swapchains (pyrofling only handles 8/10-bit).
