# tests/unit: PresentMon's own metric tests on Linux

`pml_upstream_tests` compiles `third_party/PresentMon/IntelPresentMon/UnitTests/MetricsCore.cpp`
and `SwapChainTests.cpp` **unmodified** against `pml_metrics`. Those files are written for
Microsoft's CppUnitTest framework, which does not exist on Linux, so `shim/CppUnitTest.h`
provides the subset they use on top of googletest.

## The shim

- `TEST_CLASS(X) { ... }` defines a class; each `TEST_METHOD(Y)` self-registers through
  `testing::RegisterTest` as gtest `X.Y`. Every test runs on a fresh instance of the class,
  like MSVC.
- `Assert::AreEqual / AreNotEqual / IsTrue / IsFalse / Fail / IsNull / IsNotNull / AreSame`
  and `Logger::WriteMessage`. A failed assert throws, the wrapper turns it into one gtest
  failure at the assert's source line (expected/actual, tolerance and the test's own wide
  message), and the rest of that test is skipped, as on Windows.
- `AreEqual(double, double, tolerance)` is an absolute tolerance, NaN never equal. Without a
  tolerance, floating point is compared exactly.
- Integers compare with `std::cmp_equal`, so `uint64_t` (`unsigned long` here, `unsigned long
  long` on Windows) against a `1ull` literal or a signed value never fails to compile and never
  hits sign-conversion surprises.
- `shim/CommonUtilities/qpc.h` forwards to `Qpc.h` (upstream includes the lower-case name).
- `CommonUtilities/test/FloatAssert.h` is the repo's own file; it compiles through the shim
  and fails through `Assert::Fail`.

## Running

```
cmake -S . -B build/b1-unit -G Ninja -DPML_BUILD_METRICS=ON -DPML_BUILD_TESTS=ON \
  -DPML_BUILD_LAYER=OFF -DPML_BUILD_COLLECTOR=OFF -DPML_BUILD_TEST_APPS=OFF -DPML_BUILD_E2E=OFF
cmake --build build/b1-unit && ctest --test-dir build/b1-unit -R upstream
```

`gtest_discover_tests` makes one ctest entry per test, named `upstream.<Suite>.<Test>`.

## Test count

Upstream has 207 + 11 `TEST_METHOD` occurrences in text, but the 11 in
`ConsoleAdapterTests` sit inside a `/* ... */` block (upstream skips them: they need the ETW
`PresentEvent`). So 196 + 11 = **207 tests in 34 suites** run, not 218. Nothing is excluded
by us.

## Shim self tests

`shim_selftest.cpp` checks that the asserts do fail with good messages (the upstream suites
only prove the shim can pass). `shim_fail_test.cpp` has a wrong-value test and a throwing test
that ctest runs with `WILL_FAIL`, proving a failure reaches the exit code.
