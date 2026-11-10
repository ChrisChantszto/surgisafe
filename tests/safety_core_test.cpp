// Host-side unit tests for the portable safety core. No QNX required:
//   make test
//
// Test names are tagged with the safety requirement they verify; see
// docs/HAZARDS.md for the requirement list.

#include "motion_shaper.hpp"
#include "safety_core.hpp"
#include "test_framework.hpp"

#include <cmath>
#include <limits>
#include <string_view>

using namespace surgisafe;
using testing::FakeClock;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr std::uint64_t kCycleMs = 10;  // Controller period.

// An envelope that isolates one rule at a time: no zones, and an
// acceleration limit too large to matter.
Envelope loose_envelope() {
    Envelope env;
    env.max_step_change = 1e9;
    return env;
}

bool same(Point a, Point b) { return a.x == b.x && a.y == b.y; }

// Sends a request 10 ms after the previous one (a healthy controller).
Decision send(SafetyCore& core, FakeClock& clock, std::uint32_t cycle,
              Point proposed) {
    return core.on_request(cycle, proposed, clock.advance_ms(kCycleMs));
}

// Expects a request to be rejected with `reason` and leave state untouched.
void expect_rejected(SafetyCore& core, FakeClock& clock, std::uint32_t cycle,
                     Point proposed, StopReason reason) {
    const Point before = core.position();
    const Decision d = send(core, clock, cycle, proposed);
    CHECK(!d.permitted);
    CHECK(d.reason == reason);
    CHECK(same(d.actual, before));
    CHECK(same(core.position(), before));
    CHECK(core.stopped());
    CHECK(core.stop_reason() == reason);
}

// --- Initial state ---------------------------------------------------------

TEST(starts_unarmed_at_origin) {
    SafetyCore core;
    CHECK(!core.armed());
    CHECK(!core.stopped());
    CHECK(!core.watchdog_should_run());
    CHECK(same(core.position(), Point{0.0, 0.0}));
    CHECK(core.expected_cycle() == 0);
    CHECK(core.stop_reason() == StopReason::None);
}

// --- SR-1: speed limit -----------------------------------------------------

TEST(sr1_valid_step_is_committed) {
    SafetyCore core(loose_envelope());
    FakeClock clock;
    const Decision d = send(core, clock, 0, {0.03, 0.04});
    CHECK(d.permitted);
    CHECK(d.reason == StopReason::None);
    CHECK(same(d.actual, Point{0.03, 0.04}));
    CHECK(same(core.velocity(), Point{0.03, 0.04}));
    CHECK(core.expected_cycle() == 1);
    CHECK(core.watchdog_should_run());
}

TEST(sr1_zero_step_is_allowed) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, {0.0, 0.0}).permitted);
    CHECK(send(core, clock, 1, {0.0, 0.0}).permitted);
}

TEST(sr1_exact_max_step_is_allowed) {
    SafetyCore core(loose_envelope());
    FakeClock clock;
    CHECK(send(core, clock, 0, {0.1, 0.0}).permitted);
    CHECK(send(core, clock, 1, {0.1, 0.1}).permitted);
    CHECK(send(core, clock, 2, {0.1 + 0.06, 0.1 + 0.08}).permitted);  // 3-4-5
}

TEST(sr1_speed_limit_is_euclidean) {
    // 0.08 in x and 0.08 in y is a 0.113 step: too fast, though each axis
    // alone is under the limit.
    SafetyCore core(loose_envelope());
    FakeClock clock;
    expect_rejected(core, clock, 0, {0.08, 0.08}, StopReason::SpeedLimit);
}

TEST(sr1_step_too_large_is_rejected) {
    SafetyCore core(loose_envelope());
    FakeClock clock;
    expect_rejected(core, clock, 0, {0.1 + 1e-6, 0.0}, StopReason::SpeedLimit);
}

// --- SR-2: workspace bounds ------------------------------------------------

