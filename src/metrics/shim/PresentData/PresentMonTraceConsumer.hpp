// SPDX-License-Identifier: MIT
// presentmon-linux shim. Shadows PresentData/PresentMonTraceConsumer.hpp, which upstream's
// mc/*.cpp include but which drags in <windows.h> and ETW. Of the mc sources only
// MetricsTypes.cpp (FrameData::CopyFrameData, the ETW PresentEvent -> FrameData copy) really
// uses PresentEvent; that file is not compiled here. The rest need nothing from this header.
#pragma once

#include <cstdint>
#include <memory>

struct PresentEvent;
