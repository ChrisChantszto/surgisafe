#include "safety_core.hpp"

#include <algorithm>
#include <cmath>

namespace surgisafe {

Envelope default_envelope() {
    Envelope env;
    env.zones[0] = NoGoZone{{5.0, 5.0}, 1.0};
    env.zone_count = 1;
    return env;
}

const char* to_string(StopReason reason) {
    switch (reason) {
    case StopReason::None: return "accepted";
    case StopReason::WatchdogTimeout: return "watchdog timeout";
    case StopReason::OutOfWorkspace: return "out of workspace";
    case StopReason::InvalidSequence: return "invalid sequence";
    case StopReason::MonitorFault: return "monitor fault";
    case StopReason::SpeedLimit: return "speed limit";
    case StopReason::AccelLimit: return "acceleration limit";
    case StopReason::NoGoZone: return "no-go zone";
    }
    return "unknown";
}

double distance(Point a, Point b) {
    return std::hypot(b.x - a.x, b.y - a.y);
}

double distance_to_segment(Point c, Point a, Point b) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    if (len2 == 0.0) return distance(c, a);
    const double t = std::clamp(((c.x - a.x) * dx + (c.y - a.y) * dy) / len2,
                                0.0, 1.0);
    return distance(c, Point{a.x + t * dx, a.y + t * dy});
}

bool inside_bounds(const Envelope& env, Point p) {
    return std::isfinite(p.x) && std::isfinite(p.y) &&
           p.x >= env.min_x && p.x <= env.max_x &&
           p.y >= env.min_y && p.y <= env.max_y;
}

bool segment_hits_zone(const Envelope& env, Point a, Point b) {
    for (std::size_t i = 0; i < env.zone_count; ++i) {
        const NoGoZone& z = env.zones[i];
        // Touching the boundary is allowed; entering is not.
        if (distance_to_segment(z.center, a, b) < z.radius - kTolerance) {
            return true;
        }
    }
    return false;
}

SafetyCore::SafetyCore(const Envelope& envelope, Point start)
    : env_(envelope), position_(start) {
    // A start position outside the envelope is a configuration error:
    // refuse to ever move.
    if (!inside_bounds(env_, start) || segment_hits_zone(env_, start, start)) {
        reason_ = StopReason::MonitorFault;
    }
}

void SafetyCore::latch(StopReason reason) {
    if (!stopped()) {
        reason_ = reason;
    }
}

bool SafetyCore::deadline_missed(std::uint64_t now_ns) const {
    return armed_ && now_ns > deadline_ns_;
}

StopReason SafetyCore::check(std::uint32_t cycle, Point p,
                             std::uint64_t now_ns) const {
    if (deadline_missed(now_ns)) {
        // Arrived late: the timer may not have been serviced yet, but the
        // heartbeat was still missed.
        return StopReason::WatchdogTimeout;
    }
    if (!inside_bounds(env_, p)) return StopReason::OutOfWorkspace;
    if (segment_hits_zone(env_, position_, p)) return StopReason::NoGoZone;

    const Point step{p.x - position_.x, p.y - position_.y};
    if (std::hypot(step.x, step.y) > env_.max_step + kTolerance) {
        return StopReason::SpeedLimit;
    }
    if (std::hypot(step.x - velocity_.x, step.y - velocity_.y) >
        env_.max_step_change + kTolerance) {
        return StopReason::AccelLimit;
    }
    if (cycle != expected_cycle_) return StopReason::InvalidSequence;
    return StopReason::None;
}

Decision SafetyCore::on_request(std::uint32_t cycle, Point proposed,
                                std::uint64_t now_ns) {
    if (!stopped()) {
        latch(check(cycle, proposed, now_ns));
    }
    armed_ = true;

    if (stopped()) {
        return Decision{false, reason_, position_};
    }

    // Only the monitor commits the virtual movement.
    velocity_ = Point{proposed.x - position_.x, proposed.y - position_.y};
    position_ = proposed;
    ++expected_cycle_;
    deadline_ns_ = now_ns + env_.watchdog_timeout_ns;
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