TEST(sr2_negative_coordinates_are_rejected) {
    SafetyCore a(loose_envelope());
    FakeClock clock;
    expect_rejected(a, clock, 0, {-1e-6, 0.0}, StopReason::OutOfWorkspace);
    SafetyCore b(loose_envelope());
    expect_rejected(b, clock, 0, {0.0, -1e-6}, StopReason::OutOfWorkspace);
}

TEST(sr2_upper_bounds_are_inclusive) {
    Envelope env = loose_envelope();
    SafetyCore core(env, {8.0 - 0.05, 8.0 - 0.05});
    FakeClock clock;
    CHECK(send(core, clock, 0, {8.0, 8.0}).permitted);
    expect_rejected(core, clock, 1, {8.0 + 1e-6, 8.0},
                    StopReason::OutOfWorkspace);
}

TEST(sr2_non_finite_is_rejected) {
    const Point bad[] = {{kNaN, 0.0}, {0.0, kNaN}, {kInf, 0.0}, {0.0, -kInf}};
    for (const Point p : bad) {
        SafetyCore core(loose_envelope());
        FakeClock clock;
        expect_rejected(core, clock, 0, p, StopReason::OutOfWorkspace);
    }
}

TEST(sr2_workspace_error_wins_over_sequence_and_speed) {
    SafetyCore core(loose_envelope());
    FakeClock clock;
    expect_rejected(core, clock, 7, {-5.0, kNaN}, StopReason::OutOfWorkspace);
}

// --- SR-3: requests must arrive in order -----------------------------------

TEST(sr3_first_cycle_must_be_zero) {
    SafetyCore core;
    FakeClock clock;
    expect_rejected(core, clock, 1, {0.0, 0.0}, StopReason::InvalidSequence);
}

TEST(sr3_skipped_cycle_is_rejected) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, {0.01, 0.0}).permitted);
    expect_rejected(core, clock, 2, {0.02, 0.0}, StopReason::InvalidSequence);
}

TEST(sr3_replayed_cycle_is_rejected) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, {0.01, 0.0}).permitted);
    expect_rejected(core, clock, 0, {0.02, 0.0}, StopReason::InvalidSequence);
}

// --- SR-4: a stop is latched with its first cause --------------------------

TEST(sr4_stop_is_latched_and_keeps_first_reason) {
    SafetyCore core(loose_envelope());
    FakeClock clock;
    CHECK(send(core, clock, 0, {0.1, 0.0}).permitted);
    expect_rejected(core, clock, 1, {5.0, 0.0}, StopReason::SpeedLimit);
    // A perfectly valid follow-up is still refused with the original reason.
    expect_rejected(core, clock, 1, {0.2, 0.0}, StopReason::SpeedLimit);
    // A later, different kind of error does not overwrite the reason.
    expect_rejected(core, clock, 99, {kNaN, 0.0}, StopReason::SpeedLimit);
    CHECK(!core.watchdog_should_run());
}

TEST(sr4_rejected_request_does_not_change_state) {
    SafetyCore core(loose_envelope());
    FakeClock clock;
    CHECK(send(core, clock, 0, {0.1, 0.0}).permitted);
    send(core, clock, 1, {5.0, 0.0});
    CHECK(core.expected_cycle() == 1);
    CHECK(same(core.velocity(), Point{0.1, 0.0}));
}

TEST(sr4_watchdog_reason_survives_later_invalid_request) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, {0.01, 0.0}).permitted);
    core.on_tick(clock.advance_ms(20));
    expect_rejected(core, clock, 1, {kNaN, 0.0}, StopReason::WatchdogTimeout);
}

TEST(sr4_watchdog_does_not_overwrite_earlier_stop) {
    SafetyCore core;
    FakeClock clock;
    send(core, clock, 0, {kNaN, 0.0});
    core.on_tick(clock.advance_ms(1000));
    CHECK(core.stop_reason() == StopReason::OutOfWorkspace);
}

// --- SR-5: absolute heartbeat deadline -------------------------------------

TEST(sr5_deadline_is_set_from_accepted_request) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, {0.01, 0.0}).permitted);
    CHECK(core.deadline_ns() ==
          clock.now + core.envelope().watchdog_timeout_ns);
}

