#include "protocol.hpp"
#include "safety_core.hpp"

#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <ctime>

using namespace surgisafe;

// Buffer accommodates both a QNX pulse and our application request. Every
// QNX message (including _IO_CONNECT) starts with a 16-bit type field.
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

    SafetyCore core;
    std::puts("Safety monitor ready; awaiting controller");
    std::fflush(stdout);

    while (true) {
        ReceiveBuffer message{};
        struct _msg_info info{};

        // This is a simple receive-inactivity watchdog. The timeout begins
        // when MsgReceive is entered, not at the previous cycle's release.
        // A production supervisor needs an absolute heartbeat deadline,
        // scheduler analysis, and a separately enforced actuator safe state.
        if (core.watchdog_should_run()) {
            const std::uint64_t timeout_ns = kWatchdogTimeoutNs;
            if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE,
                             nullptr, &timeout_ns, nullptr) == -1) {
                std::perror("TimerTimeout");
                core.on_fault();
                std::puts("MONITOR FAULT: SAFE_STOP");
                std::fflush(stdout);
            }
        }

        const int rcvid = MsgReceive(service->chid, &message,
                                     sizeof(message), &info);
        if (rcvid == -1) {
            if (errno == ETIMEDOUT) {
                const bool was_stopped = core.stopped();
                core.on_watchdog_timeout();
                if (!was_stopped && core.stopped()) {
                    std::puts("WATCHDOG TIMEOUT: SAFE_STOP");
                    std::fflush(stdout);
                }
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

        // name_open() sends an _IO_CONNECT message first; it must get EOK
        // or the client's name_open() fails.
        if (message.type == _IO_CONNECT) {
            MsgReply(rcvid, EOK, nullptr, 0);
            continue;
        }
        if (message.type != kMoveRequest) {
            MsgError(rcvid, ENOSYS);
            continue;
        }
        // Reject truncated or oversized requests rather than reading a
        // partially zero-filled struct.
        if (info.srcmsglen != sizeof(MoveRequest)) {
            MsgError(rcvid, EBADMSG);
            continue;
        }

        const bool was_stopped = core.stopped();
        const MoveRequest& req = message.request;
        const Decision decision =
            core.on_request(req.cycle, req.proposed_position);
        if (!was_stopped && core.stopped()) {
            std::printf("%s: SAFE_STOP\n", to_string(core.stop_reason()));
        }

        const MoveReply reply{decision.permitted ? 1 : 0,
                              static_cast<std::int32_t>(decision.reason),
                              decision.actual_position};
        if (MsgReply(rcvid, EOK, &reply, sizeof(reply)) == -1) {
            std::perror("MsgReply");
        }
        std::fflush(stdout);
    }

    name_detach(service, 0);
    return 1;
}
