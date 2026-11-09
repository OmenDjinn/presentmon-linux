# Test applications. vk_truth: a native Vulkan app whose frame timing is known.
find_package(SDL3 REQUIRED CONFIG)
find_library(PML_VULKAN_LOADER vulkan REQUIRED)
find_program(PML_GLSLANG glslangValidator REQUIRED)

set(PML_SHADER_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${PML_SHADER_GEN_DIR})

# Compile GLSL to SPIR-V at build time and embed it as a uint32_t array header.
function(pml_compile_shader src var out_list)
	set(out ${PML_SHADER_GEN_DIR}/${var}.h)
	add_custom_command(
		OUTPUT ${out}
		COMMAND ${PML_GLSLANG} -V --target-env vulkan1.3 --vn ${var} -o ${out} ${CMAKE_CURRENT_SOURCE_DIR}/${src}
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
		COMMENT "glslang ${src}"
		VERBATIM)
	set(${out_list} ${${out_list}} ${out} PARENT_SCOPE)
endfunction()

set(PML_SHADER_HEADERS)
pml_compile_shader(shaders/fullscreen.vert spv_fullscreen_vert PML_SHADER_HEADERS)
pml_compile_shader(shaders/pattern.frag    spv_pattern_frag    PML_SHADER_HEADERS)
pml_compile_shader(shaders/load.comp       spv_load_comp       PML_SHADER_HEADERS)

add_executable(vk_truth vk_truth.cpp ${PML_SHADER_HEADERS})
target_include_directories(vk_truth PRIVATE ${PML_SHADER_GEN_DIR})
target_compile_options(vk_truth PRIVATE ${PML_WARNINGS})
target_link_libraries(vk_truth PRIVATE pml_common pml_vulkan_headers SDL3::SDL3 ${PML_VULKAN_LOADER})

# d3d11_truth.exe: a D3D11 game loop for runs under Wine/Proton + DXVK (tests/e2e/test_proton.py). Cross-built
# with mingw-w64 when it is installed: GUI subsystem (no console window), everything static, so the prefix needs
# no extra DLLs.
find_program(PML_MINGW_CXX x86_64-w64-mingw32-g++)
if(PML_MINGW_CXX)
	set(PML_D3D11_TRUTH ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/d3d11_truth.exe)
	add_custom_command(
		OUTPUT ${PML_D3D11_TRUTH}
		COMMAND ${PML_MINGW_CXX} -std=c++17 -O2 -Wall -Wextra -mwindows -static -static-libgcc -static-libstdc++
		        -o ${PML_D3D11_TRUTH} ${CMAKE_CURRENT_SOURCE_DIR}/d3d11_truth.cpp -ld3d11 -ldxgi -luser32 -lgdi32
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/d3d11_truth.cpp
		COMMENT "mingw-w64 d3d11_truth.exe"
		VERBATIM)
	add_custom_target(d3d11_truth ALL DEPENDS ${PML_D3D11_TRUTH})
	# d3d12_truth.exe: the same for vkd3d-proton (tests/e2e/test_proton.py, D3D12 cases).
	set(PML_D3D12_TRUTH ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/d3d12_truth.exe)
	add_custom_command(
		OUTPUT ${PML_D3D12_TRUTH}
		COMMAND ${PML_MINGW_CXX} -std=c++17 -O2 -Wall -Wextra -mwindows -static -static-libgcc -static-libstdc++
		        -o ${PML_D3D12_TRUTH} ${CMAKE_CURRENT_SOURCE_DIR}/d3d12_truth.cpp -ld3d12 -ldxgi -luser32
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/d3d12_truth.cpp
		COMMENT "mingw-w64 d3d12_truth.exe"
		VERBATIM)
	add_custom_target(d3d12_truth ALL DEPENDS ${PML_D3D12_TRUTH})
else()
	message(STATUS "tests/apps: x86_64-w64-mingw32-g++ not found, d3d11_truth.exe (Proton tests) not built")
endif()
