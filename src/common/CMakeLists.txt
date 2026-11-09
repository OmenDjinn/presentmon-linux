# pml_common: the shared-memory protocol between the layer and its readers.
find_package(Threads REQUIRED)

add_library(pml_common STATIC
	shm.cpp
	process_info.cpp
	include/pml/protocol.hpp
	include/pml/shm.hpp
	include/pml/clock.hpp
	include/pml/process_info.hpp)
target_include_directories(pml_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(pml_common PRIVATE ${PML_WARNINGS})
target_link_libraries(pml_common PUBLIC Threads::Threads)

if(PML_BUILD_TESTS)
	add_executable(pml_common_tests tests/common_tests.cpp)
	target_link_libraries(pml_common_tests PRIVATE pml_common GTest::gtest_main)
	target_compile_options(pml_common_tests PRIVATE ${PML_WARNINGS})
	add_test(NAME pml_common_tests COMMAND pml_common_tests)
endif()
