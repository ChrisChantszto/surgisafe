// Randomized property tests for the safety core.
//
// Drives SafetyCore with long random sequences of requests and timer ticks
// (sometimes well-behaved, sometimes hostile) and checks invariants that
// must hold after every single event, whatever the input. Seeds are fixed
// so failures are reproducible; set SURGISAFE_SEED to explore other runs.

#include "motion_shaper.hpp"
#include "safety_core.hpp"
#include "test_framework.hpp"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <random>

using namespace surgisafe;

namespace {

constexpr int kRuns = 2000;
constexpr int kEventsPerRun = 300;

std::uint64_t base_seed() {
    if (const char* env = std::getenv("SURGISAFE_SEED")) {
        return std::strtoull(env, nullptr, 10);
    }
    return 20261108;
}

// A proposed position that is usually a plausible step but is sometimes
// wildly invalid, so both accept and reject paths get exercised.
Point random_position(std::mt19937_64& rng, Point current, Point velocity,
                      const Envelope& env) {
    std::uniform_int_distribution<int> kind(0, 99);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    const int k = kind(rng);
    if (k < 70) {
        // Near the current velocity, so the acceleration limit is often met.
        const double a = env.max_step_change * 1.3;
        return Point{current.x + velocity.x + unit(rng) * a,
                     current.y + velocity.y + unit(rng) * a};
    }
    if (k < 80) return current;  // Hold (may still violate accel).
    if (k < 85) return Point{std::numeric_limits<double>::quiet_NaN(), 0.0};
    if (k < 88) return Point{0.0, std::numeric_limits<double>::infinity()};
    return Point{unit(rng) * 20.0, unit(rng) * 20.0};
}

bool inside_envelope(const Envelope& env, Point p) {
    if (!inside_bounds(env, p)) return false;
    for (std::size_t i = 0; i < env.zone_count; ++i) {
        if (distance(p, env.zones[i].center) <
            env.zones[i].radius - kTolerance) {
            return false;
        }
    }
    return true;
}

struct Snapshot {
    Point position;
    Point velocity;
    std::uint32_t expected_cycle;
    bool armed;
    std::uint64_t deadline;
    bool stopped;
    StopReason reason;
};

Snapshot snap(const SafetyCore& c) {
    return {c.position(), c.velocity(), c.expected_cycle(), c.armed(),
            c.deadline_ns(), c.stopped(), c.stop_reason()};
}

// Invariants that hold after every event, regardless of history.
void check_invariants(const SafetyCore& core, const Snapshot& before) {
    // The instrument never leaves the envelope.
    CHECK(inside_envelope(core.envelope(), core.position()));
    // A stop is permanent and keeps its first reason.
    if (before.stopped) {
        CHECK(core.stopped());
        CHECK(core.stop_reason() == before.reason);
        CHECK(core.position().x == before.position.x);
        CHECK(core.position().y == before.position.y);
        CHECK(core.expected_cycle() == before.expected_cycle);
    }
    CHECK(core.stopped() == (core.stop_reason() != StopReason::None));
    CHECK(!(core.watchdog_should_run() && core.stopped()));
}

TEST(property_invariants_hold_under_random_events) {
    const std::uint64_t seed = base_seed();
    const Envelope env = default_envelope();
    for (int run = 0; run < kRuns; ++run) {
        std::mt19937_64 rng(seed + static_cast<std::uint64_t>(run));
        std::uniform_int_distribution<int> event(0, 99);
        // Gaps cluster around the 10 ms period but sometimes exceed 15 ms.
        std::uniform_int_distribution<std::uint64_t> gap_us(0, 17'000);
        std::uniform_int_distribution<int> cycle_noise(0, 99);
        std::uniform_real_distribution<double> start(0.0, 8.0);

        // Random start somewhere legal.
        Point origin{start(rng), start(rng)};
        while (!inside_envelope(env, origin)) origin = {start(rng), start(rng)};
        SafetyCore core(env, origin);
        std::uint64_t now = 1'000'000'000ULL;
        const int failures_before = testing::g_failures;

        for (int i = 0; i < kEventsPerRun; ++i) {
            now += gap_us(rng) * 1000ULL;
            const Snapshot before = snap(core);
            const int e = event(rng);

            if (e < 85) {
                std::uint32_t cycle = core.expected_cycle();
                const int n = cycle_noise(rng);
                if (n < 2) cycle += 1;                 // Skip.
                else if (n < 4 && cycle > 0) --cycle;  // Replay.
                else if (n < 5) cycle = static_cast<std::uint32_t>(rng());

                const Point p = random_position(rng, core.position(),
                                                core.velocity(), env);
                const Decision d = core.on_request(cycle, p, now);

                if (d.permitted) {
                    // Every accepted move satisfied every rule.
                    const Point step{p.x - before.position.x,
                                     p.y - before.position.y};
                    CHECK(!before.stopped);
                    CHECK(!before.armed || now <= before.deadline);
                    CHECK(cycle == before.expected_cycle);
                    CHECK(std::hypot(step.x, step.y) <=
                          env.max_step + kTolerance);
                    CHECK(std::hypot(step.x - before.velocity.x,
                                     step.y - before.velocity.y) <=
                          env.max_step_change + kTolerance);
                    CHECK(!segment_hits_zone(env, before.position, p));
                    CHECK(d.reason == StopReason::None);
                    CHECK(core.expected_cycle() == before.expected_cycle + 1);
                    CHECK(core.deadline_ns() ==
                          now + env.watchdog_timeout_ns);
                } else {
                    // A rejection never moves the instrument.
                    CHECK(core.stopped());
                    CHECK(d.reason == core.stop_reason());
                }
                CHECK(d.actual.x == core.position().x);
                CHECK(d.actual.y == core.position().y);
            } else if (e < 98) {
                const bool was_running = core.watchdog_should_run();
                core.on_tick(now);
                // A tick stops a running watchdog if and only if the
                // deadline has passed.
                if (was_running) {
                    CHECK(core.stopped() == (now > before.deadline));
                }
            } else {
                core.on_fault();
                CHECK(core.stopped());
            }

            check_invariants(core, before);
            if (testing::g_failures != failures_before) {
                std::fprintf(stderr, "  first failure: run %d (seed %llu), "
                             "event %d\n", run,
                             static_cast<unsigned long long>(seed + run), i);
                return;
            }
        }
    }
}

TEST(property_shaped_controller_never_stops) {
    // A controller that shapes its motion and sends on time is never
    // stopped, whatever targets it is given (here: random targets that
    // avoid the zone) and however often the timer fires.
    const Envelope env = default_envelope();
    std::mt19937_64 rng(base_seed());
    std::uniform_int_distribution<std::uint64_t> gap_ns(
        1, env.watchdog_timeout_ns);
    std::uniform_real_distribution<double> coord(0.0, 8.0);

    SafetyCore core(env);
    MotionShaper shaper(env, {0.0, 0.0});
    std::uint64_t now = 1'000'000'000ULL;
    std::uint32_t cycle = 0;
    Point target{0.0, 0.0};
    for (int i = 0; i < 100'000; ++i) {
        if (shaper.arrived(target, 1e-3)) {
            // Pick a new target whose straight path avoids the zone.
            do {
                target = {coord(rng), coord(rng)};
            } while (segment_hits_zone(env, shaper.position(), target) ||
                     distance_to_segment(env.zones[0].center,
                                         shaper.position(), target) <
                         env.zones[0].radius + 0.2);
        }
        now += gap_ns(rng);
        core.on_tick(now);  // Possibly-stale tick just before the request.
        const Decision d =
            core.on_request(cycle++, shaper.step_toward(target), now);
        CHECK(d.permitted);
        if (!d.permitted) {
            std::fprintf(stderr, "  stopped at cycle %u: %s\n", cycle - 1,
                         to_string(d.reason));
            return;
        }
    }
}

} // namespace
