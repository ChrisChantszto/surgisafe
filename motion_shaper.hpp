#pragma once

// Controller-side motion shaping: turns "go to this target" into a stream
// of small steps that respect the envelope's speed and acceleration limits,
// with a safety margin. This runs in the (untrusted) controller; the monitor
// still checks every step independently. A camera- or AI-driven planner
// would feed targets into this.

#include "safety_core.hpp"

#include <algorithm>
#include <cmath>

namespace surgisafe {

class MotionShaper {
public:
    // `margin` < 1 keeps commanded motion safely inside the monitor's limits.
    explicit MotionShaper(const Envelope& env, Point start,
                          double margin = 0.9)
        : max_speed_(env.max_step * margin),
          max_accel_(env.max_step_change * margin),
          position_(start) {}

    // Advances one cycle toward `target` and returns the next position.
    Point step_toward(Point target) {
        const double dx = target.x - position_.x;
        const double dy = target.y - position_.y;
        const double d = std::hypot(dx, dy);

        // Desired speed: cruise, but brake early enough to stop at the
        // target (braking uses half the accel budget as extra margin), and
        // never overshoot in a single step.
        double speed = std::min({max_speed_, d,
                                 std::sqrt(2.0 * 0.5 * max_accel_ * d)});
        Point desired{0.0, 0.0};
        if (d > 0.0) desired = Point{dx / d * speed, dy / d * speed};

        // Limit how fast the step itself can change (acceleration).
        double ax = desired.x - velocity_.x;
        double ay = desired.y - velocity_.y;
        const double a = std::hypot(ax, ay);
        if (a > max_accel_) {
            ax *= max_accel_ / a;
            ay *= max_accel_ / a;
        }
        velocity_ = Point{velocity_.x + ax, velocity_.y + ay};
        position_ = Point{position_.x + velocity_.x, position_.y + velocity_.y};
        return position_;
    }

    // Decelerates toward standstill without heading anywhere (used to hold
    // position when the target goes stale).
    Point brake() {
        const double s = std::hypot(velocity_.x, velocity_.y);
        const double k = s > max_accel_ ? (s - max_accel_) / s : 0.0;
        velocity_ = Point{velocity_.x * k, velocity_.y * k};
        position_ = Point{position_.x + velocity_.x, position_.y + velocity_.y};
        return position_;
    }

    bool arrived(Point target, double eps = 1e-6) const {
        return distance(position_, target) < eps &&
               std::hypot(velocity_.x, velocity_.y) < eps;
    }

    Point position() const { return position_; }
    Point velocity() const { return velocity_; }

private:
    double max_speed_;
    double max_accel_;
    Point position_;
    Point velocity_{0.0, 0.0};
};

} // namespace surgisafe
