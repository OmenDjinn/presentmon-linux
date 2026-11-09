# Findings

What building presentmon-linux taught us about frame timing on Linux. Most of it was measured, not read
anywhere, and several items cost days. `docs/ARCHITECTURE.md` has the design. This file covers what someone
measuring frames on Linux, or writing a layer, a limiter or a DXVK patch, should know first.

The numbers come from one test system unless stated otherwise:
- **GPU and desktop:** an NVIDIA GPU on driver 615, KDE Plasma 6 (KWin 6.8) on Wayland.
- **Screens:** one 240 Hz VRR screen and one 120 Hz fixed-refresh screen.
- **Proton:** GE-Proton 11 (DXVK v3.1.1-134-g2cb40186c), plus a Proton 11 build with vkd3d-proton.

Other drivers and compositors may behave differently. AMD/Mesa is untested; see the end of this file.

---

## 1. Display times need VK_EXT_present_timing

- **Present-call timestamps are not display times.** The return of `vkQueuePresentKHR` says nothing about when the
  frame reached the screen. With FIFO and a 3-image queue we measured 29-49 ms from present to first pixel; with
  mailbox a frame can be replaced and never shown at all. "Displayed fps", "until displayed", display latency and
  dropped frames must come from the driver.
- **What `VK_EXT_present_timing` gives on NVIDIA 615 / Wayland:**
  - Only the `IMAGE_FIRST_PIXEL_OUT` stage (no `FIRST_PIXEL_VISIBLE`, no `REQUEST_DEQUEUED`).
  - Only the present-stage-local time domain, which the layer calibrates against CLOCK_MONOTONIC about once a
    second.
  - `refreshDuration` reported as 0.
  - No present-at-absolute or present-at-relative time (`abs=0 rel=0`).
  - Reports leave the driver's queue when returned complete. A full queue rejects new timed presents
    (`VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT`).
- **Covered (composited) windows:** display times can sit 0.15-0.35 ms off the vblank grid. Uncovered fullscreen
  windows land within 0.05-0.1 ms. nvidia-drm does not answer `drmWaitVBlank` / `CRTC_GET_SEQUENCE`, so there is
  no kernel vblank time to check against.
- **Xwayland gives no display times at all on NVIDIA.** Use Wine's Wayland driver for Proton games
  (`PROTON_ENABLE_WAYLAND=1`, or a Proton that defaults to it).
- **The app may use present timing too.** DXVK 3.1+ requests stage feedback on every present. A layer that adds
  its own requests must share the driver's timing queue and the IDs with the app: emulate the app's queue size,
  never steal its reports. A mistake there changes DXVK's frame pacing.
- **Screens asleep (DPMS off):** KWin throttles windows and reports no display times. Measure only with the
  screens on.

## 2. Present IDs: vkd3d-proton sends them only with V-Sync on

- **Symptom:** in a D3D12 game with V-Sync off, every display column was empty, even though vkd3d-proton had
  created the swapchain with `PRESENT_ID_2` + present timing.
- **Cause:** vkd3d-proton (`swapchain.c`, `dxgi_vk_swap_chain_present_iteration`) attaches a present ID, and its
  own timing request, only to FIFO presents. Tearing (IMMEDIATE) presents have no ID, and timing reports are
  matched by ID.
- **Fix:** on ID-less presents of app-owned `PRESENT_ID_2` swapchains, the layer injects its own ID, kept above
  every ID the app has sent. If the app ever sends an ID at or below an injected one, the layer stops injecting on
  that swapchain.
- **Debugging empty display columns:** dump the ring first (`ring_dump --pid N --info`) and look at
  `present_id` / `timing_flags` before suspecting the driver.

## 3. GPU timing

- **Attribute on the GPU timeline, not by submission.** DXVK and vkd3d-proton submit the next frame's work
  before the current frame's present call. Assign GPU intervals by when they end, relative to the present's ready
  time.
- **Two calibrations disagree by 10-20 us.** Present-timing ready times and GPU timestamps both convert to
  CLOCK_MONOTONIC, through separate calibrations. Without a 0.5 ms slack (only for batches submitted before the
  present call), every frame got the previous frame's GPU work: the GPU start read exactly one frame early.
