// SPDX-License-Identifier: MIT
// Tests of the CppUnitTest shim itself: the upstream suites pass, so they cannot show that
// the shim would actually catch a wrong value.
#include <CppUnitTest.h>
#include <CommonUtilities/test/FloatAssert.h>

#include <atomic>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using Microsoft::VisualStudio::CppUnitTestFramework::detail::AssertFailure;

namespace
{
	std::atomic<int> g_methodRuns{ 0 };

	enum class Colour : uint8_t { Red = 1, Blue = 2 };

	template<typename F>
	std::string failureMessage(F&& f)
	{
		try {
			f();
		} catch (const AssertFailure& e) {
			return e.message;
		}
		return "<no failure>";
	}

	bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
}

TEST_CLASS(ShimAsserts)
{
public:
	TEST_METHOD(IntegralMixedWidthAndSignedness)
	{
		Assert::AreEqual(uint64_t(5), 5ull);
		Assert::AreEqual(size_t(3), size_t(3));
		Assert::AreEqual(uint32_t(7), uint64_t(7));
		// -1 must not equal the all-ones unsigned value (plain == would say it does).
		Assert::AreNotEqual(-1, ~0u);
		Assert::IsTrue(contains(failureMessage([] { Assert::AreEqual(uint64_t(3), uint64_t(4)); }), "Expected: <3>"));
	}

	TEST_METHOD(FailureMessageHasExpectedActualAndUserText)
	{
		const std::string m = failureMessage([] { Assert::AreEqual(1, 2, L"my note"); });
		Assert::IsTrue(contains(m, "Expected: <1>"));
		Assert::IsTrue(contains(m, "Actual:   <2>"));
		Assert::IsTrue(contains(m, "my note"));
	}

	TEST_METHOD(DoubleTolerance)
	{
		Assert::AreEqual(1.0, 1.00005, 0.0001);
		Assert::IsTrue(contains(failureMessage([] { Assert::AreEqual(1.0, 1.1, 0.0001, L"far"); }), "Tolerance"));
		// NaN is never equal to anything, including itself.
		Assert::IsTrue(contains(failureMessage([] { Assert::AreEqual(std::nan(""), std::nan(""), 1.0); }), "failed"));
		Assert::IsTrue(contains(failureMessage([] { Assert::AreEqual(0.1 + 0.2, 0.3); }), "failed"));  // no tolerance: exact
	}

	TEST_METHOD(EnumsPointersStrings)
	{
		Assert::AreEqual(Colour::Red, Colour::Red);
		Assert::IsTrue(contains(failureMessage([] { Assert::AreEqual(Colour::Red, Colour::Blue); }), "Expected: <1>"));
		int x = 0;
		Assert::AreEqual(&x, &x);
		Assert::AreEqual(std::string("a"), std::string("a"));
		Assert::AreEqual("abc", "abc");
		Assert::AreEqual(L"abc", L"abc");
		Assert::IsTrue(contains(failureMessage([] { Assert::AreEqual(L"abc", L"abd"); }), "failed"));
		Assert::AreEqual(true, true);
	}

	TEST_METHOD(BoolAndFail)
	{
		Assert::IsTrue(true);
		Assert::IsFalse(false);
		Assert::IsTrue(contains(failureMessage([] { Assert::IsTrue(false, L"t"); }), "t"));
		Assert::IsTrue(contains(failureMessage([] { Assert::IsFalse(true); }), "IsFalse"));
		Assert::IsTrue(contains(failureMessage([] { Assert::Fail(L"boom"); }), "boom"));
	}

	TEST_METHOD(FloatAssertHelperFailsThroughShim)
	{
		pmon::util::test::AssertAreEqualWithinTolerance(1.0, 1.0001, 0.001);
		const std::string m = failureMessage([] { pmon::util::test::AssertAreEqualWithinTolerance(1.0, 2.0, 0.001, L"helper"); });
		Assert::IsTrue(contains(m, "helper"));
		Assert::IsTrue(contains(m, "Expected: <1>"));
	}
};

// Each test method must run on its own fresh fixture object (MSVC semantics).
TEST_CLASS(ShimFixtureLifetime)
{
public:
	int counter = 0;
	TEST_METHOD(FreshInstanceA) { Assert::AreEqual(0, counter); counter++; g_methodRuns++; }
	TEST_METHOD(FreshInstanceB) { Assert::AreEqual(0, counter); counter++; g_methodRuns++; }
};
