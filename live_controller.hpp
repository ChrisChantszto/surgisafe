#pragma once

// The live (interactive) controller: runs the 10 ms control loop toward the
// latest target from the dashboard (mouse, hand tracking, or autopilot),
// and supports operator-triggered faults for the demo.
//
// It is *untrusted* from the monitor's point of view: it tries to behave
// (clamps targets to the workspace, shapes motion, holds still when the
// target goes stale), but every step it proposes is still checked by
// SafetyCore. Portable, so the QNX gateway can reuse it unchanged.

#include "motion_shaper.hpp"
#include "safety_core.hpp"

#include <cstdint>
#include <random>

namespace surgisafe {

// Targets older than this make the controller brake and hold position.
constexpr std::uint64_t kTargetStaleNs = 250ULL * 1000 * 1000;

enum class ControllerMode {
    Running,   // Following targets normally.
    Frozen,    // Temporarily sends nothing (simulated stall).
    Killed,    // Sends nothing until reset (simulated crash).
    Rogue,     // Compromised: proposes random, unshaped moves.
};

const char* to_string(ControllerMode mode);

class LiveController {
public:
    LiveController(const Envelope& env, Point start, std::uint64_t seed = 1);

    // New target from the planner (mouse, hand tracking, autopilot).
    // Clamped to the workspace bounds; non-finite targets are ignored.
    void set_target(Point target, std::uint64_t now_ns);

    // Called once per control cycle. Returns false if the controller sends
    // nothing this cycle (frozen or killed).
    bool make_request(std::uint64_t now_ns, std::uint32_t& cycle,
                      Point& proposal);

    // Restarts after an operator reset: running, at `position`, at rest.
    void reset(Point position);

    void freeze_for(std::uint64_t duration_ns, std::uint64_t now_ns);
    void kill() { mode_ = ControllerMode::Killed; }
    void set_rogue(bool on);

    ControllerMode mode() const { return mode_; }
    bool holding() const { return holding_; }
    bool has_target() const { return has_target_; }
    Point target() const { return target_; }

private:
    Envelope env_;
    MotionShaper shaper_;
    std::mt19937_64 rng_;
    ControllerMode mode_ = ControllerMode::Running;
    std::uint64_t frozen_until_ns_ = 0;
    std::uint64_t target_time_ns_ = 0;
    Point target_;
    bool has_target_ = false;
    bool holding_ = true;
    std::uint32_t cycle_ = 0;
};

} // namespace surgisafe
