// SPDX-License-Identifier: MIT
//
// Minimal <CppUnitTest.h> replacement: maps the subset of Microsoft's native C++ unit test
// framework that PresentMon's UnitTests use onto googletest. See tests/unit/README.md.
//
//   TEST_CLASS(X) { TEST_METHOD(Y) { ... } };   -> gtest suite "X", test "Y"
//   Assert::AreEqual / AreNotEqual / IsTrue / IsFalse / Fail / IsNull ...
//   Logger::WriteMessage
//
// A failed Assert throws; the test wrapper catches it and reports one gtest failure, so the
// remaining statements of that test do not run (same as MSVC, where Assert throws too).
#pragma once

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cxxabi.h>
#include <iostream>
#include <typeinfo>
#include <cwchar>
#include <exception>
#include <memory>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Microsoft::VisualStudio::CppUnitTestFramework
{
	namespace detail
	{
		// Thrown by a failed Assert; carries the formatted message and where it was raised.
		struct AssertFailure
		{
			std::string message;
			std::source_location where;
		};

		// UTF-8 for a wide message. wchar_t is 32-bit here; every code point maps 1:1.
		inline std::string narrow(const wchar_t* s)
		{
			std::string out;
			if (!s) {
				return out;
			}
			for (; *s; ++s) {
				uint32_t c = static_cast<uint32_t>(*s);
				if (c < 0x80) {
					out += static_cast<char>(c);
				} else if (c < 0x800) {
					out += static_cast<char>(0xC0 | (c >> 6));
					out += static_cast<char>(0x80 | (c & 0x3F));
				} else if (c < 0x10000) {
					out += static_cast<char>(0xE0 | (c >> 12));
					out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
					out += static_cast<char>(0x80 | (c & 0x3F));
				} else {
					out += static_cast<char>(0xF0 | (c >> 18));
					out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
					out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
					out += static_cast<char>(0x80 | (c & 0x3F));
				}
			}
			return out;
		}

		template<typename T>
		concept Streamable = requires(std::ostream& os, const T& v) { os << v; };

		template<typename T>
		std::string toString(const T& v)
		{
			std::ostringstream ss;
			ss.precision(17);
			if constexpr (std::is_array_v<T>) {
				return toString(+v);
			} else if constexpr (std::is_same_v<T, bool>) {
				ss << (v ? "true" : "false");
			} else if constexpr (std::is_enum_v<T>) {
				ss << static_cast<long long>(static_cast<std::underlying_type_t<T>>(v));
			} else if constexpr (std::is_same_v<T, wchar_t> || std::is_same_v<T, char8_t> || std::is_same_v<T, char16_t> ||
				std::is_same_v<T, char32_t>) {
				ss << static_cast<unsigned long>(v);
			} else if constexpr (std::is_same_v<T, signed char> || std::is_same_v<T, unsigned char> || std::is_same_v<T, char>) {
				ss << static_cast<int>(v);
			} else if constexpr (std::is_null_pointer_v<T>) {
				ss << "nullptr";
			} else if constexpr (std::is_same_v<T, const wchar_t*> || std::is_same_v<T, wchar_t*>) {
				ss << (v ? narrow(v) : std::string("(null)"));
			} else if constexpr (std::is_same_v<T, std::wstring>) {
				ss << narrow(v.c_str());
			} else if constexpr (std::is_pointer_v<T> && !std::is_same_v<T, const char*> && !std::is_same_v<T, char*>) {
				ss << static_cast<const void*>(v);
			} else if constexpr (Streamable<T>) {
				ss << v;
			} else {
				ss << "<" << sizeof(T) << "-byte " << typeid(T).name() << ">";
			}
			return ss.str();
		}

		// Equality that never warns or misbehaves on mixed signedness/width (Windows' 32-bit
		// `unsigned long` vs Linux' 64-bit one makes literal types differ between platforms).
		template<typename A, typename B>
		bool equal(const A& a, const B& b)
		{
			// String literals arrive as arrays: compare them as pointers (strings below).
			if constexpr (std::is_array_v<A> || std::is_array_v<B>) {
				return equal(+a, +b);
			} else
			if constexpr (std::is_integral_v<A> && std::is_integral_v<B> && !std::is_same_v<A, bool> &&
				!std::is_same_v<B, bool> && !std::is_same_v<A, char> && !std::is_same_v<B, char> &&
				!std::is_same_v<A, wchar_t> && !std::is_same_v<B, wchar_t>) {
				return std::cmp_equal(a, b);
			} else if constexpr (std::is_same_v<A, const wchar_t*> && std::is_same_v<B, const wchar_t*>) {
				return a == b || (a && b && std::wcscmp(a, b) == 0);
			} else if constexpr (std::is_convertible_v<A, const char*> && std::is_convertible_v<B, const char*> &&
				std::is_pointer_v<A> && std::is_pointer_v<B>) {
				return std::string_view(a ? a : "") == std::string_view(b ? b : "") && (a != nullptr) == (b != nullptr);
			} else {
				return a == b;
			}
		}

		[[noreturn]] inline void fail(std::string msg, const wchar_t* userMessage, std::source_location where)
		{
			if (userMessage && *userMessage) {
				msg += "\n  Message: " + narrow(userMessage);
			}
			throw AssertFailure{ std::move(msg), where };
		}
	}

	class Assert
	{
	public:
		using Loc = std::source_location;

		template<typename A, typename B>
		static void AreEqual(const A& expected, const B& actual, const wchar_t* message = nullptr, Loc where = Loc::current())
		{
			if (!detail::equal(expected, actual)) {
				detail::fail("AreEqual failed.\n  Expected: <" + detail::toString(expected) + ">\n  Actual:   <" +
					detail::toString(actual) + ">", message, where);
			}
		}

		// Doubles/floats with an absolute tolerance. NaN never compares equal (MSVC: same).
		static void AreEqual(double expected, double actual, double tolerance, const wchar_t* message = nullptr, Loc where = Loc::current())
		{
			if (!(std::fabs(expected - actual) <= tolerance)) {
				detail::fail("AreEqual failed.\n  Expected: <" + detail::toString(expected) + ">\n  Actual:   <" +
					detail::toString(actual) + ">\n  Tolerance: <" + detail::toString(tolerance) + ">", message, where);
			}
		}
		static void AreEqual(float expected, float actual, float tolerance, const wchar_t* message = nullptr, Loc where = Loc::current())
		{
			AreEqual(static_cast<double>(expected), static_cast<double>(actual), static_cast<double>(tolerance), message, where);
		}

		template<typename A, typename B>
		static void AreNotEqual(const A& notExpected, const B& actual, const wchar_t* message = nullptr, Loc where = Loc::current())
		{
			if (detail::equal(notExpected, actual)) {
				detail::fail("AreNotEqual failed.\n  Not expected: <" + detail::toString(notExpected) + ">\n  Actual:       <" +
					detail::toString(actual) + ">", message, where);
			}
		}
		static void AreNotEqual(double notExpected, double actual, double tolerance, const wchar_t* message = nullptr, Loc where = Loc::current())
		{
			if (std::fabs(notExpected - actual) <= tolerance) {
				detail::fail("AreNotEqual failed.\n  Not expected: <" + detail::toString(notExpected) + ">\n  Actual:       <" +
					detail::toString(actual) + ">\n  Tolerance: <" + detail::toString(tolerance) + ">", message, where);
			}
		}

		template<typename A, typename B>
		static void AreSame(const A& expected, const B& actual, const wchar_t* message = nullptr, Loc where = Loc::current())
		{
			if (&expected != &actual) {
				detail::fail("AreSame failed: different objects.", message, where);
			}
		}

		static void IsTrue(bool condition, const wchar_t* message = nullptr, Loc where = Loc::current())
		{
			if (!condition) {
				detail::fail("IsTrue failed.", message, where);
			}
		}
		static void IsFalse(bool condition, const wchar_t* message = nullptr, Loc where = Loc::current())
		{
			if (condition) {
				detail::fail("IsFalse failed.", message, where);
			}
		}
		template<typename T>
		static void IsNull(const T* p, const wchar_t* message = nullptr, Loc where = Loc::current())
		{
			if (p != nullptr) {
				detail::fail("IsNull failed.", message, where);
			}
		}
		template<typename T>
		static void IsNotNull(const T* p, const wchar_t* message = nullptr, Loc where = Loc::current())
		{
			if (p == nullptr) {
				detail::fail("IsNotNull failed.", message, where);
			}
		}
		[[noreturn]] static void Fail(const wchar_t* message = nullptr, Loc where = Loc::current())
		{
			detail::fail("Fail called.", message, where);
		}
	};

	class Logger
	{
	public:
		static void WriteMessage(const wchar_t* message) { std::cout << detail::narrow(message) << std::endl; }
		static void WriteMessage(const char* message) { std::cout << message << std::endl; }
	};

	namespace detail
	{
		// Runs one test method on a fresh fixture object (MSVC constructs a new instance per
		// test) and turns AssertFailure / exceptions into gtest failures.
		template<typename Fn>
		class MethodTest : public ::testing::Test
		{
		public:
			explicit MethodTest(Fn fn) : fn_(std::move(fn)) {}
			void TestBody() override
			{
				try {
					fn_();
				} catch (const AssertFailure& f) {
					ADD_FAILURE_AT(f.where.file_name(), static_cast<int>(f.where.line())) << f.message;
				} catch (const std::exception& e) {
					ADD_FAILURE() << "Unhandled exception: " << e.what();
				} catch (...) {
					ADD_FAILURE() << "Unhandled non-standard exception";
				}
			}
		private:
			Fn fn_;
		};

		// Class name without namespaces ("MetricsCoreTests::Foo" -> "Foo"), used as gtest suite.
		inline std::string suiteName(const std::type_info& ti)
		{
			int status = 0;
			char* d = abi::__cxa_demangle(ti.name(), nullptr, nullptr, &status);
			std::string n = (status == 0 && d) ? d : ti.name();
			std::free(d);
			const auto pos = n.rfind("::");
			return pos == std::string::npos ? n : n.substr(pos + 2);
		}

		template<typename Fixture, typename Method>
		bool registerMethod(const char* name, const char* file, int line, Method m)
		{
			const std::string suite = suiteName(typeid(Fixture));
			auto run = [m] { Fixture f; (f.*m)(); };
			using Test = MethodTest<decltype(run)>;
			::testing::RegisterTest(suite.c_str(), name, nullptr, nullptr, file, line,
				[run]() -> ::testing::Test* { return new Test(run); });
			return true;
		}

		// Base class of every TEST_CLASS: gives it a name for the Self type.
		template<typename Derived>
		struct ClassBase
		{
			using pml_Self = Derived;
		};
	}
}

// The registrar is a nested class so its constructor body can name the (then complete)
// enclosing class and the method declared right after it.
#define TEST_CLASS(name) class name : public ::Microsoft::VisualStudio::CppUnitTestFramework::detail::ClassBase<name>

#define TEST_METHOD(name)                                                                              \
	struct pml_reg_##name {                                                                            \
		pml_reg_##name() {                                                                             \
			::Microsoft::VisualStudio::CppUnitTestFramework::detail::registerMethod<pml_Self>(         \
				#name, __FILE__, __LINE__, &pml_Self::name);                          \
		}                                                                                              \
	};                                                                                                 \
	inline static pml_reg_##name pml_regInstance_##name{};                                             \
	void name()
