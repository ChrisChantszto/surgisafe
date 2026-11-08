// QNX controller: replays one fault-injection scenario against the running
// safety_monitor, using the same scenario streams as the host campaign
// (scenarios.cpp), so simulated and on-target results are comparable.
//
//   ./controller                     # freeze_20ms (the classic demo)
//   ./controller --normal            # nominal path, no fault
//   ./controller --scenario reversal
//   ./controller --list

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
                 "Usage: %s [--normal | --scenario NAME | --list] [--seed S]\n",
                 argv0);
}

} // namespace

int main(int argc, char** argv) {
    const char* scenario_name = "freeze_20ms";
    std::uint64_t seed = 1;
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
    std::fflush(stdout);

    ScenarioStream stream(*info, default_envelope(), seed);
    auto next_release = Clock::now();
    int exit_code = 0;
    ScenarioStep step{};

    while (stream.next(step)) {
        // Absolute release times keep the period from drifting; a freeze
        // shows up as one long gap.
        next_release += std::chrono::nanoseconds(step.gap_ns);
        std::this_thread::sleep_until(next_release);

        const MoveRequest request{kMoveRequest, 0, step.cycle, step.target.x,
                                  step.target.y};
        MoveReply reply{};
        if (MsgSend(connection, &request, sizeof(request), &reply,
                    sizeof(reply)) == -1) {
            std::perror("MsgSend");
            exit_code = 1;
            break;
        }
        if (!reply.permitted) {
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

    // Report whether the monitor did what the scenario expects. (A crash
    // scenario just goes silent; check the monitor's output for the stop.)
    const bool stopped = exit_code != 0;
    if (info->kind != ScenarioKind::Crash) {
        std::printf("%s: expected %s, got %s\n",
                    stopped == info->expect_stop ? "AS EXPECTED" : "UNEXPECTED",
                    info->expect_stop ? "a safe stop" : "no stop",
                    stopped ? "a safe stop" : "no stop");
    }
    return exit_code;
}
