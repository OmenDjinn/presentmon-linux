# pml_metrics: PresentMon's unified metrics library (mc/) compiled unmodified on Linux, plus
# the pml::FrameRecord -> FrameData adapter and the per-swapchain engine. Static + PIC: it is
# linked into the presentmon CLI and into the Vulkan layer .so.
#
# How the Windows-only parts of the upstream sources are handled without touching them:
#   * <PresentData/PresentMonTraceConsumer.hpp> is shadowed by shim/PresentData/ (the shim
#     directory comes first on the include path).
#   * "../qpc.h" (wrong case in upstream) resolves through shim/qpc_anchor/ -> shim/qpc.h.
#   * MetricsTypes.cpp (only FrameData::CopyFrameData, ETW PresentEvent -> FrameData) and
#     FrameMetricsMemberMap.h (needs PresentMonAPI2) are not compiled; nothing calls them.
#   * Qpc.cpp is replaced by src/qpc_converter.cpp (the 4 QpcConverter methods, verbatim).
set(PML_MC_DIR ${PML_PRESENTMON_DIR}/IntelPresentMon/CommonUtilities/mc)
foreach(f ${PML_MC_DIR}/UnifiedSwapChain.cpp ${PML_PRESENTMON_DIR}/PresentData/PresentEventEnums.hpp)
	if(NOT EXISTS ${f})
		message(FATAL_ERROR "pml_metrics: missing ${f} (PresentMon submodule not checked out?)")
	endif()
endforeach()

set(PML_MC_SOURCES
	AnimationErrorTracker.cpp
	DisplayFrameQueue.cpp
	MetricsCalculator.cpp
	MetricsCalculatorAnimation.cpp
	MetricsCalculatorCpuGpu.cpp
	MetricsCalculatorDisplay.cpp
	MetricsCalculatorInput.cpp
	MetricsCalculatorInstrumented.cpp
	MetricsCalculatorInterFrameEvent.cpp
	SwapChainState.cpp
	UnifiedSwapChain.cpp)
list(TRANSFORM PML_MC_SOURCES PREPEND ${PML_MC_DIR}/)

add_library(pml_metrics STATIC
	${PML_MC_SOURCES}
	src/qpc_converter.cpp
	src/adapter.cpp
	src/engine.cpp
	src/summary.cpp
	include/pml/metrics/adapter.hpp
	include/pml/metrics/engine.hpp
	include/pml/metrics/summary.hpp)

# Upstream sources are not ours: no warning flags for them.
set_source_files_properties(${PML_MC_SOURCES} src/qpc_converter.cpp PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(src/adapter.cpp src/engine.cpp src/summary.cpp PROPERTIES COMPILE_OPTIONS "${PML_WARNINGS}")

# Order matters: the shim must come before the PresentMon roots.
target_include_directories(pml_metrics PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${CMAKE_CURRENT_SOURCE_DIR}/shim
	${CMAKE_CURRENT_SOURCE_DIR}/shim/qpc_anchor
	${PML_PRESENTMON_DIR}                              # <PresentData/...>, <IntelPresentMon/...>
	${PML_PRESENTMON_DIR}/IntelPresentMon)             # <CommonUtilities/mc/MetricsTypes.h>
target_link_libraries(pml_metrics PUBLIC pml_common)
# cnr/FixedVector.h uses std::from_range_t, which is C++23 (<ranges>). GCC 16 in -std=c++20 mode
# lacks it, so every consumer of the metrics headers has to build as C++23 (CMake picks
# max(CMAKE_CXX_STANDARD, this)). Only this requirement is C++23; our own headers are C++20 code.
target_compile_features(pml_metrics PUBLIC cxx_std_23)
set_target_properties(pml_metrics PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(PML_BUILD_TESTS)
	add_executable(pml_metrics_tests
		tests/adapter_tests.cpp
		tests/timeline_tests.cpp
		tests/summary_tests.cpp
		tests/test_util.hpp)
	target_link_libraries(pml_metrics_tests PRIVATE pml_metrics pml_vulkan_headers GTest::gtest_main)
	target_compile_options(pml_metrics_tests PRIVATE ${PML_WARNINGS})
	add_test(NAME pml_metrics_tests COMMAND pml_metrics_tests)

	# The library must be usable inside a shared object (the Vulkan layer): link a tiny .so
	# with --no-undefined, then load it from a test executable.
	add_library(pml_metrics_probe SHARED tests/probe_so.cpp)
	target_link_libraries(pml_metrics_probe PRIVATE pml_metrics)
	target_link_options(pml_metrics_probe PRIVATE -Wl,--no-undefined -Wl,--exclude-libs,ALL)
	target_compile_options(pml_metrics_probe PRIVATE ${PML_WARNINGS})
	set_target_properties(pml_metrics_probe PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
		CXX_VISIBILITY_PRESET hidden)

	add_executable(pml_metrics_so_test tests/so_test.cpp)
	target_link_libraries(pml_metrics_so_test PRIVATE pml_metrics GTest::gtest_main ${CMAKE_DL_LIBS})
	target_compile_definitions(pml_metrics_so_test PRIVATE PML_PROBE_SO="$<TARGET_FILE:pml_metrics_probe>")
	target_compile_options(pml_metrics_so_test PRIVATE ${PML_WARNINGS})
	add_dependencies(pml_metrics_so_test pml_metrics_probe)
	add_test(NAME pml_metrics_so_test COMMAND pml_metrics_so_test)
endif()
