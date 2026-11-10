#include "scenarios.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace surgisafe {

namespace {

constexpr std::uint32_t kMaxCycles = 2000;

const ScenarioInfo kScenarios[] = {
    {"nominal", "Trace the workspace edge, on time, within limits",
     ScenarioKind::Nominal, false, StopReason::None},
    // Timing scenarios scale with the timing profile and keep `margin`
    // either side of the deadline, so delivery latency (wake-up,
    // scheduling, IPC) cannot flip the expected outcome. With only 1 ms of
    // margin, runs on QNX in QEMU failed.
    {"jitter", "Send gaps vary between half a period and deadline - margin",
     ScenarioKind::Jitter, false, StopReason::None},
    {"freeze_inside", "Stall leaving the gap a margin inside the deadline",
     ScenarioKind::FreezeInside, false, StopReason::None},
    {"freeze_past", "Stall putting the gap a margin past the deadline",
     ScenarioKind::FreezePast, true, StopReason::WatchdogTimeout},
    {"freeze_long", "Stall of twice the deadline",
     ScenarioKind::FreezeLong, true, StopReason::WatchdogTimeout},
    {"crash", "Controller stops sending at cycle 50",
     ScenarioKind::Crash, true, StopReason::WatchdogTimeout},
    {"overspeed", "One step 1.5x longer than the speed limit",
     ScenarioKind::Overspeed, true, StopReason::SpeedLimit},
    {"reversal", "Instant reversal at cruise speed (too much acceleration)",
     ScenarioKind::Reversal, true, StopReason::AccelLimit},
    {"leave_workspace", "Step below the workspace floor",
     ScenarioKind::LeaveWorkspace, true, StopReason::OutOfWorkspace},
    {"nan_command", "Proposed position is NaN",
     ScenarioKind::NaNCommand, true, StopReason::OutOfWorkspace},
    {"replay", "Re-sends the previous cycle number",
     ScenarioKind::Replay, true, StopReason::InvalidSequence},
    {"skip_cycle", "Skips a cycle number",
     ScenarioKind::SkipCycle, true, StopReason::InvalidSequence},
    {"nogo_intrusion", "Heads straight for the no-go zone",
     ScenarioKind::NoGoIntrusion, true, StopReason::NoGoZone},
    {"rogue_ai", "From cycle 50, random and hostile proposals every cycle",
     ScenarioKind::RogueAI, true, StopReason::None},
    {nullptr, nullptr, ScenarioKind::Nominal, false, StopReason::None},
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
                               const TimingProfile& timing, std::uint64_t seed)
    : info_(info), env_(env), timing_(timing), rng_(seed),
      shaper_(env, {env.min_x, env.min_y}) {}

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

    const std::uint64_t period = timing_.period_ns;
    const std::uint64_t deadline = env_.watchdog_timeout_ns;
    const std::uint64_t margin = timing_.margin_ns;
    out.gap_ns = period;
    out.cycle = cycle_;
    if (info_.kind == ScenarioKind::Jitter) {
        std::uniform_int_distribution<std::uint64_t> gap(period / 2,
                                                          deadline - margin);
        out.gap_ns = gap(rng_);
    }

    const Point before = shaper_.position();
    const Point v = shaper_.velocity();
    Point target = shaper_.step_toward(nominal_target());
    if (shaper_.arrived(nominal_target(), 1e-3)) ++waypoint_;

    if (cycle_ == kFaultCycle) {
        switch (info_.kind) {
        // Freezes set the whole gap, relative to the deadline.
        case ScenarioKind::FreezeInside: out.gap_ns = deadline - margin; break;
        case ScenarioKind::FreezePast: out.gap_ns = deadline + margin; break;
        case ScenarioKind::FreezeLong: out.gap_ns = period + 2 * deadline; break;
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
