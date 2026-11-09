cmake_minimum_required(VERSION 3.25)
project(presentmon_linux VERSION 0.1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)  # PresentMon's mc/ code needs std::from_range_t (C++23 in GCC 16)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
# Everything may end up inside the Vulkan layer .so (metrics code is linked into the overlay).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)

# Executables land in <build>/bin. The layer puts its .so and the build-tree manifest in
# <build>/layer (see src/layer/CMakeLists.txt) so VK_ADD_IMPLICIT_LAYER_PATH=<build>/layer works.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---------------------------------------------------------------------------------------
# Components. Each one can be switched off so parts of the tree can be built and tested on
# their own (each component also only gets added once its CMakeLists.txt exists).
# ---------------------------------------------------------------------------------------
option(PML_BUILD_METRICS   "PresentMon metrics library (src/metrics)" ON)
option(PML_BUILD_LAYER     "Vulkan layer (src/layer)" ON)
option(PML_BUILD_OVERLAY   "In-game overlay inside the layer (src/layer/overlay)" ON)
option(PML_BUILD_COLLECTOR "presentmon CLI (src/collector)" ON)
option(PML_BUILD_GUI       "presentmon-gui desktop app (src/gui; needs Qt 6, Kirigami, KGlobalAccel and the overlay)" ON)
option(PML_BUILD_TESTS     "Unit tests (src/*/tests, tests/unit)" ON)
option(PML_BUILD_TEST_APPS "Test applications (tests/apps)" ON)
option(PML_BUILD_E2E       "End-to-end tests (tests/e2e)" ON)

# ---------------------------------------------------------------------------------------
# Third-party locations (git submodules; never modified)
# ---------------------------------------------------------------------------------------
set(PML_THIRD_PARTY_DIR    ${CMAKE_SOURCE_DIR}/third_party)
set(PML_PRESENTMON_DIR     ${PML_THIRD_PARTY_DIR}/PresentMon)
set(PML_PYROFLING_DIR      ${PML_THIRD_PARTY_DIR}/pyrofling)
set(PML_IMGUI_DIR          ${PML_THIRD_PARTY_DIR}/imgui)
set(PML_VULKAN_HEADERS_DIR ${PML_PYROFLING_DIR}/Granite/third_party/khronos/vulkan-headers/include)

foreach(dir ${PML_PRESENTMON_DIR}/IntelPresentMon ${PML_PYROFLING_DIR}/layer-util ${PML_VULKAN_HEADERS_DIR}/vulkan)
	if(NOT EXISTS ${dir})
		message(FATAL_ERROR "Missing ${dir}. Run: git submodule update --init third_party/PresentMon "
		                    "third_party/pyrofling third_party/imgui && "
		                    "git -C third_party/pyrofling submodule update --init --recursive Granite")
	endif()
endforeach()

# Vulkan headers v1.4.363 from Granite (they have VK_EXT_present_timing). Use these instead of
# the system headers everywhere.
add_library(pml_vulkan_headers INTERFACE)
target_include_directories(pml_vulkan_headers SYSTEM INTERFACE ${PML_VULKAN_HEADERS_DIR})

# Warning flags for our own code (not for third-party sources).
set(PML_WARNINGS -Wall -Wextra -Wno-missing-field-initializers)

enable_testing()
if(PML_BUILD_TESTS)
	find_package(GTest REQUIRED)
endif()

macro(pml_add_component option dir)
	if(${option} AND EXISTS ${CMAKE_SOURCE_DIR}/${dir}/CMakeLists.txt)
		add_subdirectory(${dir})
	endif()
endmacro()

# The layer <-> reader contract. Always built.
add_subdirectory(src/common)

pml_add_component(PML_BUILD_METRICS   src/metrics)
pml_add_component(PML_BUILD_LAYER     src/layer)       # adds src/layer/overlay itself when PML_BUILD_OVERLAY
pml_add_component(PML_BUILD_COLLECTOR src/collector)
pml_add_component(PML_BUILD_GUI       src/gui)         # after src/layer: reuses pml_overlay_core
pml_add_component(PML_BUILD_TESTS     tests/unit)
pml_add_component(PML_BUILD_TEST_APPS tests/apps)
pml_add_component(PML_BUILD_E2E       tests/e2e)