TEST(sr5_watchdog_ignored_before_first_request) {
    SafetyCore core;
    FakeClock clock;
    core.on_tick(clock.advance_ms(1000));
    CHECK(!core.stopped());
    CHECK(send(core, clock, 0, {0.01, 0.0}).permitted);
}

TEST(sr5_tick_before_or_on_deadline_is_harmless) {
    // A stale or forged timer pulse must not stop a healthy controller.
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, {0.01, 0.0}).permitted);
    core.on_tick(clock.now);
    core.on_tick(core.deadline_ns());
    CHECK(!core.stopped());
    CHECK(send(core, clock, 1, {0.02, 0.0}).permitted);
}

TEST(sr5_tick_after_deadline_latches) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, {0.01, 0.0}).permitted);
    core.on_tick(core.deadline_ns() + 1);
    CHECK(core.stopped());
    CHECK(!core.watchdog_should_run());
    expect_rejected(core, clock, 1, {0.02, 0.0}, StopReason::WatchdogTimeout);
}

TEST(sr5_late_request_is_rejected_without_a_tick) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, {0.01, 0.0}).permitted);
    const Decision d = core.on_request(1, {0.02, 0.0}, clock.advance_ms(20));
    CHECK(!d.permitted);
    CHECK(d.reason == StopReason::WatchdogTimeout);
    CHECK(same(core.position(), Point{0.01, 0.0}));
}

TEST(sr5_request_exactly_on_deadline_is_accepted) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, {0.01, 0.0}).permitted);
    CHECK(core.on_request(1, {0.02, 0.0}, core.deadline_ns()).permitted);
}

TEST(sr5_late_wins_over_every_other_error) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, {0.01, 0.0}).permitted);
    core.on_request(7, {kNaN, 99.0}, clock.advance_ms(30));
    CHECK(core.stop_reason() == StopReason::WatchdogTimeout);
}

TEST(sr5_invalid_first_request_still_arms) {
    SafetyCore core;
    FakeClock clock;
    send(core, clock, 0, {kNaN, 0.0});
    CHECK(core.armed());
}

// --- SR-6: monitor faults fail safe ----------------------------------------

TEST(sr6_fault_latches_even_before_arming) {
    SafetyCore core;
    FakeClock clock;
    core.on_fault();
    expect_rejected(core, clock, 0, {0.0, 0.0}, StopReason::MonitorFault);
}

TEST(sr6_start_outside_envelope_refuses_to_move) {
    FakeClock clock;
    SafetyCore outside(default_envelope(), {-1.0, 0.0});
    CHECK(outside.stop_reason() == StopReason::MonitorFault);
    SafetyCore in_zone(default_envelope(), {5.0, 5.0});
    CHECK(in_zone.stop_reason() == StopReason::MonitorFault);
    expect_rejected(in_zone, clock, 0, {5.0, 5.0}, StopReason::MonitorFault);
}

// --- SR-8: acceleration limit ----------------------------------------------

TEST(sr8_first_step_from_rest_is_accel_limited) {
    SafetyCore ok;
    FakeClock clock;
    CHECK(send(ok, clock, 0, {0.02, 0.0}).permitted);
    SafetyCore too_fast;
    expect_rejected(too_fast, clock, 0, {0.05, 0.0}, StopReason::AccelLimit);
}

TEST(sr8_gradual_speed_up_is_allowed) {
    SafetyCore core;
    FakeClock clock;
    double x = 0.0, v = 0.0;
    for (std::uint32_t c = 0; c < 10; ++c) {
        v = std::fmin(v + 0.02, 0.1);
        x += v;
        CHECK(send(core, clock, c, {x, 0.0}).permitted);
    }
    CHECK(std::fabs(core.velocity().x - 0.1) < 1e-12);
}

