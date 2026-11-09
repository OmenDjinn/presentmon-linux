// SPDX-License-Identifier: MIT
// presentmon-linux shim. Upstream's mc/*.h do #include "../qpc.h", but the file is
// CommonUtilities/Qpc.h and Linux is case-sensitive. "../qpc.h" is resolved through the
// include directory shim/qpc_anchor/ (a sibling of this file), so that
// <shim/qpc_anchor>/../qpc.h == this header. It forwards to the real Qpc.h.
#pragma once

#include <IntelPresentMon/CommonUtilities/Qpc.h>
