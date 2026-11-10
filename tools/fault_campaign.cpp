// Fault-injection campaign: runs every scenario many times against the
// safety core in simulated time and reports whether the monitor reacted
// exactly as specified.
//
//   make campaign                      # 200 trials per scenario
//   build/fault_campaign --trials 1000 --csv build/campaign.csv
//
// Simulation model (host only, NOT real-time evidence): the controller
// releases each scenario step on an absolute schedule (no drift); each
// request reaches the monitor after a random delivery latency (wake-up,
// scheduling and IPC delay, default 0-2 ms), and the watchdog timer fires
// at the deadline plus a random timer latency (default 0-200 us). On QNX
// the same scenarios run through the real controller and monitor
// processes.
//
// The delivery latency matters: an early version modelled delivery as
// instant, and scenarios with only 1 ms of margin before the deadline
// passed here but failed on QNX in QEMU.
//
// For every committed move the tool also checks that the instrument stayed
// inside the envelope; any breach is reported as a failure.
//
// Exit status is non-zero if any trial did not behave as expected.

#include "safety_core.hpp"
#include "scenarios.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

using namespace surgisafe;

namespace {

struct Options {
    int trials = 200;
    std::uint64_t seed = 20261108;
    std::uint64_t max_timer_latency_ns = 200'000;
    // Defaults to the timing profile's expected latency.
    std::uint64_t max_delivery_latency_ns = 0;
    bool delivery_latency_set = false;
    const TimingProfile* profile = &hardware_profile();
    const char* csv_path = nullptr;
    const char* only = nullptr;
};

struct TrialResult {
    bool stopped = false;
    StopReason reason = StopReason::None;
    std::uint32_t stop_cycle = 0;
    // Time between the last accepted request and the stop, for watchdog
    // stops (how long the instrument was unsupervised).
    std::uint64_t silence_ns = 0;
    std::uint64_t accepted = 0;
    std::uint64_t rejected_after_stop = 0;
    bool envelope_breach = false;
    bool latch_broken = false;
};

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

TrialResult run_trial(const ScenarioInfo& info, std::uint64_t seed,
                      const Options& opt) {
    const Envelope env = default_envelope(*opt.profile);
    SafetyCore core(env, {env.min_x, env.min_y});
    ScenarioStream stream(info, env, *opt.profile, seed);
    std::mt19937_64 rng(seed ^ 0x9E3779B97F4A7C15ULL);
    std::uniform_int_distribution<std::uint64_t> latency(
        0, opt.max_timer_latency_ns);
    std::uniform_int_distribution<std::uint64_t> delivery(
        0, opt.max_delivery_latency_ns);

    TrialResult r;
    std::uint64_t release = 1'000'000'000ULL;  // Controller's schedule.
    std::uint64_t now = release;
    std::uint64_t last_accept = now;
    int extra_after_stop = 0;

    auto stop_at = [&](std::uint64_t t, std::uint32_t cycle) {
        if (!r.stopped && core.stopped()) {
            r.stopped = true;
            r.reason = core.stop_reason();
            r.stop_cycle = cycle;
            if (r.reason == StopReason::WatchdogTimeout) {
                r.silence_ns = t - last_accept;
            }
        }
    };

    ScenarioStep step{};
    while (stream.next(step)) {
        release += step.gap_ns;
        const std::uint64_t send_at =
            std::max(now, release + delivery(rng));  // Arrival time.
        // The watchdog timer fires before the request if its (latency-
        // delayed) expiry comes first.
        if (core.watchdog_should_run()) {
            const std::uint64_t fire = core.deadline_ns() + latency(rng);
            if (fire < send_at) {
                core.on_tick(fire);
                stop_at(fire, step.cycle);
            }
        }
        now = send_at;
        const Decision d = core.on_request(step.cycle, step.target, now);
        stop_at(now, step.cycle);
        if (d.permitted) {
            ++r.accepted;
            last_accept = now;
            if (!inside_envelope(env, core.position())) r.envelope_breach = true;
        } else if (r.stopped) {
            // After a stop, keep sending a few requests: all must be refused
            // with the original reason.
            ++r.rejected_after_stop;
            if (d.reason != r.reason) r.latch_broken = true;
            if (++extra_after_stop >= 3) break;
        }
    }

    // Stream ended. A crash means silence: let the timer fire.
    if (info.kind == ScenarioKind::Crash && core.watchdog_should_run()) {
        const std::uint64_t fire = core.deadline_ns() + latency(rng);
        core.on_tick(fire);
        stop_at(fire, kFaultCycle);
    }
    return r;
}

bool trial_passed(const ScenarioInfo& info, const TrialResult& r) {
    if (r.envelope_breach || r.latch_broken) return false;
    if (!info.expect_stop) return !r.stopped;
    if (!r.stopped) return false;
    return info.expected == StopReason::None || r.reason == info.expected;
}

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage: %s [--trials N] [--seed S] [--timer-latency-us U]\n"
                 "          [--delivery-latency-us U] [--profile hardware|qemu]\n"
                 "          [--scenario NAME] [--csv PATH] [--list]\n",
                 argv0);
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--list") {
            for (const ScenarioInfo* s = scenario_table(); s->name; ++s) {
                std::printf("%-16s %s\n", s->name, s->description);
            }
            return 0;
        } else if (a == "--trials" && has_value) {
            opt.trials = std::atoi(argv[++i]);
        } else if (a == "--seed" && has_value) {
            opt.seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--delivery-latency-us" && has_value) {
            opt.max_delivery_latency_ns =
                std::strtoull(argv[++i], nullptr, 10) * 1000ULL;
            opt.delivery_latency_set = true;
        } else if (a == "--profile" && has_value) {
            opt.profile = find_timing_profile(argv[++i]);
            if (!opt.profile) {
                std::fprintf(stderr, "Unknown profile (try hardware, qemu)\n");
                return 2;
            }
        } else if (a == "--timer-latency-us" && has_value) {
            opt.max_timer_latency_ns =
                std::strtoull(argv[++i], nullptr, 10) * 1000ULL;
        } else if (a == "--scenario" && has_value) {
            opt.only = argv[++i];
        } else if (a == "--csv" && has_value) {
            opt.csv_path = argv[++i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!opt.delivery_latency_set) {
        opt.max_delivery_latency_ns = opt.profile->expected_latency_ns;
    }
    if (opt.trials <= 0) {
        usage(argv[0]);
        return 2;
    }
    if (opt.only && !find_scenario(opt.only)) {
        std::fprintf(stderr, "Unknown scenario '%s' (try --list)\n", opt.only);
        return 2;
    }

    std::FILE* csv = nullptr;
    if (opt.csv_path) {
        csv = std::fopen(opt.csv_path, "w");
        if (!csv) {
            std::perror(opt.csv_path);
            return 1;
        }
        std::fprintf(csv, "scenario,trial,seed,expected,actual,stop_cycle,"
                          "silence_us,accepted,envelope_breach,result\n");
    }

    std::printf("SurgiSafe-RT fault-injection campaign (simulated time)\n");
    std::printf("timing profile %s: period %.0f ms, deadline %.0f ms, "
                "test margin %.0f ms (%s)\n",
                opt.profile->name, opt.profile->period_ns / 1e6,
                opt.profile->deadline_ns / 1e6, opt.profile->margin_ns / 1e6,
                opt.profile->description);
    std::printf("trials/scenario=%d seed=%llu timer latency=0-%llu us "
                "delivery latency=0-%llu us\n\n",
                opt.trials, static_cast<unsigned long long>(opt.seed),
                static_cast<unsigned long long>(opt.max_timer_latency_ns /
                                                1000),
                static_cast<unsigned long long>(
                    opt.max_delivery_latency_ns / 1000));
    std::printf("%-16s %-18s %7s %7s %13s %13s\n", "scenario", "expected",
                "pass", "fail", "max silence", "moves/trial");

    int total_fail = 0;
    std::uint64_t total_moves = 0, total_breaches = 0;
    for (const ScenarioInfo* s = scenario_table(); s->name; ++s) {
        if (opt.only && std::strcmp(opt.only, s->name) != 0) continue;
        int pass = 0, fail = 0;
        std::uint64_t max_silence = 0, moves = 0;
        for (int t = 0; t < opt.trials; ++t) {
            const std::uint64_t seed =
                opt.seed + static_cast<std::uint64_t>(t) * 7919;
            const TrialResult r = run_trial(*s, seed, opt);
            const bool ok = trial_passed(*s, r);
            ok ? ++pass : ++fail;
            max_silence = std::max(max_silence, r.silence_ns);
            moves += r.accepted;
            total_breaches += r.envelope_breach ? 1 : 0;
            if (csv) {
                std::fprintf(csv, "%s,%d,%llu,%s,%s,%u,%.1f,%llu,%d,%s\n",
                             s->name, t, static_cast<unsigned long long>(seed),
                             !s->expect_stop ? "no stop"
                             : s->expected == StopReason::None
                                 ? "any stop"
                                 : to_string(s->expected),
                             r.stopped ? to_string(r.reason) : "no stop",
                             r.stop_cycle, r.silence_ns / 1000.0,
                             static_cast<unsigned long long>(r.accepted),
                             r.envelope_breach ? 1 : 0, ok ? "PASS" : "FAIL");
            }
        }
        total_fail += fail;
        total_moves += moves;
        char silence[32] = "-";
        if (max_silence > 0) {
            std::snprintf(silence, sizeof silence, "%.3f ms",
                          max_silence / 1e6);
        }
        const char* expected = !s->expect_stop ? "no stop"
                               : s->expected == StopReason::None
                                   ? "any stop"
                                   : to_string(s->expected);
        std::printf("%-16s %-18s %7d %7d %13s %13llu\n", s->name, expected,
                    pass, fail, silence,
                    static_cast<unsigned long long>(moves / opt.trials));
    }
    if (csv) std::fclose(csv);

    std::printf("\n%llu moves committed, %llu envelope breaches, "
                "%d failing trials\n",
                static_cast<unsigned long long>(total_moves),
                static_cast<unsigned long long>(total_breaches), total_fail);
    std::printf("%s\n", total_fail == 0 ? "CAMPAIGN PASSED" : "CAMPAIGN FAILED");
    return total_fail == 0 ? 0 : 1;
}
