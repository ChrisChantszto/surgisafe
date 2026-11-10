#pragma once

// Portable safety decision logic for SurgiSafe-RT.
//
// This header has no QNX dependencies so the rules that decide whether a
// movement is permitted can be compiled and unit-tested on any host. The
// QNX-specific IPC and timer handling lives in safety_monitor.cpp and feeds
// events, stamped with a monotonic clock, into SafetyCore.
//
// The instrument tip moves in a 2D workspace. All positions are in
// workspace units (think centimetres); limits are per control cycle
// (nominally 10 ms). No heap allocation happens after construction.

#include <array>
#include <cstddef>
#include <cstdint>

namespace surgisafe {

struct Point {
    double x;
    double y;
};

// A circular region the instrument tip must never enter or cross, e.g. a
// critical structure. This is the "virtual fixture" idea from surgical
// robotics, in its simplest form.
struct NoGoZone {
    Point center;
    double radius;
};

constexpr std::size_t kMaxZones = 8;

// The safety envelope: everything the monitor enforces.
struct Envelope {
    double min_x = 0.0;
    double max_x = 8.0;
    double min_y = 0.0;
    double max_y = 8.0;
    double max_step = 0.1;          // Max distance per cycle (speed limit).
    double max_step_change = 0.02;  // Max change of step per cycle (accel).
    std::uint64_t watchdog_timeout_ns = 15ULL * 1000 * 1000;
    std::array<NoGoZone, kMaxZones> zones{};
    std::size_t zone_count = 0;
};

// Timing assumptions for a target platform. The heartbeat deadline is part
// of the safety envelope; the margin is how far timing tests keep their
// gaps from the deadline (it must exceed the worst delivery latency the
// platform shows, or tests flip from pass to fail by chance).
struct TimingProfile {
    const char* name;
    const char* description;
    std::uint64_t period_ns;            // Controller period.
    std::uint64_t deadline_ns;          // Watchdog deadline.
    std::uint64_t margin_ns;            // Test margin either side of deadline.
    std::uint64_t expected_latency_ns;  // Delivery latency the host campaign
                                        // models (must be < margin_ns).
};

// "hardware": 10 ms period, 15 ms deadline (the design target).
const TimingProfile& hardware_profile();
// All profiles, terminated by an entry with name == nullptr.
const TimingProfile* timing_profiles();
const TimingProfile* find_timing_profile(const char* name);

// The envelope used by the demo: an 8 x 8 workspace with one no-go zone,
// with the heartbeat deadline of the given timing profile.
Envelope default_envelope(const TimingProfile& profile = hardware_profile());

// Absorbs floating-point rounding when a request sits exactly on a limit.
constexpr double kTolerance = 1e-9;

// Values are sent on the wire in MoveReply::reason; do not renumber.
enum class StopReason : std::int32_t {
    None = 0,             // Movement accepted.
    WatchdogTimeout = 1,  // Heartbeat deadline missed.
    OutOfWorkspace = 2,   // Non-finite, or outside the workspace bounds.
    InvalidSequence = 3,  // Cycle number skipped, repeated, or out of order.
    MonitorFault = 4,     // The monitor itself could not enforce its checks.
    SpeedLimit = 5,       // Step longer than max_step.
    AccelLimit = 6,       // Step changed by more than max_step_change.
    NoGoZone = 7,         // Path enters or crosses a no-go zone.
};

const char* to_string(StopReason reason);

struct Decision {
    bool permitted;
    StopReason reason;  // StopReason::None when permitted.
    Point actual;       // Position after this decision.
};

// Pure geometry helpers, exposed for tests and tools.
double distance(Point a, Point b);
// Shortest distance from `c` to the segment a-b.
double distance_to_segment(Point c, Point a, Point b);
bool inside_bounds(const Envelope& env, Point p);
bool segment_hits_zone(const Envelope& env, Point a, Point b);

// Owns the virtual instrument position and the latched safe-stop state.
// Once stopped, the core never permits movement again and keeps reporting
// the reason that caused the first stop.
//
// Checks on every request, in priority order (the first failure is the
// reported reason): heartbeat deadline, workspace bounds, no-go zones,
// speed, acceleration, cycle sequence.
//
// Watchdog: every accepted request sets an *absolute* deadline of
// (receive time + watchdog_timeout_ns). The deadline is checked both when
// the timer fires (on_tick) and when the next request arrives (on_request),
// so a late request is refused even if the timer event has not been
// processed yet, and a stale or forged timer event cannot stop a healthy
// controller. All times are nanoseconds from one monotonic clock.
class SafetyCore {
public:
    explicit SafetyCore(const Envelope& envelope = default_envelope(),
                        Point start = {0.0, 0.0});

    Decision on_request(std::uint32_t cycle, Point proposed,
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
    Point position() const { return position_; }
    // The last accepted step (position change per cycle).
    Point velocity() const { return velocity_; }
    std::uint32_t expected_cycle() const { return expected_cycle_; }
    const Envelope& envelope() const { return env_; }

private:
    void latch(StopReason reason);
    bool deadline_missed(std::uint64_t now_ns) const;
    StopReason check(std::uint32_t cycle, Point proposed,
                     std::uint64_t now_ns) const;

    Envelope env_;
    Point position_;
    Point velocity_{0.0, 0.0};
    std::uint32_t expected_cycle_ = 0;
    std::uint64_t deadline_ns_ = 0;
    bool armed_ = false;
    StopReason reason_ = StopReason::None;
};

} // namespace surgisafe
