// TestFramework.hpp
// A tiny self-contained unit test framework, so the project has no external
// dependencies. Usage:
//
//   TEST(my_test_name) {
//       CHECK(1 + 1 == 2);
//       CHECK_EQ(someValue, 42);
//   }
//
// Every TEST registers itself in a global list before main() runs.
// main() (see TestFramework.cpp) runs them all and prints a summary.
#pragma once

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace testing {

using TestFunction = void (*)();

struct TestCase {
    std::string name;
    TestFunction function;
};

// Returns the single global list of tests.
std::vector<TestCase>& registry();

// Constructing one of these adds a test to the registry.
struct TestRegistrar {
    TestRegistrar(const char* name, TestFunction function) {
        registry().push_back(TestCase{name, function});
    }
};

// Thrown by CHECK / CHECK_EQ when an assertion fails.
struct TestFailure : public std::runtime_error {
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

}  // namespace testing

#define TEST(name)                                                    \
    static void name();                                               \
    static testing::TestRegistrar registrar_##name(#name, name);      \
    static void name()

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::ostringstream message_;                                          \
            message_ << __FILE__ << ":" << __LINE__ << ": CHECK(" #condition ") failed"; \
            throw testing::TestFailure(message_.str());                           \
        }                                                                         \
    } while (false)

#define CHECK_EQ(actual, expected)                                                \
    do {                                                                          \
        const auto actualValue_ = (actual);                                       \
        const auto expectedValue_ = (expected);                                   \
        if (!(actualValue_ == expectedValue_)) {                                  \
            std::ostringstream message_;                                          \
            message_ << __FILE__ << ":" << __LINE__ << ": CHECK_EQ(" #actual ", " #expected \
                     << ") failed: got " << actualValue_ << ", expected " << expectedValue_; \
            throw testing::TestFailure(message_.str());                           \
        }                                                                         \
    } while (false)
