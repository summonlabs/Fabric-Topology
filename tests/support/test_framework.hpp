// Fabric Topology test support - deterministic, dependency-free test harness.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_TESTS_TEST_FRAMEWORK_HPP
#define FABRIC_TOPOLOGY_TESTS_TEST_FRAMEWORK_HPP

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace ft_test {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> body);
};

/// Record a failure for the running test and abort it. Never returns.
[[noreturn]] void fail(const char* file, int line, const std::string& message);

/// Record that this test cannot run in this environment. Never returns.
[[noreturn]] void skip(const char* file, int line, const std::string& reason);

int run_all(int argc, char** argv);

}  // namespace ft_test

#define FT_TEST(suite_name, test_name)                                      \
    static void suite_name##_##test_name##_body();                          \
    static const ::ft_test::Registrar suite_name##_##test_name##_registrar( \
        #suite_name, #test_name, suite_name##_##test_name##_body);          \
    static void suite_name##_##test_name##_body()

#define FT_FAIL(message) ::ft_test::fail(__FILE__, __LINE__, (message))

#define FT_CHECK(condition)                                                          \
    do {                                                                             \
        if (!(condition)) {                                                          \
            ::ft_test::fail(__FILE__, __LINE__, std::string("check failed: ") + #condition); \
        }                                                                            \
    } while (false)

#define FT_CHECK_EQ(actual, expected)                                                 \
    do {                                                                             \
        const auto& ft_actual = (actual);                                            \
        const auto& ft_expected = (expected);                                        \
        if (!(ft_actual == ft_expected)) {                                           \
            ::ft_test::fail(__FILE__, __LINE__,                                      \
                            std::string("expected ") + #actual + " == " + #expected); \
        }                                                                            \
    } while (false)

#define FT_SKIP(reason) ::ft_test::skip(__FILE__, __LINE__, (reason))

#endif  // FABRIC_TOPOLOGY_TESTS_TEST_FRAMEWORK_HPP
