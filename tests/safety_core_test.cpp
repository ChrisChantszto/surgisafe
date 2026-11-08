// Host-side unit tests for the portable safety core. No QNX required:
//   make test

#include "safety_core.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <limits>
#include <string_view>
#include <vector>

using namespace surgisafe;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, \
                         __LINE__, #cond);                                \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Register {
    Register(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

#define TEST(name)                                     \
    void name();                                       \
    const Register register_##name{#name, name};       \
    void name()

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// Expects a request to be rejected with `reason` and leave state untouched.
void expect_rejected(SafetyCore& core, std::uint32_t cycle, double proposed,
                     StopReason reason) {
    const double before = core.position();
    const Decision d = core.on_request(cycle, proposed);
    CHECK(!d.permitted);
    CHECK(d.reason == reason);
    CHECK(d.actual_position == before);
    CHECK(core.position() == before);
    CHECK(core.stopped());
    CHECK(core.stop_reason() == reason);
}

// --- Initial state ---------------------------------------------------------

TEST(starts_unarmed_at_origin) {
    SafetyCore core;
    CHECK(!core.armed());
    CHECK(!core.stopped());
    CHECK(!core.watchdog_should_run());
    CHECK(core.position() == 0.0);
    CHECK(core.expected_cycle() == 0);
    CHECK(core.stop_reason() == StopReason::None);
}

// --- Accepted motion -------------------------------------------------------

TEST(valid_step_is_committed) {
    SafetyCore core;
    const Decision d = core.on_request(0, 0.05);
    CHECK(d.permitted);
    CHECK(d.reason == StopReason::None);
    CHECK(d.actual_position == 0.05);
    CHECK(core.position() == 0.05);
    CHECK(core.expected_cycle() == 1);
    CHECK(core.armed());
    CHECK(core.watchdog_should_run());
}

TEST(zero_step_is_allowed) {
    SafetyCore core;
    CHECK(core.on_request(0, 0.0).permitted);
    CHECK(core.on_request(1, 0.0).permitted);
}

TEST(exact_max_step_is_allowed_both_directions) {
    SafetyCore core;
    CHECK(core.on_request(0, kMaxStep).permitted);
    CHECK(core.on_request(1, 2 * kMaxStep).permitted);
    CHECK(core.on_request(2, kMaxStep).permitted);  // Step back down.
}

TEST(controller_ramp_reaches_upper_limit) {
    // Mirrors controller.cpp: clamp toward the target each cycle.
    SafetyCore core;
    double position = 0.0;
    std::uint32_t cycle = 0;
    for (; cycle < 100; ++cycle) {
        const double command =
            std::clamp(kMaxPosition - position, -kMaxStep, kMaxStep);
        const Decision d = core.on_request(cycle, position + command);
        CHECK(d.permitted);
        if (!d.permitted) return;
        position = d.actual_position;
    }
    CHECK(position == kMaxPosition);
    CHECK(!core.stopped());
}

// --- Invalid motion --------------------------------------------------------

TEST(step_too_large_is_rejected) {
    SafetyCore core;
    expect_rejected(core, 0, kMaxStep + 1e-6, StopReason::InvalidMotion);
}

TEST(negative_position_is_rejected) {
    SafetyCore core;
    expect_rejected(core, 0, -1e-6, StopReason::InvalidMotion);
}

TEST(above_upper_limit_is_rejected) {
    SafetyCore core;
    double position = 0.0;
    std::uint32_t cycle = 0;
    while (position < kMaxPosition) {
        position = std::min(position + kMaxStep, kMaxPosition);
        CHECK(core.on_request(cycle++, position).permitted);
    }
    expect_rejected(core, cycle, kMaxPosition + 1e-6,
                    StopReason::InvalidMotion);
}

TEST(nan_is_rejected) {
    SafetyCore core;
    expect_rejected(core, 0, kNaN, StopReason::InvalidMotion);
}

TEST(positive_infinity_is_rejected) {
    SafetyCore core;
    expect_rejected(core, 0, kInf, StopReason::InvalidMotion);
}

TEST(negative_infinity_is_rejected) {
    SafetyCore core;
    expect_rejected(core, 0, -kInf, StopReason::InvalidMotion);
}

TEST(motion_error_wins_over_sequence_error) {
    SafetyCore core;
    expect_rejected(core, 7, kNaN, StopReason::InvalidMotion);
}

// --- Sequence --------------------------------------------------------------

TEST(first_cycle_must_be_zero) {
    SafetyCore core;
    expect_rejected(core, 1, 0.0, StopReason::InvalidSequence);
}

TEST(skipped_cycle_is_rejected) {
    SafetyCore core;
    CHECK(core.on_request(0, 0.1).permitted);
    expect_rejected(core, 2, 0.2, StopReason::InvalidSequence);
}

TEST(replayed_cycle_is_rejected) {
    SafetyCore core;
    CHECK(core.on_request(0, 0.1).permitted);
    expect_rejected(core, 0, 0.1, StopReason::InvalidSequence);
}

// --- Latching --------------------------------------------------------------

TEST(stop_is_latched_and_keeps_first_reason) {
    SafetyCore core;
    CHECK(core.on_request(0, 0.1).permitted);
    expect_rejected(core, 1, 5.0, StopReason::InvalidMotion);
    // A perfectly valid follow-up is still refused with the original reason.
    expect_rejected(core, 1, 0.2, StopReason::InvalidMotion);
    // A later, different kind of error does not overwrite the reason.
    expect_rejected(core, 99, 0.2, StopReason::InvalidMotion);
    CHECK(!core.watchdog_should_run());
}

TEST(rejected_request_does_not_advance_cycle) {
    SafetyCore core;
    CHECK(core.on_request(0, 0.1).permitted);
    core.on_request(1, 5.0);
    CHECK(core.expected_cycle() == 1);
}

// --- Watchdog --------------------------------------------------------------

TEST(watchdog_ignored_before_first_request) {
    SafetyCore core;
    core.on_watchdog_timeout();
    CHECK(!core.stopped());
    CHECK(core.on_request(0, 0.1).permitted);
}

TEST(watchdog_latches_after_arming) {
    SafetyCore core;
    CHECK(core.on_request(0, 0.1).permitted);
    core.on_watchdog_timeout();
    CHECK(core.stopped());
    CHECK(!core.watchdog_should_run());
    // The next (otherwise valid) request reports the watchdog as the cause.
    expect_rejected(core, 1, 0.2, StopReason::WatchdogTimeout);
}

TEST(watchdog_reason_survives_later_invalid_request) {
    // Regression: previously the reported reason depended on the validity
    // of the current request rather than on what caused the stop.
    SafetyCore core;
    CHECK(core.on_request(0, 0.1).permitted);
    core.on_watchdog_timeout();
    expect_rejected(core, 1, kNaN, StopReason::WatchdogTimeout);
}

TEST(watchdog_does_not_overwrite_earlier_stop) {
    SafetyCore core;
    core.on_request(0, kNaN);
    core.on_watchdog_timeout();
    CHECK(core.stop_reason() == StopReason::InvalidMotion);
}

TEST(invalid_first_request_still_arms) {
    SafetyCore core;
    core.on_request(0, kNaN);
    CHECK(core.armed());
}

// --- Monitor fault ---------------------------------------------------------

TEST(fault_latches_even_before_arming) {
    SafetyCore core;
    core.on_fault();
    expect_rejected(core, 0, 0.1, StopReason::MonitorFault);
}

// --- Misc ------------------------------------------------------------------

TEST(reason_names_and_wire_values) {
    CHECK(static_cast<int>(StopReason::None) == 0);
    CHECK(static_cast<int>(StopReason::WatchdogTimeout) == 1);
    CHECK(static_cast<int>(StopReason::InvalidMotion) == 2);
    CHECK(static_cast<int>(StopReason::InvalidSequence) == 3);
    CHECK(static_cast<int>(StopReason::MonitorFault) == 4);
    CHECK(std::string_view(to_string(StopReason::WatchdogTimeout)) ==
          "watchdog timeout");
    CHECK(std::string_view(to_string(static_cast<StopReason>(42))) ==
          "unknown");
}

} // namespace

int main() {
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
