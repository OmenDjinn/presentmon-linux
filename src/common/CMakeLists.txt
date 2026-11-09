# pml_common: the shared-memory protocol between the layer and its readers.
find_package(Threads REQUIRED)

add_library(pml_common STATIC
	shm.cpp
	process_info.cpp
	drm_outputs.cpp
	include/pml/protocol.hpp
	include/pml/shm.hpp
	include/pml/clock.hpp
	include/pml/process_info.hpp
	include/pml/drm_outputs.hpp)
target_include_directories(pml_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(pml_common PRIVATE ${PML_WARNINGS})
target_link_libraries(pml_common PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
# libdrm's headers only: drm_outputs.cpp loads libdrm.so.2 with dlopen (the layer runs in games' containers).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
	pkg_check_modules(PML_LIBDRM_HEADERS QUIET libdrm)
endif()
if(PML_LIBDRM_HEADERS_FOUND)
	target_include_directories(pml_common PRIVATE ${PML_LIBDRM_HEADERS_INCLUDE_DIRS})
	target_compile_definitions(pml_common PRIVATE PML_HAVE_LIBDRM_HEADERS)
else()
	message(STATUS "pml_common: libdrm headers not found, VRR state from DRM unavailable")
endif()

if(PML_BUILD_TESTS)
	add_executable(pml_common_tests tests/common_tests.cpp)
	target_link_libraries(pml_common_tests PRIVATE pml_common GTest::gtest_main)
	target_compile_options(pml_common_tests PRIVATE ${PML_WARNINGS})
	add_test(NAME pml_common_tests COMMAND pml_common_tests)
endif()
