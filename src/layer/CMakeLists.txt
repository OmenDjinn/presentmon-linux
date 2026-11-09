# VkLayer_presentmon: the implicit Vulkan layer. See docs/ARCHITECTURE.md, section 3.1.
#
# Targets
#   pml_layer_core         static: present-timing broker, pNext-chain patching, logging (no GPU needed)
#   VkLayer_presentmon     the layer .so, output in <build>/layer next to VkLayer_presentmon.json
#   pml_layer_tests        unit tests (fake driver)
#   pml_ring_dump          prints the records of a process's shared-memory ring (bin/ring_dump)
find_package(Threads REQUIRED)

add_library(pml_layer_core STATIC
	timing_broker.cpp
	gpu_intervals.cpp
	present_chain.cpp
	process_segment.cpp
	log.cpp)
target_include_directories(pml_layer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(pml_layer_core PUBLIC VK_NO_PROTOTYPES)
target_compile_options(pml_layer_core PRIVATE ${PML_WARNINGS})
target_link_libraries(pml_layer_core PUBLIC pml_common pml_vulkan_headers Threads::Threads)

# The overlay (src/layer/overlay, owned by another component) implements the functions declared in
# overlay_seam.hpp. Without it the layer links the no-op implementation in overlay_stub.cpp.
set(PML_LAYER_OVERLAY_TARGET "")
if(PML_BUILD_OVERLAY AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/overlay/CMakeLists.txt)
	add_subdirectory(overlay)
	set(PML_LAYER_OVERLAY_TARGET pml_overlay)
endif()

add_library(VkLayer_presentmon SHARED
	layer.cpp
	gpu_timer.cpp
	vk_dispatch.cpp)
if(NOT PML_LAYER_OVERLAY_TARGET)
	target_sources(VkLayer_presentmon PRIVATE overlay_stub.cpp)
endif()
target_compile_options(VkLayer_presentmon PRIVATE ${PML_WARNINGS} -fvisibility=hidden -fvisibility-inlines-hidden)
target_compile_definitions(VkLayer_presentmon PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(VkLayer_presentmon PRIVATE pml_layer_core ${PML_LAYER_OVERLAY_TARGET})
target_link_options(VkLayer_presentmon PRIVATE -Wl,--no-undefined -Wl,--exclude-libs,ALL)
set_target_properties(VkLayer_presentmon PROPERTIES
	LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/layer
	OUTPUT_NAME VkLayer_presentmon
	PREFIX "lib")

# Build-tree manifest (relative library_path) and install manifest (absolute library_path, written at
# install time so it follows --prefix / DESTDIR and the install libdir).
set(PML_LAYER_API_VERSION "1.4.363")
set(PML_LAYER_LIBRARY_PATH "./libVkLayer_presentmon.so")
configure_file(VkLayer_presentmon.json.in ${CMAKE_BINARY_DIR}/layer/VkLayer_presentmon.json @ONLY)

install(TARGETS VkLayer_presentmon LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(CODE "
	set(_prefix \"\${CMAKE_INSTALL_PREFIX}\")
	set(PML_LAYER_API_VERSION \"${PML_LAYER_API_VERSION}\")
	if(IS_ABSOLUTE \"${CMAKE_INSTALL_LIBDIR}\")
		set(PML_LAYER_LIBRARY_PATH \"${CMAKE_INSTALL_LIBDIR}/libVkLayer_presentmon.so\")
	else()
		set(PML_LAYER_LIBRARY_PATH \"\${_prefix}/${CMAKE_INSTALL_LIBDIR}/libVkLayer_presentmon.so\")
	endif()
	file(READ \"${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_presentmon.json.in\" _manifest)
	string(REPLACE \"@PML_LAYER_LIBRARY_PATH@\" \"\${PML_LAYER_LIBRARY_PATH}\" _manifest \"\${_manifest}\")
	string(REPLACE \"@PML_LAYER_API_VERSION@\" \"\${PML_LAYER_API_VERSION}\" _manifest \"\${_manifest}\")
	set(_dir \"\$ENV{DESTDIR}\${_prefix}/${CMAKE_INSTALL_DATAROOTDIR}/vulkan/implicit_layer.d\")
	file(MAKE_DIRECTORY \"\${_dir}\")
	file(WRITE \"\${_dir}/VkLayer_presentmon.json\" \"\${_manifest}\")
	message(STATUS \"Installing: \${_dir}/VkLayer_presentmon.json\")
")

if(PML_BUILD_TESTS)
	add_executable(pml_ring_dump tests/ring_dump.cpp)
	target_link_libraries(pml_ring_dump PRIVATE pml_common)
	target_compile_options(pml_ring_dump PRIVATE ${PML_WARNINGS})
	set_target_properties(pml_ring_dump PROPERTIES OUTPUT_NAME ring_dump)

	# Probe app: prints raw VK_EXT_present_timing behaviour (needs SDL3 and the Vulkan loader).
	find_package(SDL3 CONFIG QUIET)
	find_library(PML_VULKAN_LOADER vulkan)
	if(SDL3_FOUND AND PML_VULKAN_LOADER)
		add_executable(pml_timing_probe tests/timing_probe.cpp)
		target_include_directories(pml_timing_probe SYSTEM PRIVATE ${PML_VULKAN_HEADERS_DIR})
		target_link_libraries(pml_timing_probe PRIVATE SDL3::SDL3 ${PML_VULKAN_LOADER})
		set_target_properties(pml_timing_probe PROPERTIES OUTPUT_NAME timing_probe)
	endif()

	add_executable(pml_layer_tests tests/broker_tests.cpp tests/chain_tests.cpp tests/gpu_tests.cpp tests/process_tests.cpp)
	target_link_libraries(pml_layer_tests PRIVATE pml_layer_core GTest::gtest_main)
	target_compile_options(pml_layer_tests PRIVATE ${PML_WARNINGS})
	add_test(NAME pml_layer_tests COMMAND pml_layer_tests)

	# chain_sizes.inc (the structure sizes the pNext chain copies rely on) must match vk.xml.
	find_package(Python3 QUIET COMPONENTS Interpreter)
	if(Python3_Interpreter_FOUND)
		add_test(NAME pml_chain_table_check
			COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_chain_sizes.py
			        ${PML_VULKAN_HEADERS_DIR}/.. --check ${CMAKE_CURRENT_SOURCE_DIR}/chain_sizes.inc)
	endif()

	# Real-GPU smoke test with vkcube (skipped when vkcube is missing).
	find_program(PML_VKCUBE vkcube)
	find_program(PML_PYTHON3 python3)
	if(PML_VKCUBE AND PML_PYTHON3)
		add_test(NAME pml_layer_smoke
			COMMAND ${PML_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_vkcube.py
			        --build-dir ${CMAKE_BINARY_DIR})
		# Windows on the real desktop: timing checks fail if another window test runs at the same time.
		set_tests_properties(pml_layer_smoke PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 RESOURCE_LOCK desktop LABELS live)
	endif()
endif()
