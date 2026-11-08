#pragma once

// Portable safety decision logic for SurgiSafe-RT.
//
// This header has no QNX dependencies so the rules that decide whether a
// movement is permitted can be compiled and unit-tested on any host. The
// QNX-specific IPC and timer handling lives in safety_monitor.cpp and feeds
// events, stamped with a monotonic clock, into SafetyCore.

#include <cstdint>

namespace surgisafe {

constexpr double kMaxPosition = 8.0;
constexpr double kMaxStep = 0.1;
// Absorbs floating-point rounding in (proposed - current) for a maximal step.
constexpr double kStepTolerance = 1e-9;
// Maximum time allowed between accepted heartbeats (requests).
constexpr std::uint64_t kWatchdogTimeoutNs = 15ULL * 1000 * 1000;

// Values are sent on the wire in MoveReply::reason; do not renumber.
enum class StopReason : std::int32_t {
    None = 0,             // Movement accepted.
    WatchdogTimeout = 1,  // Heartbeat deadline missed.
    InvalidMotion = 2,    // Non-finite, out of range, or step too large.
    InvalidSequence = 3,  // Cycle number skipped, repeated, or out of order.
    MonitorFault = 4,     // The monitor itself could not enforce its checks.
};

const char* to_string(StopReason reason);

struct Decision {
    bool permitted;
    StopReason reason;       // StopReason::None when permitted.
    double actual_position;  // Position after this decision.
};

// Owns the virtual instrument position and the latched safe-stop state.
// Once stopped, the core never permits movement again and keeps reporting
// the reason that caused the first stop.
//
// Watchdog: every accepted request sets an *absolute* deadline of
// (receive time + kWatchdogTimeoutNs). The deadline is checked both when the
// timer fires (on_tick) and when the next request arrives (on_request), so a
// late request is refused even if the timer event has not been processed yet,
// and a stale or spoofed timer event cannot stop a healthy controller.
// All times are nanoseconds from one monotonic clock.
class SafetyCore {
public:
    Decision on_request(std::uint32_t cycle, double proposed_position,
                        std::uint64_t now_ns);

    // The watchdog timer fired (or any other reason to re-check the
    // deadline). Latches WatchdogTimeout only if the deadline has passed.
    void on_tick(std::uint64_t now_ns);

    // The monitor could not enforce its checks (e.g. failed to arm a timer).
    void on_fault();

    bool armed() const { return armed_; }
    bool stopped() const { return reason_ != StopReason::None; }
    // True while the monitor should keep a watchdog timer armed.
    bool watchdog_should_run() const { return armed_ && !stopped(); }
    // Absolute deadline for the next request; meaningful only while
    // watchdog_should_run() is true.
    std::uint64_t deadline_ns() const { return deadline_ns_; }
    StopReason stop_reason() const { return reason_; }
    double position() const { return position_; }
    std::uint32_t expected_cycle() const { return expected_cycle_; }

private:
    void latch(StopReason reason);
    bool deadline_missed(std::uint64_t now_ns) const;

    double position_ = 0.0;
    std::uint32_t expected_cycle_ = 0;
    std::uint64_t deadline_ns_ = 0;
    bool armed_ = false;
    StopReason reason_ = StopReason::None;
};

} // namespace surgisafe
