// SPDX-License-Identifier: MIT
// Intentionally failing tests; ctest runs them with WILL_FAIL (see CMakeLists.txt).
#include <CppUnitTest.h>
#include <stdexcept>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

TEST_CLASS(ShimFail)
{
public:
	TEST_METHOD(WrongValue) { Assert::AreEqual(3, 4, L"deliberate"); }
	TEST_METHOD(Throws) { throw std::runtime_error("deliberate"); }
};
