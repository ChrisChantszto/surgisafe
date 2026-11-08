#pragma once

// Minimal dependency-free test framework shared by the host test files.

#include <cstdint>
#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

namespace testing {

inline int g_failures = 0;

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Register {
    Register(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline int run_all() {
    int failed_tests = 0;
    for (const TestCase& test : registry()) {
        const int before = g_failures;
        test.fn();
        const bool ok = g_failures == before;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", test.name);
        if (!ok) ++failed_tests;
    }
    std::printf("\n%zu tests, %d failed\n", registry().size(), failed_tests);
    return failed_tests == 0 ? 0 : 1;
}

// Deterministic fake monotonic clock, in nanoseconds.
struct FakeClock {
    std::uint64_t now = 1'000'000'000ULL;  // Arbitrary non-zero start.
    std::uint64_t advance_ms(std::uint64_t ms) {
        now += ms * 1'000'000ULL;
        return now;
    }
    std::uint64_t advance_ns(std::uint64_t ns) {
        now += ns;
        return now;
    }
};

} // namespace testing

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, \
                         __LINE__, #cond);                                \
            ++::testing::g_failures;                                      \
        }                                                                 \
    } while (0)

#define TEST(name)                                           \
    void name();                                             \
    const ::testing::Register register_##name{#name, name};  \
    void name()