TEST(sr8_sudden_reversal_is_rejected) {
    SafetyCore core;
    FakeClock clock;
    double x = 0.0;
    for (std::uint32_t c = 0; c < 5; ++c) {
        x += 0.02 * (c + 1);
        CHECK(send(core, clock, c, {x, 0.0}).permitted);
    }
    // Moving +0.1 per cycle; reversing to -0.1 is a 0.2 change.
    expect_rejected(core, clock, 5, {x - 0.1, 0.0}, StopReason::AccelLimit);
}

TEST(sr8_sharp_turn_is_rejected) {
    // Same speed, but a 90-degree turn changes the step by 0.141.
    SafetyCore core;
    FakeClock clock;
    double x = 0.0;
    for (std::uint32_t c = 0; c < 5; ++c) {
        x += 0.02 * (c + 1);
        CHECK(send(core, clock, c, {x, 0.0}).permitted);
    }
    expect_rejected(core, clock, 5, {x, 0.1}, StopReason::AccelLimit);
}

TEST(sr8_speed_error_wins_over_accel_error) {
    SafetyCore core;
    FakeClock clock;
    expect_rejected(core, clock, 0, {0.5, 0.0}, StopReason::SpeedLimit);
}

// --- SR-9: no-go zones -----------------------------------------------------

Envelope zone_envelope() {
    Envelope env = loose_envelope();
    env.zones[0] = NoGoZone{{4.0, 4.0}, 1.0};
    env.zone_count = 1;
    return env;
}

TEST(sr9_entering_zone_is_rejected) {
    SafetyCore core(zone_envelope(), {4.0, 2.95});
    FakeClock clock;
    CHECK(send(core, clock, 0, {4.0, 3.0}).permitted);  // On the boundary.
    expect_rejected(core, clock, 1, {4.0, 3.05}, StopReason::NoGoZone);
}

TEST(sr9_crossing_zone_edge_is_rejected) {
    // Both endpoints are outside, but the straight path clips the zone.
    Envelope env = zone_envelope();
    env.max_step = 3.0;
    const Point from{3.0, 3.1}, to{5.0, 3.1};  // Passes 0.9 from the centre.
    CHECK(distance(from, {4.0, 4.0}) > 1.0);
    CHECK(distance(to, {4.0, 4.0}) > 1.0);
    SafetyCore core(env, from);
    FakeClock clock;
    expect_rejected(core, clock, 0, to, StopReason::NoGoZone);
}

TEST(sr9_passing_beside_zone_is_allowed) {
    SafetyCore core(zone_envelope(), {2.9, 2.9});
    FakeClock clock;
    CHECK(send(core, clock, 0, {2.9, 2.95}).permitted);
}

TEST(sr9_multiple_zones_are_all_enforced) {
    Envelope env = loose_envelope();
    env.zones[0] = NoGoZone{{2.0, 2.0}, 0.5};
    env.zones[1] = NoGoZone{{6.0, 6.0}, 0.5};
    env.zone_count = 2;
    SafetyCore core(env, {6.0, 5.45});
    FakeClock clock;
    expect_rejected(core, clock, 0, {6.0, 5.55}, StopReason::NoGoZone);
}

TEST(sr9_zone_error_wins_over_speed) {
    SafetyCore core(zone_envelope(), {4.0, 2.95});
    FakeClock clock;
    expect_rejected(core, clock, 0, {4.0, 4.0}, StopReason::NoGoZone);
}

// --- SR-10: contact force limit --------------------------------------------

TEST(sr10_force_within_limit_is_allowed) {
    SafetyCore core;
    FakeClock clock;
    core.on_force(0.0);
    core.on_force(core.envelope().max_contact_force_n);  // On the limit.
    CHECK(!core.stopped());
    CHECK(send(core, clock, 0, {0.01, 0.0}).permitted);
}

TEST(sr10_force_over_limit_latches) {
    SafetyCore core;
    FakeClock clock;
    CHECK(send(core, clock, 0, {0.01, 0.0}).permitted);
    core.on_force(core.envelope().max_contact_force_n + 0.01);
    CHECK(core.stop_reason() == StopReason::ContactForce);
    expect_rejected(core, clock, 1, {0.02, 0.0}, StopReason::ContactForce);
}

