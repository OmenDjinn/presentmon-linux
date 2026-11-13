# Third-party notices

presentmon-linux stands on other people's work. These projects are git submodules under `third_party/`, each with
its full license text there; parts of them are compiled into the binaries.

## Intel PresentMon

- https://github.com/GameTechDev/PresentMon, `third_party/PresentMon`, MIT License,
  Copyright (C) 2017-2024 Intel Corporation.
- The CLI and the overlay run PresentMon's own metrics code (`UnifiedSwapChain` and the frame metrics in
  `IntelPresentMon/CommonUtilities/mc`), compiled unmodified, so the numbers and the CSV columns match PresentMon's.
  The CSV writer follows PresentMon's console application, and its gold test files check the output.
- The metric names and definitions (MsBetweenPresents, MsAnimationError, DisplayLatency, ...) are Intel's.
- presentmon-linux is not affiliated with or endorsed by Intel.

## PyroFling

- https://github.com/Themaister/pyrofling, `third_party/pyrofling`, MIT License,
  Copyright (c) 2023-2025 Hans-Kristian Arntzen.
- The layer measures display times with `VK_EXT_present_timing` the way PyroFling's latency measurement layer does.
  Its calibration of CLOCK_MONOTONIC against present-stage-local time is based on `Swapchain::updateFeedback()` in
  `latency_measurement_layer.cpp`.
- The layer's dispatch tables are its own, modelled on PyroFling's `layer-util`.
- PyroFling's Granite submodule provides the Vulkan headers and `stb_image_write.h` used by the build.

## Dear ImGui

- https://github.com/ocornut/imgui, `third_party/imgui`, MIT License, Copyright (c) 2014-2026 Omar Cornut.
- It draws the in-game overlay and is compiled unmodified.
- The font Cousine (Steve Matteson, Copyright (c) 2010 Google Corporation, SIL Open Font License 1.1) is embedded
  in the overlay from imgui's `misc/fonts`.

## Others

- **stb_image_write.h:** Sean Barrett, public domain or MIT. Only the overlay's test hook (`PRESENTMON_OVERLAY_DUMP`)
  uses it.
- **Vulkan headers:** The Khronos Group Inc., Apache-2.0 OR MIT, through Granite.

## Metrics by others

- **GQI:** the Gaming Quality Index is Blackbird PC Tech's metric.
