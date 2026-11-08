// Host-side unit tests for the portable safety core. No QNX required:
//   make test
//
// Test names are tagged with the safety requirement they verify; see
// docs/HAZARDS.md for the requirement list.

#include "safety_core.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <limits>
#include <string_view>

using namespace surgisafe;
using testing::FakeClock;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr std::uint64_t kCycleMs = 10;  // Controller period.

// Sends a request 10 ms after the previous one (a healthy controller).
Decision send(SafetyCore& core, FakeClock& clock, std::uint32_t cycle,
              double proposed) {
    return core.on_request(cycle, proposed, clock.advance_ms(kCycleMs));
}

// Expects a request to be rejected with `reason` and leave state untouched.
void expect_rejected(SafetyCore& core, FakeClock& clock, std::uint32_t cycle,
                     double proposed, StopReason reason) {
    const double before = core.position();
    const Decision d = send(core, clock, cycle, proposed);
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

// --- SR-1: only bounded steps are committed --------------------------------

TEST(sr1_valid_step_is_committed) {
    SafetyCore core;
    FakeClock clock;
    const Decision d = send(core, clock, 0, 0.05);
    CHECK(d.permitted);
    CHECK(d.reason == StopReason::None);
    CHECK(d.actual_position == 0.05);
    CHECK(core.position() == 0.05);
    CHECK(core.expected_cycle() == 1);
    CHECK(core.armed());
    CHECK(core.watchdog_should_run());
}

TEST(sr1_zero_step_is_allowed) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, 0.0).permitted);
    CHECK(send(core, clock, 1, 0.0).permitted);
}

TEST(sr1_exact_max_step_is_allowed_both_directions) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, kMaxStep).permitted);
    CHECK(send(core, clock, 1, 2 * kMaxStep).permitted);
    CHECK(send(core, clock, 2, kMaxStep).permitted);  // Step back down.
}

TEST(sr1_step_too_large_is_rejected) {
    SafetyCore core;
    FakeClock clock;
    expect_rejected(core, clock, 0, kMaxStep + 1e-6,
                    StopReason::InvalidMotion);
}

TEST(sr1_controller_ramp_reaches_upper_limit) {
    // Mirrors controller.cpp: clamp toward the target each cycle.
    SafetyCore core;
    FakeClock clock;
    double position = 0.0;
    for (std::uint32_t cycle = 0; cycle < 100; ++cycle) {
        const double command =
            std::clamp(kMaxPosition - position, -kMaxStep, kMaxStep);
        const Decision d = send(core, clock, cycle, position + command);
        CHECK(d.permitted);
        if (!d.permitted) return;
        position = d.actual_position;
    }
    CHECK(position == kMaxPosition);
    CHECK(!core.stopped());
}

// --- SR-2: position stays inside the workspace -----------------------------

TEST(sr2_negative_position_is_rejected) {
    SafetyCore core;
    FakeClock clock;
    expect_rejected(core, clock, 0, -1e-6, StopReason::InvalidMotion);
}

TEST(sr2_above_upper_limit_is_rejected) {
    SafetyCore core;
    FakeClock clock;
    double position = 0.0;
    std::uint32_t cycle = 0;
    while (position < kMaxPosition) {
        position = std::min(position + kMaxStep, kMaxPosition);
        CHECK(send(core, clock, cycle++, position).permitted);
    }
    expect_rejected(core, clock, cycle, kMaxPosition + 1e-6,
                    StopReason::InvalidMotion);
}

TEST(sr2_nan_is_rejected) {
    SafetyCore core;
    FakeClock clock;
    expect_rejected(core, clock, 0, kNaN, StopReason::InvalidMotion);
}

TEST(sr2_positive_infinity_is_rejected) {
    SafetyCore core;
    FakeClock clock;
    expect_rejected(core, clock, 0, kInf, StopReason::InvalidMotion);
}

TEST(sr2_negative_infinity_is_rejected) {
    SafetyCore core;
    FakeClock clock;
    expect_rejected(core, clock, 0, -kInf, StopReason::InvalidMotion);
}

TEST(sr2_motion_error_wins_over_sequence_error) {
    SafetyCore core;
    FakeClock clock;
    expect_rejected(core, clock, 7, kNaN, StopReason::InvalidMotion);
}

// --- SR-3: requests must arrive in order -----------------------------------

TEST(sr3_first_cycle_must_be_zero) {
    SafetyCore core;
    FakeClock clock;
    expect_rejected(core, clock, 1, 0.0, StopReason::InvalidSequence);
}

TEST(sr3_skipped_cycle_is_rejected) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, 0.1).permitted);
    expect_rejected(core, clock, 2, 0.2, StopReason::InvalidSequence);
}

TEST(sr3_replayed_cycle_is_rejected) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, 0.1).permitted);
    expect_rejected(core, clock, 0, 0.1, StopReason::InvalidSequence);
}

// --- SR-4: a stop is latched with its first cause --------------------------

