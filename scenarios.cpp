#include "scenarios.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace surgisafe {

namespace {

constexpr std::uint64_t kMs = 1000ULL * 1000;
constexpr std::uint32_t kMaxCycles = 2000;

const ScenarioInfo kScenarios[] = {
    {"nominal", "Trace the workspace edge, on time, within limits",
     ScenarioKind::Nominal, 0, false, StopReason::None},
    {"jitter", "Nominal path with send gaps varying between 5 and 14 ms",
     ScenarioKind::Jitter, 0, false, StopReason::None},
    {"freeze_4ms", "Controller stalls 4 ms (gap 14 ms, inside the deadline)",
     ScenarioKind::Freeze, 4 * kMs, false, StopReason::None},
    {"freeze_6ms", "Controller stalls 6 ms (gap 16 ms, misses the deadline)",
     ScenarioKind::Freeze, 6 * kMs, true, StopReason::WatchdogTimeout},
    {"freeze_20ms", "Controller stalls 20 ms",
     ScenarioKind::Freeze, 20 * kMs, true, StopReason::WatchdogTimeout},
    {"crash", "Controller stops sending at cycle 50",
     ScenarioKind::Crash, 0, true, StopReason::WatchdogTimeout},
    {"overspeed", "One step 1.5x longer than the speed limit",
     ScenarioKind::Overspeed, 0, true, StopReason::SpeedLimit},
    {"reversal", "Instant reversal at cruise speed (too much acceleration)",
     ScenarioKind::Reversal, 0, true, StopReason::AccelLimit},
    {"leave_workspace", "Step below the workspace floor",
     ScenarioKind::LeaveWorkspace, 0, true, StopReason::OutOfWorkspace},
    {"nan_command", "Proposed position is NaN",
     ScenarioKind::NaNCommand, 0, true, StopReason::OutOfWorkspace},
    {"replay", "Re-sends the previous cycle number",
     ScenarioKind::Replay, 0, true, StopReason::InvalidSequence},
    {"skip_cycle", "Skips a cycle number",
     ScenarioKind::SkipCycle, 0, true, StopReason::InvalidSequence},
    {"nogo_intrusion", "Heads straight for the no-go zone",
     ScenarioKind::NoGoIntrusion, 0, true, StopReason::NoGoZone},
    {"rogue_ai", "From cycle 50, random and hostile proposals every cycle",
     ScenarioKind::RogueAI, 0, true, StopReason::None},
    {nullptr, nullptr, ScenarioKind::Nominal, 0, false, StopReason::None},
};

} // namespace

const ScenarioInfo* scenario_table() { return kScenarios; }

const ScenarioInfo* find_scenario(const char* name) {
    for (const ScenarioInfo* s = kScenarios; s->name; ++s) {
        if (std::strcmp(s->name, name) == 0) return s;
    }
    return nullptr;
}

ScenarioStream::ScenarioStream(const ScenarioInfo& info, const Envelope& env,
                               std::uint64_t seed)
    : info_(info), env_(env), rng_(seed), shaper_(env, {env.min_x, env.min_y}) {}

Point ScenarioStream::nominal_target() {
    if (info_.kind == ScenarioKind::NoGoIntrusion && env_.zone_count > 0) {
        return env_.zones[0].center;
    }
    const Point corners[] = {{env_.max_x, env_.min_y},
                             {env_.max_x, env_.max_y},
                             {env_.min_x, env_.max_y},
                             {env_.min_x, env_.min_y}};
    return corners[waypoint_ % 4];
}

bool ScenarioStream::next(ScenarioStep& out) {
    if (cycle_ >= kMaxCycles || waypoint_ >= 4) return false;
    if (info_.kind == ScenarioKind::Crash && cycle_ == kFaultCycle) {
        return false;
    }

    out.gap_ns = kNominalPeriodNs;
    out.cycle = cycle_;
    if (info_.kind == ScenarioKind::Jitter) {
        std::uniform_int_distribution<std::uint64_t> gap(5 * kMs, 14 * kMs);
        out.gap_ns = gap(rng_);
    }

    const Point before = shaper_.position();
    const Point v = shaper_.velocity();
    Point target = shaper_.step_toward(nominal_target());
    if (shaper_.arrived(nominal_target(), 1e-3)) ++waypoint_;

    if (cycle_ == kFaultCycle) {
        switch (info_.kind) {
        case ScenarioKind::Freeze: out.gap_ns += info_.freeze_ns; break;
        case ScenarioKind::Overspeed: {
            const double s = std::hypot(v.x, v.y);
            const double k = s > 0.0 ? 1.5 * env_.max_step / s : 0.0;
            target = Point{before.x + v.x * k, before.y + v.y * k};
            break;
        }
        case ScenarioKind::Reversal:
            target = Point{before.x - v.x, before.y - v.y};
            break;
        case ScenarioKind::LeaveWorkspace:
            target = Point{before.x + v.x, env_.min_y - 0.01};
            break;
        case ScenarioKind::NaNCommand:
            target.x = std::numeric_limits<double>::quiet_NaN();
            break;
        case ScenarioKind::Replay: out.cycle = cycle_ - 1; break;
        case ScenarioKind::SkipCycle: out.cycle = cycle_ + 1; break;
        default: break;
        }
    }

    if (info_.kind == ScenarioKind::RogueAI && cycle_ >= kFaultCycle) {
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const double r = unit(rng_);
        if (r < 0.05) {
            target = Point{std::numeric_limits<double>::quiet_NaN(), before.y};
        } else if (r < 0.10) {
            target = Point{unit(rng_) * 20.0 - 6.0, unit(rng_) * 20.0 - 6.0};
        } else {
            const double angle = unit(rng_) * 6.283185307179586;
            const double len = unit(rng_) * 3.0 * env_.max_step;
            target = Point{before.x + std::cos(angle) * len,
                           before.y + std::sin(angle) * len};
        }
    }

    out.target = target;
    ++cycle_;
    return true;
}

} // namespace surgisafe
