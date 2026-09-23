// Minimal dependency-free test harness shared by the test binaries.
#pragma once

#include <cstddef>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace lru_test {

inline int& check_count() {
    static int count = 0;
    return count;
}

inline int& failure_count() {
    static int count = 0;
    return count;
}

inline std::string& current_test() {
    static std::string name;
    return name;
}

inline void report_failure(const char* file, int line, const std::string& what) {
    ++failure_count();
    std::cerr << "    FAIL [" << current_test() << "] " << file << ":" << line << ": " << what
              << "\n";
}

template <typename T>
std::string describe(const T& value) {
    std::ostringstream os;
    os << value;
    return os.str();
}

template <typename T>
std::string describe(const std::optional<T>& value) {
    return value ? ("optional(" + describe(*value) + ")") : std::string("nullopt");
}

inline std::string describe(std::nullopt_t) { return "nullopt"; }
inline std::string describe(bool value) { return value ? "true" : "false"; }

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline void add_test(std::string name, std::function<void()> fn) {
    registry().push_back({std::move(name), std::move(fn)});
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) {
        add_test(std::move(name), std::move(fn));
    }
};

inline int run_all(const char* suite_name) {
    std::cout << suite_name << ": running " << registry().size() << " tests\n";
    int failed_tests = 0;

    for (const TestCase& test : registry()) {
        current_test() = test.name;
        const int failures_before = failure_count();
        std::cout << "  " << test.name << "\n";
        test.fn();
        if (failure_count() != failures_before) {
            ++failed_tests;
        }
    }

    std::cout << "\n"
              << check_count() << " checks, " << failure_count() << " failures in "
              << registry().size() << " tests\n";
    if (failed_tests == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << failed_tests << " TEST(S) FAILED\n";
    return 1;
}

}  // namespace lru_test

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++lru_test::check_count();                                                    \
        if (!(cond)) {                                                                \
            lru_test::report_failure(__FILE__, __LINE__, "CHECK(" #cond ") failed");  \
        }                                                                             \
    } while (0)

// Values are copied, not bound by reference: binding a reference to a member of
// a temporary (for example cache.get(k)->field) would dangle immediately.
#define CHECK_EQ(actual, expected)                                                         \
    do {                                                                                   \
        ++lru_test::check_count();                                                         \
        const auto actual_value = (actual);                                                \
        const auto expected_value = (expected);                                            \
        if (!(actual_value == expected_value)) {                                           \
            lru_test::report_failure(__FILE__, __LINE__,                                   \
                                     "CHECK_EQ(" #actual ", " #expected ") failed: got " + \
                                         lru_test::describe(actual_value) + ", want " +    \
                                         lru_test::describe(expected_value));              \
        }                                                                                  \
    } while (0)

#define TEST(name)                                                   \
    static void name();                                              \
    static const lru_test::Registrar registrar_##name(#name, name);  \
    static void name()