TEST(sr4_stop_is_latched_and_keeps_first_reason) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, 0.1).permitted);
    expect_rejected(core, clock, 1, 5.0, StopReason::InvalidMotion);
    // A perfectly valid follow-up is still refused with the original reason.
    expect_rejected(core, clock, 1, 0.2, StopReason::InvalidMotion);
    // A later, different kind of error does not overwrite the reason.
    expect_rejected(core, clock, 99, 0.2, StopReason::InvalidMotion);
    CHECK(!core.watchdog_should_run());
}

TEST(sr4_rejected_request_does_not_advance_cycle) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, 0.1).permitted);
    send(core, clock, 1, 5.0);
    CHECK(core.expected_cycle() == 1);
}

TEST(sr4_watchdog_reason_survives_later_invalid_request) {
    // Regression: the reported reason once depended on the validity of the
    // current request rather than on what caused the stop.
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, 0.1).permitted);
    core.on_tick(clock.advance_ms(20));
    expect_rejected(core, clock, 1, kNaN, StopReason::WatchdogTimeout);
}

TEST(sr4_watchdog_does_not_overwrite_earlier_stop) {
    SafetyCore core;
    FakeClock clock;
    send(core, clock, 0, kNaN);
    core.on_tick(clock.advance_ms(1000));
    CHECK(core.stop_reason() == StopReason::InvalidMotion);
}

// --- SR-5: absolute heartbeat deadline -------------------------------------

TEST(sr5_deadline_is_set_from_accepted_request) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, 0.1).permitted);
    CHECK(core.deadline_ns() == clock.now + kWatchdogTimeoutNs);
}

TEST(sr5_watchdog_ignored_before_first_request) {
    SafetyCore core;
    FakeClock clock;
    core.on_tick(clock.advance_ms(1000));
    CHECK(!core.stopped());
    CHECK(send(core, clock, 0, 0.1).permitted);
}

TEST(sr5_tick_before_deadline_is_harmless) {
    // A stale or forged timer pulse must not stop a healthy controller.
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, 0.1).permitted);
    core.on_tick(clock.now);
    core.on_tick(clock.now + kWatchdogTimeoutNs);  // Exactly on deadline.
    CHECK(!core.stopped());
    CHECK(send(core, clock, 1, 0.2).permitted);
}

TEST(sr5_tick_after_deadline_latches) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, 0.1).permitted);
    core.on_tick(core.deadline_ns() + 1);
    CHECK(core.stopped());
    CHECK(!core.watchdog_should_run());
    expect_rejected(core, clock, 1, 0.2, StopReason::WatchdogTimeout);
}

TEST(sr5_late_request_is_rejected_without_a_tick) {
    // The request itself enforces the deadline, even if the timer pulse
    // has not been delivered or processed yet.
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, 0.1).permitted);
    const double before = core.position();
    const Decision d = core.on_request(1, 0.2, clock.advance_ms(20));
    CHECK(!d.permitted);
    CHECK(d.reason == StopReason::WatchdogTimeout);
    CHECK(core.position() == before);
}

TEST(sr5_request_exactly_on_deadline_is_accepted) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, 0.1).permitted);
    CHECK(core.on_request(1, 0.2, core.deadline_ns()).permitted);
}

TEST(sr5_late_wins_over_invalid_motion) {
    // The missed heartbeat happened first, so it is the reported cause.
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, 0.1).permitted);
    core.on_request(1, kNaN, clock.advance_ms(30));
    CHECK(core.stop_reason() == StopReason::WatchdogTimeout);
}

TEST(sr5_injected_freeze_scenario) {
    // Mirrors the controller's default run: 10 ms period, one extra 20 ms
    // freeze before cycle 40.
    SafetyCore core;
    FakeClock clock;
    double position = 0.0;
    std::uint32_t cycle = 0;
    for (; cycle < 40; ++cycle) {
        position += kMaxStep;
        CHECK(send(core, clock, cycle, position).permitted);
    }
    const std::uint64_t freeze_end = clock.advance_ms(kCycleMs + 20);
    // The timer fires at the deadline, well before the frozen request.
    core.on_tick(core.deadline_ns() + 1);
    CHECK(core.stop_reason() == StopReason::WatchdogTimeout);
    const Decision d = core.on_request(cycle, position + kMaxStep, freeze_end);
    CHECK(!d.permitted);
    CHECK(d.reason == StopReason::WatchdogTimeout);
}

TEST(sr5_invalid_first_request_still_arms) {
    SafetyCore core;
    FakeClock clock;
    send(core, clock, 0, kNaN);
    CHECK(core.armed());
}

// --- SR-6: monitor faults fail safe ----------------------------------------

TEST(sr6_fault_latches_even_before_arming) {
    SafetyCore core;
    FakeClock clock;
    core.on_fault();
    expect_rejected(core, clock, 0, 0.1, StopReason::MonitorFault);
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

int main() { return testing::run_all(); }
