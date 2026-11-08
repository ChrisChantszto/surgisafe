#include "safety_core.hpp"

#include <cmath>

namespace surgisafe {

const char* to_string(StopReason reason) {
    switch (reason) {
    case StopReason::None: return "accepted";
    case StopReason::WatchdogTimeout: return "watchdog timeout";
    case StopReason::InvalidMotion: return "invalid motion";
    case StopReason::InvalidSequence: return "invalid sequence";
    case StopReason::MonitorFault: return "monitor fault";
    }
    return "unknown";
}

void SafetyCore::latch(StopReason reason) {
    if (!stopped()) {
        reason_ = reason;
    }
}

bool SafetyCore::deadline_missed(std::uint64_t now_ns) const {
    return armed_ && now_ns > deadline_ns_;
}

Decision SafetyCore::on_request(std::uint32_t cycle, double proposed,
                                std::uint64_t now_ns) {
    if (!stopped()) {
        const bool motion_ok = std::isfinite(proposed) &&
                               proposed >= 0.0 && proposed <= kMaxPosition &&
                               std::fabs(proposed - position_) <=
                                   kMaxStep + kStepTolerance;
        if (deadline_missed(now_ns)) {
            // Arrived late: the timer may not have been serviced yet, but
            // the heartbeat was still missed.
            latch(StopReason::WatchdogTimeout);
        } else if (!motion_ok) {
            latch(StopReason::InvalidMotion);
        } else if (cycle != expected_cycle_) {
            latch(StopReason::InvalidSequence);
        }
    }
    armed_ = true;

    if (stopped()) {
        return Decision{false, reason_, position_};
    }

    position_ = proposed;  // Only the monitor commits the virtual movement.
    ++expected_cycle_;
    deadline_ns_ = now_ns + kWatchdogTimeoutNs;
    return Decision{true, StopReason::None, position_};
}

void SafetyCore::on_tick(std::uint64_t now_ns) {
    if (deadline_missed(now_ns)) {
        latch(StopReason::WatchdogTimeout);
    }
}

void SafetyCore::on_fault() {
    latch(StopReason::MonitorFault);
}

} // namespace surgisafe
