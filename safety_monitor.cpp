#include "protocol.hpp"
#include "safety_core.hpp"

#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <ctime>

using namespace surgisafe;

namespace {

// Pulse code our watchdog timer delivers to the monitor's own channel.
constexpr int kWatchdogPulseCode = _PULSE_CODE_MINAVAIL;

// Buffer accommodates both a QNX pulse and our application request. Every
// QNX message (including _IO_CONNECT) starts with a 16-bit type field.
union ReceiveBuffer {
    std::uint16_t type;
    struct _pulse pulse;
    MoveRequest request;
};

std::uint64_t monotonic_now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

timespec to_timespec(std::uint64_t ns) {
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
    ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
    return ts;
}

// Watchdog built from a QNX timer that delivers a pulse to our own channel.
// It is armed with an absolute CLOCK_MONOTONIC deadline, so unrelated
// messages or pulses cannot extend the window.
class WatchdogTimer {
public:
    bool create(int chid) {
        // A side-channel connection so the kernel can deliver the pulse to
        // the channel name_attach() created.
        coid_ = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
        if (coid_ == -1) {
            std::perror("ConnectAttach");
            return false;
        }
        struct sigevent event{};
        SIGEV_PULSE_INIT(&event, coid_, SIGEV_PULSE_PRIO_INHERIT,
                         kWatchdogPulseCode, 0);
        if (timer_create(CLOCK_MONOTONIC, &event, &timer_) == -1) {
            std::perror("timer_create");
            return false;
        }
        return true;
    }

    bool arm_at(std::uint64_t deadline_ns) {
        itimerspec spec{};
        spec.it_value = to_timespec(deadline_ns);  // One-shot, no interval.
        if (timer_settime(timer_, TIMER_ABSTIME, &spec, nullptr) == -1) {
            std::perror("timer_settime");
            return false;
        }
        return true;
    }

    void disarm() {
        const itimerspec spec{};  // Zero it_value disarms the timer.
        timer_settime(timer_, 0, &spec, nullptr);
    }

private:
    int coid_ = -1;
    timer_t timer_{};
};

} // namespace

int main() {
    name_attach_t* service = name_attach(nullptr, kServiceName, 0);
    if (!service) {
        std::perror("name_attach");
        return 1;
    }

    // Refuse to run at all if the watchdog cannot be created.
    WatchdogTimer watchdog;
    if (!watchdog.create(service->chid)) {
        name_detach(service, 0);
        return 1;
    }

    SafetyCore core;
    std::puts("Safety monitor ready; awaiting controller");
    std::fflush(stdout);

    while (true) {
        ReceiveBuffer message{};
        struct _msg_info info{};

        const int rcvid = MsgReceive(service->chid, &message,
                                     sizeof(message), &info);
        const std::uint64_t now = monotonic_now_ns();
        if (rcvid == -1) {
            if (errno == EINTR) continue;
            std::perror("MsgReceive");
            break;
        }

        if (rcvid == 0) {
            if (message.pulse.code == kWatchdogPulseCode) {
                // The core re-checks the deadline itself, so a stale pulse
                // (timer re-armed after it was queued) or a pulse forged by
                // another process cannot stop a healthy controller.
                const bool was_stopped = core.stopped();
                core.on_tick(now);
                if (!was_stopped && core.stopped()) {
                    std::puts("WATCHDOG TIMEOUT: SAFE_STOP");
                    std::fflush(stdout);
                }
            } else if (message.pulse.code == _PULSE_CODE_DISCONNECT) {
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
            core.on_request(req.cycle, Point{req.x, req.y}, now);

        // Re-arm before replying, so the controller's next cycle is always
        // covered by the new deadline.
        if (core.watchdog_should_run() && !watchdog.arm_at(core.deadline_ns())) {
            core.on_fault();
        }
        if (!was_stopped && core.stopped()) {
            watchdog.disarm();
            std::printf("%s: SAFE_STOP\n", to_string(core.stop_reason()));
        }

        // If arming failed just now, the move above was already committed
        // to the virtual position but the controller is told to stop.
        const bool permitted = decision.permitted && !core.stopped();
        const MoveReply reply{permitted ? 1 : 0,
                              static_cast<std::int32_t>(core.stop_reason()),
                              core.position().x, core.position().y};
        if (MsgReply(rcvid, EOK, &reply, sizeof(reply)) == -1) {
            std::perror("MsgReply");
        }
        std::fflush(stdout);
    }

    name_detach(service, 0);
    return 1;
}