TEST(sr10_invalid_force_reading_fails_safe) {
    const double bad[] = {kNaN, kInf, -1.0};
    for (const double f : bad) {
        SafetyCore core;
        core.on_force(f);
        CHECK(core.stop_reason() == StopReason::ContactForce);
    }
}

TEST(sr10_force_does_not_overwrite_earlier_stop) {
    SafetyCore core;
    core.on_fault();
    core.on_force(100.0);
    CHECK(core.stop_reason() == StopReason::MonitorFault);
}

// --- Geometry helpers ------------------------------------------------------

TEST(geometry_distance_to_segment) {
    CHECK(std::fabs(distance_to_segment({0, 1}, {-1, 0}, {1, 0}) - 1.0) < 1e-12);
    CHECK(std::fabs(distance_to_segment({3, 0}, {-1, 0}, {1, 0}) - 2.0) < 1e-12);
    CHECK(std::fabs(distance_to_segment({0, 2}, {0, 0}, {0, 0}) - 2.0) < 1e-12);
}

// --- Motion shaper (controller side) ---------------------------------------

TEST(shaper_output_is_always_accepted_by_monitor) {
    const Envelope env = default_envelope();
    SafetyCore core(env);
    MotionShaper shaper(env, {0.0, 0.0});
    FakeClock clock;
    const Point targets[] = {{8.0, 0.0}, {8.0, 3.0}, {1.0, 3.0}, {0.0, 0.0}};
    std::uint32_t cycle = 0;
    for (const Point t : targets) {
        int guard = 0;
        while (!shaper.arrived(t, 1e-3) && ++guard < 2000) {
            const Decision d = send(core, clock, cycle++, shaper.step_toward(t));
            CHECK(d.permitted);
            if (!d.permitted) return;
        }
        CHECK(guard < 2000);
    }
}

// --- Timing profiles -------------------------------------------------------

TEST(timing_profiles_are_consistent) {
    int count = 0;
    for (const TimingProfile* p = timing_profiles(); p->name; ++p, ++count) {
        // A gap "deadline - margin" must still be longer than a period, the
        // margin must exceed the latency the campaign models, and the
        // envelope must carry the profile's deadline.
        CHECK(p->deadline_ns > p->period_ns);
        CHECK(p->deadline_ns - p->margin_ns > p->period_ns);
        CHECK(p->margin_ns > p->expected_latency_ns);
        CHECK(default_envelope(*p).watchdog_timeout_ns == p->deadline_ns);
        CHECK(find_timing_profile(p->name) == p);
    }
    CHECK(count >= 2);
    CHECK(find_timing_profile("nope") == nullptr);
    // The design target stays at a 15 ms deadline.
    CHECK(default_envelope().watchdog_timeout_ns == 15ULL * 1000 * 1000);
    CHECK(std::string_view(find_timing_profile("qemu")->name) == "qemu");
}

// --- Misc ------------------------------------------------------------------

TEST(reason_names_and_wire_values) {
    CHECK(static_cast<int>(StopReason::None) == 0);
    CHECK(static_cast<int>(StopReason::WatchdogTimeout) == 1);
    CHECK(static_cast<int>(StopReason::OutOfWorkspace) == 2);
    CHECK(static_cast<int>(StopReason::InvalidSequence) == 3);
    CHECK(static_cast<int>(StopReason::MonitorFault) == 4);
    CHECK(static_cast<int>(StopReason::SpeedLimit) == 5);
    CHECK(static_cast<int>(StopReason::AccelLimit) == 6);
    CHECK(static_cast<int>(StopReason::NoGoZone) == 7);
    CHECK(static_cast<int>(StopReason::ContactForce) == 8);
    CHECK(std::string_view(to_string(StopReason::NoGoZone)) == "no-go zone");
    CHECK(std::string_view(to_string(static_cast<StopReason>(42))) ==
          "unknown");
}

} // namespace

int main() { return testing::run_all(); }
