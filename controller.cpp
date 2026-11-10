// QNX controller: replays one fault-injection scenario against the running
// safety_monitor, using the same scenario streams as the host campaign
// (scenarios.cpp), so simulated and on-target results are comparable.
//
//   ./controller                     # freeze_long (the classic demo)
//   ./controller --normal            # nominal path, no fault
//   ./controller --scenario reversal
//   ./controller --list
//   ./controller --profile qemu ...  # timing profile; must match the monitor

#include "protocol.hpp"
#include "scenarios.hpp"

#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace surgisafe;
using Clock = std::chrono::steady_clock;

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage: %s [--normal | --scenario NAME | --list] [--seed S]\n"
                 "          [--profile hardware|qemu]\n",
                 argv0);
}

} // namespace

int main(int argc, char** argv) {
    const char* scenario_name = "freeze_long";
    std::uint64_t seed = 1;
    const TimingProfile* profile = &hardware_profile();
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") {
            for (const ScenarioInfo* s = scenario_table(); s->name; ++s) {
                std::printf("%-16s %s\n", s->name, s->description);
            }
            return 0;
        } else if (a == "--normal") {
            scenario_name = "nominal";
        } else if (a == "--scenario" && i + 1 < argc) {
            scenario_name = argv[++i];
        } else if (a == "--seed" && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--profile" && i + 1 < argc) {
            profile = find_timing_profile(argv[++i]);
            if (!profile) {
                std::fprintf(stderr, "Unknown profile (hardware, qemu)\n");
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    const ScenarioInfo* info = find_scenario(scenario_name);
    if (!info) {
        std::fprintf(stderr, "Unknown scenario '%s' (try --list)\n",
                     scenario_name);
        return 2;
    }

    const int connection = name_open(kServiceName, 0);
    if (connection == -1) {
        std::perror("name_open (start safety_monitor first)");
        return 1;
    }

    std::printf("Scenario %s: %s\n", info->name, info->description);
    std::printf("Timing profile %s: period %.0f ms, deadline %.0f ms\n",
                profile->name, profile->period_ns / 1e6,
                profile->deadline_ns / 1e6);
    std::fflush(stdout);

    ScenarioStream stream(*info, default_envelope(*profile), *profile, seed);
    auto next_release = Clock::now();
    int exit_code = 0;
    ScenarioStep step{};
    std::uint32_t steps_sent = 0;
    StopReason stop_reason = StopReason::None;
    std::uint32_t stop_step = 0;
    // Timing evidence: how late each wake-up was versus its scheduled
    // release, and how long each MsgSend round trip took.
    double max_late_ms = 0.0, max_rtt_ms = 0.0, sum_late_ms = 0.0;
    std::uint32_t late_over_2ms = 0;

    while (stream.next(step)) {
        // Absolute release times keep the period from drifting; a freeze
        // shows up as one long gap.
        next_release += std::chrono::nanoseconds(step.gap_ns);
        std::this_thread::sleep_until(next_release);
        const auto woke = Clock::now();
        const double late_ms =
            std::chrono::duration<double, std::milli>(woke - next_release)
                .count();
        sum_late_ms += late_ms;
        if (late_ms > max_late_ms) max_late_ms = late_ms;
        if (late_ms > 2.0) ++late_over_2ms;

        const MoveRequest request{kMoveRequest, 0, step.cycle, step.target.x,
                                  step.target.y};
        MoveReply reply{};
        if (MsgSend(connection, &request, sizeof(request), &reply,
                    sizeof(reply)) == -1) {
            std::perror("MsgSend");
            exit_code = 1;
            break;
        }
        const double rtt_ms = std::chrono::duration<double, std::milli>(
                                  Clock::now() - woke)
                                  .count();
        if (rtt_ms > max_rtt_ms) max_rtt_ms = rtt_ms;
        ++steps_sent;
        if (!reply.permitted) {
            stop_reason = static_cast<StopReason>(reply.reason);
            stop_step = steps_sent - 1;
            std::printf("SAFE_STOP at cycle %u: reason %d (%s), tip held at "
                        "(%.3f, %.3f)\n",
                        step.cycle, reply.reason,
                        to_string(static_cast<StopReason>(reply.reason)),
                        reply.x, reply.y);
            exit_code = 1;
            break;
        }
        if (step.cycle % 10 == 0) {
            std::printf("Cycle %4u | tip (%.3f, %.3f)\n", step.cycle, reply.x,
                        reply.y);
        }
    }
    name_close(connection);

    if (steps_sent > 0) {
        std::printf("TIMING steps=%u wake_late_ms(avg=%.3f max=%.3f) "
                    "late_over_2ms=%u msgsend_rtt_max_ms=%.3f\n",
                    steps_sent, sum_late_ms / steps_sent, max_late_ms,
                    late_over_2ms, max_rtt_ms);
    }

    // Report whether the monitor did what the scenario expects: the right
    // outcome, for the right reason, at the right time (a stop before the
    // fault was injected is a false alarm, not a success). A crash
    // scenario just goes silent; check the monitor's output for the stop.
    const bool stopped = stop_reason != StopReason::None;
    if (info->kind != ScenarioKind::Crash) {
        bool ok = stopped == info->expect_stop;
        if (ok && stopped && info->expected != StopReason::None &&
            stop_reason != info->expected) {
            ok = false;
        }
        const bool fault_at_cycle = info->kind != ScenarioKind::Nominal &&
                                    info->kind != ScenarioKind::Jitter &&
                                    info->kind != ScenarioKind::NoGoIntrusion;
        if (ok && stopped && fault_at_cycle && stop_step < kFaultCycle) {
            ok = false;
        }
        std::printf("%s: expected %s, got %s",
                    ok ? "AS EXPECTED" : "UNEXPECTED",
                    info->expect_stop ? "a safe stop" : "no stop",
                    stopped ? to_string(stop_reason) : "no stop");
        if (stopped) std::printf(" at step %u", stop_step);
        std::printf("\n");
    }
    return exit_code;
}