- **Overhead:** each submit batch is wrapped in two pre-recorded command buffers (query reset, two timestamps).
  This cost +0.03 ms per 4 ms GPU-bound frame (0.7 %, within noise). GPU busy matched the test app's own
  timestamps within 5-17 us.

## 4. Under DXVK the "CPU" columns describe DXVK's thread

DXVK calls `vkQueuePresentKHR` from its own thread (`dxvk-submit`), once per game `Present()`, in order:
- **Unaffected:** frame times and display times (median within 0.005 ms of the game's own `Present()` intervals).
- **Presenter-thread CPU columns:** `MsInPresentAPI`, `CPUBusy` and `CPUWait` describe the presenter thread. A
  V-Sync-bound game waits 8.3 ms inside `Present()`, but the columns show 0.04 ms in the API.
- **Latency columns, one refresh off:** with L = how long after the game's `Present()` DXVK's
  `vkQueuePresentKHR` comes, `MsUntilDisplayed` is short by L and `DisplayLatency` is long by the game's wait
  minus L.
  - DXVK 3.1.1 presents about one refresh late, so `DisplayLatency` is right and `MsUntilDisplayed` is one refresh
    short.
  - DXVK 2.5.1 presents at once, so the reverse.
  - Frame-capped without V-Sync, both are right.
- **Useful identity:** `DisplayLatency` (frame CPU start to first pixel) = `MsUntilDisplayed + MsCPUBusy`,
  exactly. Default-format CSVs without a `DisplayLatency` column still give it.

## 5. How DXVK 3.1.1+ and vkd3d-proton pace frames

Read this before writing any limiter, latency feature or presenter patch.

- **DXVK's frame thread.** `Presenter::runFrameThread` (`dxvk-frame`) handles each presented frame in order:
  1. `updatePresentTiming()` polls `vkGetPastPresentationTimingEXT` under `m_timingMutex` when present timing is
     available.
  2. It waits: either `waitUntilFrameTargetTime` (a CPU sleep to a computed target), or, for FIFO without timing,
     `vkWaitForPresent2KHR(UINT64_MAX)`.
  3. It runs `FpsLimiter::delay()`, unless it already waited.
  4. It signals the frame latency event.

  The game thread blocks on that event inside `Present()`, so delaying it holds the game **before** its next frame.
- **Present timing replaces per-frame present waits.** When present timing is available, the frame thread does
  not call `vkWaitForPresent*` per frame (we saw two calls at start-up, 100 ms timeout). With
  `DXVK_FRAME_RATE`, FIFO frames get target times `reference + (id - refId) * interval` and the frame thread sleeps
  to them. That is DXVK's limiter now.
- **`presentImage` takes `m_timingMutex` for FIFO presents.** Anything that blocks inside the timing poll also
  blocks FIFO presents. Mailbox and immediate presents don't take it.
- **vkd3d-proton.** Its wait worker calls `vkWaitForPresent2KHR(UINT64_MAX)` only for FIFO presents, then
  `VKD3D_FRAME_RATE`'s sleep, then releases the frame latency fence. With V-Sync off it waits on a timeline
  semaphore instead.

## 6. Frame caps: where the wait sits decides the latency

**The algorithm matters little.** The best known one is an absolute deadline grid, as in DXVK's `FpsLimiter`, with
a sleep and then a short spin. Lowering the thread's timer slack to 1 ns for the sleep cuts wake-up lateness from
about 50 us to a few us. Our limiter releases frames within 3 us (p99) of the deadline.

**What matters is which thread waits.** A wait inside `vkQueuePresentKHR` runs on DXVK's or vkd3d-proton's
submit thread. The next frame's work queues behind it while the game thread runs a frame ahead.

Latency at 100 fps, from the game's frame start to the first pixel (medians of 2-3 runs):

| Case | Game's own cap | Wait in the present (MangoHud late) | Translation layer's limiter | presentmon-linux |
|---|---|---|---|---|
| Native Vulkan | 8-10 ms | 8-9 ms | - | 9 ms (late) |
| DXVK, V-Sync off | 10 ms | 18-21 ms | `DXVK_FRAME_RATE` 9.5-9.9 ms | 9.7-10.6 ms (holds DXVK's timing poll) |
| DXVK, V-Sync on | 10 ms | 18.5 ms | `DXVK_FRAME_RATE` 10 ms | 19.5 ms (late; see below) |
| vkd3d-proton, V-Sync on | 7-9 ms | 28-29 ms | `VKD3D_FRAME_RATE` 8-10 ms | 8-9 ms (holds its present wait) |

- **Waiting before the present ("early") adds about a frame everywhere** (native 18 ms, DXVK 28 ms).
- **Holding the frame thread.** A layer can match the game's own latency by holding the call the translation
  layer's frame thread makes before it releases the game: the present wait (vkd3d-proton, older DXVK with V-Sync
  on) or the present-timing poll (DXVK 3.1.1+, V-Sync off).
- **Not reachable from a layer:**
  - DXVK 3.1.1+ with V-Sync on. Holding the poll there blocks FIFO presents through `m_timingMutex`; we measured
    29 ms and an uneven rate.
  - vkd3d-proton with V-Sync off.

  Use the game's own cap, `DXVK_FRAME_RATE`/`VKD3D_FRAME_RATE`, or Reflex there.
- **Engage a hold only on a steady pattern.** We require 8+ calls in a row, the last within max(100 ms, 3 frame
  intervals). DXVK's two start-up present waits once switched a cap into "hold" mode, leaving a 500 ms uncapped hole.
- **No backstop wait on the present thread while holding.** A safety wait at 0.95 x the interval on DXVK's submit
  thread became the pacer itself: presents every 9.5 ms, the hold re-anchoring every frame, the rate drifting to
  105 fps.
- **Judge a hold by the average present interval over 32 presents, skipping the first window.** Frames queued
  before the switch drain in the first window: it read 9.2 ms for a 10 ms cap.
- **Reflex (NV_low_latency2) on NVIDIA 615:**
  - The driver enforces `minimumIntervalUs` even with `lowLatencyMode` false, about 0.5 % long (10.055 ms for
    10000 us, so a 100 fps cap gives 99.5 fps).
  - To cap through it, raise the game's own interval to `max(game, ceil(1e6 / cap))`, so a game's lower cap stays.
  - vk.xml marks no external synchronization on these commands, so another thread may change the sleep mode.
  - For a native Vulkan game a present-side wait already equals a game-thread wait, so Reflex gains nothing there.
- **A Reflex interval evens out frame starts, not presents.** In a vkd3d-proton game (Spider-Man 2, V-Sync off, VRR,
  60 fps cap through `minimumIntervalUs`), frame starts were paced, but the game's per-frame CPU work varied by
  several ms. Presents, and under VRR the screen, came 11-27 ms apart: 67 % of frames jumped more than 2 ms and
  the animation error averaged 3.6 ms. An early present-side wait (`--placement early --no-reflex`) put presents on
  an even grid. Jumps fell to 16-30 %, animation error to 0.9-1.4 ms, and pacing went from 67 to 89-95, at the cost
  of about a frame of latency. For games with uneven CPU work per frame, pace the presents, not only the starts.
  presentmon-linux now uses the Reflex interval only with late placement; early always waits at the present.
- **The "just under VRR maximum" cap.** `floor(hz - hz^2/3600)` (224 at 240 Hz, 138 at 144) is a community
  estimate; we found no NVIDIA source.
- **Rate accuracy:** an absolute grid holds the rate exactly (0.000 %). MangoHud's relative sleep runs 0.003-0.007 %
  slow, NVIDIA's Reflex interval 0.5 % slow.
- **Frame-to-frame jitter** (sd of the app's present intervals): ours 0.011-0.028 ms, MangoHud's 0.026-0.031 ms.
  On screen, run-to-run noise (0.14-0.69 ms under VRR) is larger than either.

## 7. VRR on KWin

- **KWin ("Automatic" policy) enables VRR only while the active window is fullscreen on that output.** A click on
  any other window, even on another screen, switches VRR off mid-game. A test window that cannot take focus
  never gets VRR. Read the CRTC's `VRR_ENABLED` property (libdrm, read-only) to know the real state rather than
  guessing from timing.
- **Another output's activity can disturb VRR pacing.** On NVIDIA, an animated wallpaper rendering at 15 fps on the
  second screen made every ~10th frame on the VRR screen arrive late, at multiples of 66.7 ms. The game's own
  frame times were steady. Pausing the animation restored even pacing. It was not HDR, not the cables, and not the
  other screen's VRR setting. If VRR pacing is uneven while the game is steady, look at what else renders.
- **There is no standard for "VRR stability".** Our score counts screen-interval jumps larger than
  max(2 ms, 25 % of the median). A 1.2 ms threshold counted the games' normal noise. Fixed refresh can be told
  apart from VRR by screen intervals snapping to a refresh grid while the game's don't (try the lowest common rate
  first: a 120 Hz grid also fits 240 Hz).

## 8. Scoring the experience

- **GQI** (`1% low^2 / (60 x average fps)`) rewards raw frame rate without bound. An uncapped 975 fps run on a
  120 Hz screen scored 15.9 against about 2 for the same screen output capped. Its 60 fps reference ignores the
  display.
- **Use what reached the screen.** A V-Sync queue hides a 30 ms CPU hitch from the screen cadence: only
  `MsAnimationError` shows it. A 100 ms freeze moves screen and simulation together, so the animation error stays
  about 0. Detecting stutter needs both signals.
- **One missed refresh is judder, not a hitch.** A frame shown for exactly two refreshes (90 fps on 120 Hz) must
  not count as a stutter, or judder is penalised twice.
- **Report consistency separately from frame rate.** Players mean "frames come evenly" by smooth. A score that also
  rewards frame rate makes a steady 60 fps look like a defect. presentmon-linux shows a pacing score (judder and
  stutters only) next to the overall one.
- **Pacing needs its own scale.** Judder and stutter weighed lightly inside an overall score (fine next to frame
  rate and latency) compress a standalone pacing score: a visibly juddery 12/21 ms alternation still scored 89.
  At 3x it scores 67, against 89-95 for the same game evenly paced.
- **The 1% low / average ratio misses alternation.** Two runs of one game, one alternating 12/21 ms and one even, both
  read 0.71 on present times. Only screen intervals and animation error separate them.
- **Perception research gives anchors, not a full model.**
  - QoE is about linear in frame time and flattens above 75-90 fps.
  - Swings of about 2 ms go unnoticed.
  - Spike size matters, not frequency. Spikes under about 50 ms are acceptable, 150 ms or more hurt.
  - ITU-T G.1072 gives an input-delay curve.

  No study weighs smoothness against latency. That weight is a design choice; say so. Formula, citations and
  validation: `docs/ARCHITECTURE.md` 3.4.3.

## 9. Measuring well

- **Latency on a composited fixed-refresh window varies by up to ~12 ms between runs.** One `DXVK_FRAME_RATE` run
  read 22 ms, the repeat 10. Compare medians of 3 runs.
- **Keep test windows from disturbing the desktop.** Proton test windows without a Steam app ID get the class
  `steam_proton`: give them a window rule, or they open on the main screen. A covered or unfocused test window
  never gets VRR.
- **`MANGOHUD_CONFIG` replaces the user's MangoHud config file** (unless `read_cfg` is in it). To compare against
  MangoHud's limiter alone: `MANGOHUD=1 MANGOHUD_CONFIG=no_display,fps_limit=N,fps_limit_method=late|early`.
- **Use a test app whose frames have a known cause.** `vk_truth` logs its own timestamps for every frame
  (`--log`). It can cap itself (`--fps`), add CPU/GPU load (`--cpu-ms`, `--gpu-ms`), periodic hitches
  (`--stutter-every N --stutter-ms M`) and random jitter (`--jitter-ms J`), and use Reflex (`--ll2`). Validate a
  metric against those known causes before trusting it on a game.

## AMD / Mesa

Untested so far: the project was developed on NVIDIA. Mesa's RADV has `VK_EXT_present_timing` on Wayland from
Mesa 26.1. Without it, display columns read NA and the scores stay empty, while frame times, CPU/GPU numbers and
the frame cap still work. AMD's Anti-Lag (`VK_AMD_anti_lag`, with a `maxFPS` field) would be the counterpart of the
Reflex cap path; it is not implemented yet. Reports from AMD systems are welcome.
