// Fabric Topology test support - deterministic, dependency-free test harness.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "test_framework.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace ft_test {

namespace {

struct TestAbort : std::exception {};

struct TestSkip : std::exception {
    std::string reason;
    explicit TestSkip(std::string text) : reason(std::move(text)) {}
};

thread_local std::size_t g_failures = 0;
thread_local std::string g_last_failure;

}  // namespace

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

Registrar::Registrar(const char* suite, const char* name, std::function<void()> body) {
    registry().push_back(TestCase{suite, name, std::move(body)});
}

void fail(const char* file, int line, const std::string& message) {
    ++g_failures;
    g_last_failure = std::string(file) + ":" + std::to_string(line) + ": " + message;
    throw TestAbort{};
}

void skip(const char* file, int line, const std::string& reason) {
    throw TestSkip(std::string(file) + ":" + std::to_string(line) + ": " + reason);
}

int run_all(int argc, char** argv) {
    std::string filter;
    bool list_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const std::string prefix = "--filter=";
        if (argument.rfind(prefix, 0) == 0) {
            filter = argument.substr(prefix.size());
        } else if (argument == "--list") {
            list_only = true;
        }
    }

    std::vector<TestCase>& tests = registry();
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;

    for (const TestCase& test : tests) {
        const std::string full = test.suite + "." + test.name;
        if (!filter.empty() && full.find(filter) == std::string::npos) {
            continue;
        }
        if (list_only) {
            std::cout << full << "\n";
            continue;
        }
        g_failures = 0;
        g_last_failure.clear();
        std::string status = "PASS";
        std::string detail;
        try {
            test.body();
        } catch (const TestSkip& skipped_test) {
            status = "SKIP";
            detail = skipped_test.reason;
            ++skipped;
        } catch (const TestAbort&) {
            status = "FAIL";
            detail = g_last_failure;
            ++failed;
        } catch (const std::exception& error) {
            status = "FAIL";
            detail = std::string("unhandled exception: ") + error.what();
            ++failed;
        } catch (...) {
            status = "FAIL";
            detail = "unhandled non-standard exception";
            ++failed;
        }
        if (status == "PASS") {
            ++passed;
        }
        std::cout << status << " " << full;
        if (!detail.empty()) {
            std::cout << " - " << detail;
        }
        std::cout << "\n";
        std::cout.flush();
    }

    if (list_only) {
        return 0;
    }
    std::cout << "summary passed=" << passed << " failed=" << failed << " skipped=" << skipped << "\n";
    if (failed != 0) {
        return 1;
    }
    if (passed == 0 && skipped != 0) {
        return 77;
    }
    return 0;
}

}  // namespace ft_test

int main(int argc, char** argv) { return ::ft_test::run_all(argc, argv); }
