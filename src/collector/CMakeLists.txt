# presentmon: the collector CLI (capture -> PresentMon-format CSV / console stats, list, overlay control).
#
# pml_collector_core holds everything except main() so the tests can call the CSV writer and the
# option parser directly. pml_metrics makes every consumer build as C++23 (see src/metrics).
add_library(pml_collector_core STATIC
	src/options.cpp
	src/csv.cpp
	src/log.cpp
	src/capture.cpp
	src/list_cmd.cpp
	src/overlay_cmd.cpp
	src/options.hpp
	src/csv.hpp
	src/log.hpp
	src/capture.hpp
	src/commands.hpp)
target_include_directories(pml_collector_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pml_collector_core PUBLIC pml_metrics pml_common)
target_compile_options(pml_collector_core PRIVATE ${PML_WARNINGS})

add_executable(presentmon src/main.cpp)
target_link_libraries(presentmon PRIVATE pml_collector_core)
target_compile_options(presentmon PRIVATE ${PML_WARNINGS})
install(TARGETS presentmon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(PML_BUILD_TESTS)
	# Stand-in for VkLayer_presentmon: creates a real segment and publishes a deterministic timeline.
	add_executable(pml_fake_producer tests/fake_producer.cpp tests/fake_timeline.hpp)
	target_link_libraries(pml_fake_producer PRIVATE pml_common)
	target_compile_options(pml_fake_producer PRIVATE ${PML_WARNINGS})

	add_executable(pml_collector_tests
		tests/csv_tests.cpp
		tests/cli_tests.cpp
		tests/test_support.hpp
		tests/fake_timeline.hpp)
	target_link_libraries(pml_collector_tests PRIVATE pml_collector_core GTest::gtest_main)
	target_compile_definitions(pml_collector_tests PRIVATE
		PML_PRESENTMON_EXE="$<TARGET_FILE:presentmon>"
		PML_FAKE_PRODUCER_EXE="$<TARGET_FILE:pml_fake_producer>"
		PML_GOLD_DIR="${PML_PRESENTMON_DIR}/Tests/Gold"
		PML_TEST_TMP="${CMAKE_CURRENT_BINARY_DIR}/test-tmp")
	target_compile_options(pml_collector_tests PRIVATE ${PML_WARNINGS})
	add_dependencies(pml_collector_tests presentmon pml_fake_producer)
	add_test(NAME pml_collector_tests COMMAND pml_collector_tests)
	set_tests_properties(pml_collector_tests PROPERTIES TIMEOUT 180)
endif()
