// Randomized property tests for the safety core.
//
// Drives SafetyCore with long random sequences of requests and timer ticks
// (sometimes well-behaved, sometimes hostile) and checks invariants that
// must hold after every single event, whatever the input. Seeds are fixed
// so failures are reproducible; set SURGISAFE_SEED to explore other runs.

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
double random_position(std::mt19937_64& rng, double current) {
    std::uniform_int_distribution<int> kind(0, 99);
    const int k = kind(rng);
    if (k < 70) {  // Plausible step, occasionally slightly too large.
        std::uniform_real_distribution<double> step(-kMaxStep * 1.05,
                                                    kMaxStep * 1.05);
        return current + step(rng);
    }
    if (k < 80) return current;  // Hold.
    if (k < 85) return std::numeric_limits<double>::quiet_NaN();
    if (k < 88) return std::numeric_limits<double>::infinity();
    if (k < 90) return -std::numeric_limits<double>::infinity();
    std::uniform_real_distribution<double> anywhere(-20.0, 20.0);
    return anywhere(rng);
}

struct Snapshot {
    double position;
    std::uint32_t expected_cycle;
    bool stopped;
    StopReason reason;
};

Snapshot snap(const SafetyCore& c) {
    return {c.position(), c.expected_cycle(), c.stopped(), c.stop_reason()};
}

// Invariants that hold after every event, regardless of history.
void check_invariants(const SafetyCore& core, const Snapshot& before) {
    // The instrument never leaves the workspace.
    CHECK(std::isfinite(core.position()));
    CHECK(core.position() >= 0.0 && core.position() <= kMaxPosition);
    // A stop is permanent and keeps its first reason.
    if (before.stopped) {
        CHECK(core.stopped());
        CHECK(core.stop_reason() == before.reason);
        CHECK(core.position() == before.position);
        CHECK(core.expected_cycle() == before.expected_cycle);
    }
    CHECK(core.stopped() == (core.stop_reason() != StopReason::None));
    CHECK(!(core.watchdog_should_run() && core.stopped()));
}

TEST(property_invariants_hold_under_random_events) {
    const std::uint64_t seed = base_seed();
    for (int run = 0; run < kRuns; ++run) {
        std::mt19937_64 rng(seed + static_cast<std::uint64_t>(run));
        std::uniform_int_distribution<int> event(0, 99);
        // Gaps cluster around the 10 ms period but sometimes exceed 15 ms.
        std::uniform_int_distribution<std::uint64_t> gap_us(0, 25'000);
        std::uniform_int_distribution<int> cycle_noise(0, 99);

        SafetyCore core;
        std::uint64_t now = 1'000'000'000ULL;
        const int failures_before = testing::g_failures;

        for (int i = 0; i < kEventsPerRun; ++i) {
            now += gap_us(rng) * 1000ULL;
            const Snapshot before = snap(core);
            const bool armed_before = core.armed();
            const std::uint64_t deadline_before = core.deadline_ns();
            const int e = event(rng);

            if (e < 80) {
                std::uint32_t cycle = core.expected_cycle();
                const int n = cycle_noise(rng);
                if (n < 3) cycle += 1;              // Skip.
                else if (n < 5 && cycle > 0) --cycle;  // Replay.
                else if (n < 6) cycle = static_cast<std::uint32_t>(rng());

                const double proposed = random_position(rng, core.position());
                const Decision d = core.on_request(cycle, proposed, now);

                if (d.permitted) {
                    // Every accepted move satisfied every rule.
                    CHECK(!before.stopped);
                    CHECK(!armed_before || now <= deadline_before);
                    CHECK(cycle == before.expected_cycle);
                    CHECK(std::fabs(proposed - before.position) <=
                          kMaxStep + kStepTolerance);
                    CHECK(d.reason == StopReason::None);
                    CHECK(core.position() == proposed);
                    CHECK(core.expected_cycle() == before.expected_cycle + 1);
                    CHECK(core.deadline_ns() == now + kWatchdogTimeoutNs);
                } else {
                    // A rejection never moves the instrument.
                    CHECK(core.stopped());
                    CHECK(d.reason == core.stop_reason());
                    CHECK(core.position() == before.position);
                }
                CHECK(d.actual_position == core.position());
            } else if (e < 98) {
                const bool was_running = core.watchdog_should_run();
                const std::uint64_t deadline = core.deadline_ns();
                core.on_tick(now);
                // A tick stops a running watchdog if and only if the
                // deadline has passed.
                if (was_running) {
                    CHECK(core.stopped() == (now > deadline));
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

TEST(property_healthy_controller_never_stops) {
    // With on-time heartbeats and valid steps, the monitor never stops,
    // however long it runs and however often the timer fires.
    std::mt19937_64 rng(base_seed());
    std::uniform_int_distribution<std::uint64_t> gap_ns(
        1, kWatchdogTimeoutNs);  // Always within the deadline.
    std::uniform_real_distribution<double> step(-kMaxStep, kMaxStep);

    SafetyCore core;
    std::uint64_t now = 1'000'000'000ULL;
    for (std::uint32_t cycle = 0; cycle < 100'000; ++cycle) {
        now += gap_ns(rng);
        core.on_tick(now);  // Possibly-stale tick just before the request.
        double next = core.position() + step(rng);
        next = std::fmin(std::fmax(next, 0.0), kMaxPosition);
        const Decision d = core.on_request(cycle, next, now);
        CHECK(d.permitted);
        if (!d.permitted) {
            std::fprintf(stderr, "  stopped at cycle %u: %s\n", cycle,
                         to_string(d.reason));
            return;
        }
    }
}

} // namespace
