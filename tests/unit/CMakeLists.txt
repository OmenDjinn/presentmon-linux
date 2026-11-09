# Upstream PresentMon unit tests (MetricsCore.cpp, SwapChainTests.cpp) compiled UNMODIFIED
# against pml_metrics. They are written for Microsoft's CppUnitTest framework; shim/CppUnitTest.h
# maps it onto googletest. See README.md.
set(PML_UT_DIR ${PML_PRESENTMON_DIR}/IntelPresentMon/UnitTests)

add_executable(pml_upstream_tests
	${PML_UT_DIR}/MetricsCore.cpp
	${PML_UT_DIR}/SwapChainTests.cpp
	shim/CppUnitTest.h)
# shim/ must win over anything else named CppUnitTest.h; the upstream sources are not ours,
# so no warning flags for them.
target_include_directories(pml_upstream_tests BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shim)
target_link_libraries(pml_upstream_tests PRIVATE pml_metrics GTest::gtest_main)
target_compile_options(pml_upstream_tests PRIVATE -w)

include(GoogleTest)
# One ctest entry per upstream test method, named <Suite>.<Test>.
gtest_discover_tests(pml_upstream_tests TEST_PREFIX upstream. DISCOVERY_TIMEOUT 60)

# The shim must itself fail when it should (the upstream suites only prove it can pass).
add_executable(pml_shim_selftest shim_selftest.cpp shim/CppUnitTest.h)
target_include_directories(pml_shim_selftest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shim)
target_link_libraries(pml_shim_selftest PRIVATE pml_metrics GTest::gtest_main)
target_compile_options(pml_shim_selftest PRIVATE ${PML_WARNINGS})
add_test(NAME shim.selftest COMMAND pml_shim_selftest)

# End to end: a deliberately failing TEST_METHOD must make the executable fail (WILL_FAIL), and
# an exception thrown by a test must be reported as a failure rather than crash.
add_executable(pml_shim_fail_test shim_fail_test.cpp shim/CppUnitTest.h)
target_include_directories(pml_shim_fail_test BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shim)
target_link_libraries(pml_shim_fail_test PRIVATE GTest::gtest_main)
add_test(NAME shim.failing_assert_is_detected COMMAND pml_shim_fail_test --gtest_filter=ShimFail.WrongValue)
add_test(NAME shim.exception_is_detected COMMAND pml_shim_fail_test --gtest_filter=ShimFail.Throws)
set_tests_properties(shim.failing_assert_is_detected shim.exception_is_detected PROPERTIES WILL_FAIL TRUE)
