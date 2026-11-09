# presentmon (collector CLI)

`presentmon` reads the shared-memory rings of processes that run `VkLayer_presentmon`
(`PRESENTMON_LAYER=1`), computes PresentMon's metrics with `pml_metrics` and writes PresentMon-format
CSV plus a once-a-second console view. It also lists layer-enabled processes and controls their
in-game overlay.

Build: `cmake -S . -B build/b2-collector -G Ninja -DPML_BUILD_COLLECTOR=ON -DPML_BUILD_METRICS=ON`
then `cmake --build build/b2-collector`; the executable is `<build>/bin/presentmon`.

## Capture (default)

    presentmon [capture] [options]

Option spelling follows PresentMon's console app (`--x`, `-x` and `/x`, case-insensitive).

| Option | Meaning |
|---|---|
| `--process_name N` (repeatable), `--exclude N`, `--process_id P` | Which processes. Names ignore case, directory and extension (`Foo.exe` = `foo`). |
| `--output_file path`, `--output_stdout`, `--no_csv`, `--multi_csv` | Where the CSV goes. Default file: `PresentMon-YYYY-MM-DDTHHMMSS.csv` in the current directory; `--multi_csv` adds `-NAME-PID` before the extension. The file starts with a UTF-8 BOM (stdout does not). |
| `--v1_metrics`, `--v2_metrics` | Column set. Without either: the v2 metrics under the old (1.x/2.0) column names. |
| `--qpc_time`, `--qpc_time_ms`, `--date_time` | Time unit. `--qpc_time` prints raw `CLOCK_MONOTONIC` nanoseconds. |
| `--track_frame_type`, `--track_app_timing`, `--track_pc_latency`, `--track_hybrid_present`, `--track_gpu_video` | Optional columns. |
| `--no_track_gpu`, `--no_track_display`, `--no_track_input` | Drop column groups (`--no_track_display` only with `--no_track_gpu`, as upstream). |
| `--exclude_dropped`, `--write_display_metadata`, `--write_frame_id`, `--write_display_time` | As upstream. |
| `--no_console_stats` | No statistics block (also implied by `--output_stdout`). |
| `--delay S`, `--timed S`, `--terminate_after_timed`, `--terminate_on_proc_exit` | Recording window and exit conditions. Without `--terminate_after_timed` the program keeps running after `--timed` ends the recording. |

Windows/ETW options (`--etl_file`, `--hotkey`, `--session_name`, ...) are recognised and rejected with
an explanation; anything else unknown is an error. Ctrl+C, SIGTERM, SIGHUP and SIGQUIT flush and close the CSV
(a signal the parent left ignored, e.g. SIGHUP under nohup or SIGINT in a script's `&` job, stays ignored: use SIGTERM).

Linux values in the columns: `Application` = process name from the segment (the Windows exe name for
Wine), `SwapChainAddress` = the `VkSwapchainKHR` handle, `PresentRuntime` = `DXVK` / `VKD3D` / `Vulkan`,
`PresentMode` = the Vulkan present mode (`FIFO`, `MAILBOX`, ...). Columns for data that does not exist
yet print what upstream prints for absent data: GPU columns `0.0000` until M2, input / flip delay /
instrumented / PC latency `NA`.

Behaviour worth knowing:

* Capture starts now: the ring of a process that already runs is read from its newest record. A process
  that appears after the capture started is read from its first record.
* In v2 mode a displayed frame is reported when the next displayed one is known, so the last frame is
  never written (PresentMon does not write it either). v1 mode writes one row per present.
* The console block (stdout) is redrawn in place on a terminal and appended once per interval when
  piped. It shows displayed and presented fps, average frame time, CPU busy, display latency, dropped
  % and lost records per swapchain.
* Stale segments (producer dead and heartbeat older than 10 s, or marked exited) are unlinked and logged;
  live ones are never touched.
* Log: stderr, `PRESENTMON_LOG=error|warn|info|debug` (default `info`), prefix `[presentmon] `.

## list

    presentmon list [--process_name N] [--process_id P]

One block per live layer-enabled process: engine, layer version, heartbeat age, overlay state
(`visible|hidden, position, scale, available|not available`), frame cap (setting and what is in force), ring
statistics and each swapchain (size,
format, colour space, present mode, surface, supported/used present stages, timing flags).

## overlay

    presentmon overlay show|hide|toggle|status [--process_name N] [--process_id P]
                       [--position top-left|top-right|bottom-left|bottom-right] [--scale F|auto]

Writes the `OverlayControl` area of every matching live segment (all of them without a filter) and prints
the resulting state. `status` only reads. A process whose overlay is not loaded
(`PRESENTMON_OVERLAY=off`) shows `not available`; show/hide still store the value and warn. Exit code 1
when nothing matched.

## cap

    presentmon cap <fps>|auto|off|status [--process_name N] [--process_id P] [--placement late|early]
                   [--max F] [--reflex|--no-reflex]

Writes the settings half of the `CapControl` area of every matching live segment (all of them without a filter),
then waits up to 500 ms for the layer to apply it (it echoes the change's generation with its status on its next
present) and prints the setting and what is in force (rate, method: Reflex interval, hold on the game's frame
thread, present wait late/early; mean wait, p99 release lateness; for auto the VRR output's refresh). A game that
does not present in that time prints `not applied yet`. `--max` bounds `auto`; placement and Reflex are kept by a
later change of the rate. `status` only reads. Exit code 1 when nothing matched or on a bad value (1-10000 fps).
Design: `docs/ARCHITECTURE.md` 3.4.2.

## Tests

`ctest` target `pml_collector_tests` (no GPU): CSV headers against PresentMon's gold files, the gold first
rows rebuilt from their values, option parsing, and end-to-end runs of the executable against
`pml_fake_producer` (a real segment with a deterministic timeline, `tests/fake_timeline.hpp`): v1/v2/default
CSV values computed by hand, NA columns, dropped frames, console stats, lost records, list, an overlay
toggle and a cap round trip through the segment, stale cleanup, `--timed`, `--delay`, `--terminate_on_proc_exit`,
SIGINT/SIGTERM. Hidden options for tests: `--segment NAME`, `--discovery_ms`, `--stats_interval_ms`.
