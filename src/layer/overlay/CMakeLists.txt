# In-game overlay (Dear ImGui), linked into VkLayer_presentmon as target pml_overlay.
# See docs/ARCHITECTURE.md section 3.4 and src/layer/overlay_seam.hpp.
#
# Targets
#   pml_imgui          Dear ImGui core + Vulkan backend (no prototypes: loaded through the layer's dispatch)
#   pml_overlay_core   rolling statistics on top of pml_metrics, dump-file encoders (no Vulkan calls, no ImGui)
#   pml_overlay        seam implementation + ImGui layout (what the layer links)
#   pml_overlay_tests  unit tests (stats, encoders, headless UI)
#   overlay_ctl        writes OverlayControl into a process's segment (test helper for the future CLI)
find_program(PML_GLSLANG glslangValidator)
if(NOT PML_GLSLANG)
	message(FATAL_ERROR "The overlay needs glslangValidator to build its shaders")
endif()

set(PML_OVERLAY_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${PML_OVERLAY_GEN_DIR})

# --- generated headers: font bytes and the four colour-mode fragment shaders ------------------
set(PML_OVERLAY_FONT ${PML_IMGUI_DIR}/misc/fonts/Cousine-Regular.ttf)
add_custom_command(
	OUTPUT ${PML_OVERLAY_GEN_DIR}/ov_font.h
	COMMAND ${CMAKE_COMMAND} -DIN=${PML_OVERLAY_FONT} -DOUT=${PML_OVERLAY_GEN_DIR}/ov_font.h -DNAME=ov_font
	        -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
	DEPENDS ${PML_OVERLAY_FONT} ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
	COMMENT "embedding Cousine-Regular.ttf"
	VERBATIM)
set(PML_OVERLAY_GENERATED ${PML_OVERLAY_GEN_DIR}/ov_font.h)
foreach(mode 0 1 2 3)
	set(out ${PML_OVERLAY_GEN_DIR}/ov_frag_mode${mode}.h)
	add_custom_command(
		OUTPUT ${out}
		COMMAND ${PML_GLSLANG} -V --target-env vulkan1.0 -DMODE=${mode} --vn spv_overlay_frag_mode${mode} -o ${out}
		        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/overlay.frag
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/overlay.frag
		COMMENT "glslang overlay.frag MODE=${mode}"
		VERBATIM)
	list(APPEND PML_OVERLAY_GENERATED ${out})
endforeach()

# --- Dear ImGui -------------------------------------------------------------------------------
add_library(pml_imgui STATIC
	${PML_IMGUI_DIR}/imgui.cpp
	${PML_IMGUI_DIR}/imgui_draw.cpp
	${PML_IMGUI_DIR}/imgui_tables.cpp
	${PML_IMGUI_DIR}/imgui_widgets.cpp
	${PML_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(pml_imgui SYSTEM PUBLIC ${PML_IMGUI_DIR} ${PML_IMGUI_DIR}/backends)
target_compile_definitions(pml_imgui PUBLIC
	IMGUI_USER_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/imconfig_pml.h"
	IMGUI_IMPL_VULKAN_NO_PROTOTYPES
	VK_NO_PROTOTYPES)
target_compile_options(pml_imgui PRIVATE -w -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_libraries(pml_imgui PUBLIC pml_vulkan_headers)
set_target_properties(pml_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- statistics and dump encoders --------------------------------------------------------------
add_library(pml_overlay_core STATIC stats.cpp dump.cpp stats.hpp dump.hpp)
target_include_directories(pml_overlay_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(pml_overlay_core SYSTEM PRIVATE ${PML_PYROFLING_DIR}/Granite/third_party/stb)
target_compile_options(pml_overlay_core PRIVATE ${PML_WARNINGS} -fvisibility=hidden -fvisibility-inlines-hidden)
target_compile_definitions(pml_overlay_core PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(pml_overlay_core PUBLIC pml_metrics pml_common pml_vulkan_headers)

# --- the overlay proper ------------------------------------------------------------------------
add_library(pml_overlay STATIC overlay.cpp ui.cpp ui.hpp ${PML_OVERLAY_GENERATED})
target_include_directories(pml_overlay PRIVATE ${PML_OVERLAY_GEN_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(pml_overlay PRIVATE ${PML_WARNINGS} -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_libraries(pml_overlay PUBLIC pml_overlay_core pml_imgui pml_layer_core)

if(PML_BUILD_TESTS)
	add_executable(pml_overlay_tests tests/overlay_tests.cpp ui.cpp)
	target_link_libraries(pml_overlay_tests PRIVATE pml_overlay_core pml_imgui GTest::gtest_main)
	target_compile_options(pml_overlay_tests PRIVATE ${PML_WARNINGS})
	add_test(NAME pml_overlay_tests COMMAND pml_overlay_tests)

	add_executable(pml_overlay_ctl tests/overlay_ctl.cpp)
	target_link_libraries(pml_overlay_ctl PRIVATE pml_common)
	target_compile_options(pml_overlay_ctl PRIVATE ${PML_WARNINGS})
	set_target_properties(pml_overlay_ctl PROPERTIES OUTPUT_NAME overlay_ctl)

	# Real-GPU tests with vk_truth (needs the layer, ring_dump, vk_truth; skipped otherwise).
	find_program(PML_PYTHON3 python3)
	if(PML_PYTHON3)
		add_test(NAME pml_overlay_live
			COMMAND ${PML_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/tests/overlay_live.py --build-dir ${CMAKE_BINARY_DIR})
		# Windows on the real desktop: timing checks fail if another window test runs at the same time.
		set_tests_properties(pml_overlay_live PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 RESOURCE_LOCK desktop LABELS live)
	endif()
endif()
