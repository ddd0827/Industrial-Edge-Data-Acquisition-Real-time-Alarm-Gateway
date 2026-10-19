#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace egtest {

class Failure : public std::runtime_error {
public:
    Failure(const char* file, int line, const std::string& message)
        : std::runtime_error(std::string(file) + ":" + std::to_string(line) + ": " +
                             message) {}
};

using TestFunction = std::function<void()>;

struct TestCaseInfo {
    std::string name;
    TestFunction function;
};

inline std::vector<TestCaseInfo>& registry() {
    static std::vector<TestCaseInfo> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, TestFunction function) {
        registry().push_back(TestCaseInfo{name, std::move(function)});
    }
};

inline int run_all() {
    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const TestCaseInfo& test : registry()) {
        try {
            test.function();
            ++passed;
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& ex) {
            ++failed;
            std::cout << "[FAIL] " << test.name << "\n       " << ex.what() << "\n";
        } catch (...) {
            ++failed;
            std::cout << "[FAIL] " << test.name << "\n       unknown exception\n";
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed, "
              << registry().size() << " total\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace egtest

#define TEST_CASE_NAME_INNER(line) test_case_##line
#define TEST_CASE_NAME(line) TEST_CASE_NAME_INNER(line)

#define TEST_CASE(name)                                                          \
    static void name();                                                          \
    static const ::egtest::Registrar test_registrar_##name(#name, name);         \
    static void name()

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        if (!(condition)) {                                                      \
            throw ::egtest::Failure(__FILE__, __LINE__,                          \
                                    "requirement failed: " #condition);          \
        }                                                                        \
    } while (false)

#define REQUIRE_EQ(actual, expected)                                             \
    do {                                                                         \
        const auto lhs_value = (actual);                                         \
        const auto rhs_value = (expected);                                       \
        if (!(lhs_value == rhs_value)) {                                         \
            throw ::egtest::Failure(__FILE__, __LINE__,                          \
                                    "expected " #actual " == " #expected);       \
        }                                                                        \
    } while (false)

#define REQUIRE_NEAR(actual, expected, tolerance)                                \
    do {                                                                         \
        const double lhs_value = static_cast<double>(actual);                    \
        const double rhs_value = static_cast<double>(expected);                  \
        if (std::fabs(lhs_value - rhs_value) > (tolerance)) {                    \
            std::ostringstream message;                                          \
            message << "expected " #actual " near " #expected " (got "           \
                    << lhs_value << " vs " << rhs_value << ")";                  \
            throw ::egtest::Failure(__FILE__, __LINE__, message.str());          \
        }                                                                        \
    } while (false)

#define REQUIRE_THROW(expression, exception_type)                                \
    do {                                                                         \
        bool thrown = false;                                                     \
        try {                                                                    \
            (void)(expression);                                                  \
        } catch (const exception_type&) {                                        \
            thrown = true;                                                       \
        } catch (...) {                                                          \
            throw ::egtest::Failure(__FILE__, __LINE__,                          \
                                    "wrong exception type from " #expression);   \
        }                                                                        \
        if (!thrown) {                                                           \
            throw ::egtest::Failure(__FILE__, __LINE__,                          \
                                    "expected exception from " #expression);     \
        }                                                                        \
    } while (false)
