#include "live_controller.hpp"

#include <algorithm>
#include <cmath>

namespace surgisafe {

const char* to_string(ControllerMode mode) {
    switch (mode) {
    case ControllerMode::Running: return "running";
    case ControllerMode::Frozen: return "frozen";
    case ControllerMode::Killed: return "killed";
    case ControllerMode::Rogue: return "rogue";
    }
    return "unknown";
}

LiveController::LiveController(const Envelope& env, Point start,
                               std::uint64_t seed)
    : env_(env), shaper_(env, start), rng_(seed), target_(start) {}

void LiveController::set_target(Point t, std::uint64_t now_ns) {
    if (!std::isfinite(t.x) || !std::isfinite(t.y)) return;
    target_ = Point{std::clamp(t.x, env_.min_x, env_.max_x),
                    std::clamp(t.y, env_.min_y, env_.max_y)};
    target_time_ns_ = now_ns;
    has_target_ = true;
}

void LiveController::reset(Point position) {
    shaper_ = MotionShaper(env_, position);
    mode_ = ControllerMode::Running;
    target_ = position;
    has_target_ = false;
    holding_ = true;
    cycle_ = 0;
}

void LiveController::freeze_for(std::uint64_t duration_ns,
                                std::uint64_t now_ns) {
    if (mode_ == ControllerMode::Running) {
        mode_ = ControllerMode::Frozen;
        frozen_until_ns_ = now_ns + duration_ns;
    }
}

void LiveController::set_rogue(bool on) {
    if (on && mode_ == ControllerMode::Running) mode_ = ControllerMode::Rogue;
    if (!on && mode_ == ControllerMode::Rogue) mode_ = ControllerMode::Running;
}

bool LiveController::make_request(std::uint64_t now_ns, std::uint32_t& cycle,
                                  Point& proposal) {
    if (mode_ == ControllerMode::Frozen) {
        if (now_ns < frozen_until_ns_) return false;
        mode_ = ControllerMode::Running;
    }
    if (mode_ == ControllerMode::Killed) return false;

    if (mode_ == ControllerMode::Rogue) {
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const Point p = shaper_.position();
        const double angle = unit(rng_) * 6.283185307179586;
        const double len = unit(rng_) * 3.0 * env_.max_step;
        proposal = Point{p.x + std::cos(angle) * len,
                         p.y + std::sin(angle) * len};
    } else {
        holding_ = !has_target_ || now_ns - target_time_ns_ > kTargetStaleNs;
        proposal = holding_ ? shaper_.brake() : shaper_.step_toward(target_);
    }
    cycle = cycle_++;
    return true;
}

} // namespace surgisafe
