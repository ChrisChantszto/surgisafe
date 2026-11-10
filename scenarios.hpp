#pragma once

// Fault-injection scenarios shared by the host campaign tool and the QNX
// controller, so the same command streams run in simulation and on target.
//
// A scenario is a deterministic (given a seed) stream of requests: how long
// to wait before sending, which cycle number to send, and which position to
// propose. Nominal motion traces the edge of the workspace with the motion
// shaper; each fault scenario injects one specific failure.

#include "motion_shaper.hpp"
#include "safety_core.hpp"

#include <cstdint>
#include <random>

namespace surgisafe {

// Cycle at which most faults are injected (cruising along the bottom edge).
constexpr std::uint32_t kFaultCycle = 50;

enum class ScenarioKind {
    Nominal,
    Jitter,
    FreezeInside,  // Stall that keeps the gap `margin` below the deadline.
    FreezePast,    // Stall that puts the gap `margin` past the deadline.
    FreezeLong,    // Stall of twice the deadline.
    Crash,
    Overspeed,
    Reversal,
    LeaveWorkspace,
    NaNCommand,
    Replay,
    SkipCycle,
    NoGoIntrusion,
    RogueAI,
};

struct ScenarioInfo {
    const char* name;
    const char* description;
    ScenarioKind kind;
    bool expect_stop;
    // Expected reason when expect_stop; StopReason::None means "any reason".
    StopReason expected;
};

// All scenarios, terminated by an entry with name == nullptr.
const ScenarioInfo* scenario_table();
const ScenarioInfo* find_scenario(const char* name);

struct ScenarioStep {
    std::uint64_t gap_ns;  // Wait this long after the previous send.
    std::uint32_t cycle;
    Point target;
};

class ScenarioStream {
public:
    // Timing scenarios (jitter, freezes) scale with `timing`, so they keep
    // the same margin to the deadline on every platform.
    ScenarioStream(const ScenarioInfo& info, const Envelope& env,
                   const TimingProfile& timing, std::uint64_t seed);

    // Produces the next request; returns false when the controller stops
    // sending (end of path, or a crash).
    bool next(ScenarioStep& out);

    const ScenarioInfo& info() const { return info_; }

private:
    Point nominal_target();

    ScenarioInfo info_;
    Envelope env_;
    TimingProfile timing_;
    std::mt19937_64 rng_;
    MotionShaper shaper_;
    std::uint32_t cycle_ = 0;
    std::size_t waypoint_ = 0;
};

} // namespace surgisafe
