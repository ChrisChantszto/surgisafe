#include "protocol.hpp"

#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <cstdint>

using namespace surgisafe;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
    // Default: freeze once at cycle 40. Use --normal for no fault.
    const bool inject_freeze = argc == 1;
    if (argc > 1 && (argc != 2 || std::string(argv[1]) != "--normal")) {
        std::fprintf(stderr, "Usage: %s [--normal]\n", argv[0]);
        return 2;
    }

    int connection = name_open(kServiceName, 0);
    if (connection == -1) {
        std::perror("name_open (start safety_monitor first)");
        return 1;
    }

    double position = 0.0;
    const double target = 8.0; // Do not deliberately violate the boundary.
    auto next_release = Clock::now();
    int exit_code = 0;

    for (std::uint32_t cycle = 0; cycle < 100; ++cycle) {
        next_release += 10ms;
        std::this_thread::sleep_until(next_release);

        if (inject_freeze && cycle == 40) {
            std::puts("Injecting 20 ms controller freeze...");
            std::fflush(stdout);
            std::this_thread::sleep_for(20ms);
        }

        const double command = std::clamp(target - position,
                                          -kMaxStep, kMaxStep);
        const MoveRequest request{kMoveRequest, 0, cycle,
                                  position + command};
        MoveReply reply{};
        const int result = MsgSend(connection, &request, sizeof(request),
                                   &reply, sizeof(reply));
        if (result == -1) {
            std::perror("MsgSend");
            exit_code = 1;
            break;
        }
        if (!reply.permitted) {
            std::printf("SAFE_STOP: movement rejected (reason %d: %s)\n",
                        reply.reason,
                        to_string(static_cast<StopReason>(reply.reason)));
            exit_code = 1;
            break;
        }
        position = reply.actual_position;
        std::printf("Cycle %u | Virtual position %.2f\n", cycle, position);
    }
    name_close(connection);
    return exit_code;
}
