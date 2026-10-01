// TestFramework.cpp
// Runs every registered test and prints a pass/fail summary.
// Exit code is 0 only if all tests pass (so `make test` fails loudly otherwise).
#include "TestFramework.hpp"

#include <exception>
#include <iostream>

namespace testing {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

}  // namespace testing

int main() {
    int passed = 0;
    int failed = 0;

    for (const testing::TestCase& test : testing::registry()) {
        try {
            test.function();
            std::cout << "[PASS] " << test.name << "\n";
            passed++;
        } catch (const std::exception& error) {
            std::cout << "[FAIL] " << test.name << "\n       " << error.what() << "\n";
            failed++;
        }
    }

    std::cout << "\n" << passed << " passed, " << failed << " failed, "
              << (passed + failed) << " total\n";

    if (failed > 0) {
        return 1;
    }
    return 0;
}
