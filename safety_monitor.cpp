#include "protocol.hpp"

#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>

using namespace surgisafe;

// Buffer accommodates both a QNX pulse and our application request.
union ReceiveBuffer {
    std::uint16_t type;
    struct _pulse pulse;
    MoveRequest request;
};

int main() {
    name_attach_t* service = name_attach(nullptr, kServiceName, 0);
    if (!service) {
        std::perror("name_attach");
        return 1;
    }

    double position = 0.0;
    std::uint32_t expected_cycle = 0;
    bool armed = false;
    bool stopped = false;
    std::puts("Safety monitor ready; awaiting controller");
    std::fflush(stdout);

    while (true) {
        ReceiveBuffer message{};

        // This is a simple receive-inactivity watchdog. The timeout begins
        // when MsgReceive is entered, not at the previous cycle's release.
        // A production supervisor needs an absolute heartbeat deadline,
        // scheduler analysis, and a separately enforced actuator safe state.
        if (armed && !stopped) {
            const std::uint64_t timeout_ns = 15ULL * 1000 * 1000;
            if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE,
                             nullptr, &timeout_ns, nullptr) == -1) {
                std::perror("TimerTimeout");
                stopped = true;
            }
        }

        const int rcvid = MsgReceive(service->chid, &message,
                                    sizeof(message), nullptr);
        if (rcvid == -1) {
            if (errno == ETIMEDOUT) {
                if (!stopped) {
                    std::puts("WATCHDOG TIMEOUT: SAFE_STOP");
                    std::fflush(stdout);
                }
                stopped = true;
                continue;
            }
            if (errno == EINTR) continue;
            std::perror("MsgReceive");
            break;
        }
        if (rcvid == 0) {
            if (message.pulse.code == _PULSE_CODE_DISCONNECT) {
                ConnectDetach(message.pulse.scoid);
            }
            continue;
        }
        if (message.type != kMoveRequest) {
            MsgError(rcvid, ENOSYS);
            continue;
        }

        armed = true;
        const MoveRequest& req = message.request;
        const double next = req.proposed_position;
        const bool valid = std::isfinite(next) &&
                           next >= 0.0 && next <= kMaxPosition &&
                           std::fabs(next - position) <= kMaxStep + 1e-9 &&
                           req.cycle == expected_cycle;
        if (!stopped && !valid) {
            stopped = true;
            std::puts("INVALID MOTION/SEQUENCE: SAFE_STOP");
        }

        MoveReply reply{0, stopped ? (valid ? 1 : 2) : 0, position};
        if (!stopped) {
            position = next;  // Only monitor commits the virtual movement.
            ++expected_cycle;
            reply.permitted = 1;
            reply.actual_position = position;
        }
        if (MsgReply(rcvid, EOK, &reply, sizeof(reply)) == -1) {
            std::perror("MsgReply");
        }
        std::fflush(stdout);
    }

    name_detach(service, 0);
    return 1;
}
