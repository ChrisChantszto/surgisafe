# Hazards, safety requirements and verification

SurgiSafe-RT is an **educational simulation**. This table is written in the
style of a medical-device risk analysis (inspired by ISO 14971 and
IEC 62304) to show how each safety rule traces to a hazard and to the
evidence that it works. It is **not** a certified analysis, and the project
must not be used with a real instrument or patient.

The instrument tip moves in a 2D workspace (default 8 × 8 units, one
circular no-go zone of radius 1 at (5, 5)). Limits are per 10 ms control
cycle. All limits live in one `Envelope` struct ([safety_core.hpp](../safety_core.hpp)).

## Hazards

| ID | Hazard | Possible causes |
|----|--------|-----------------|
| H-1 | Instrument moves too fast | Controller bug, corrupted message, AI planner output outside expectations |
| H-2 | Instrument leaves its permitted workspace | Accumulated error, NaN/infinity in the command, deliberate bad command |
| H-3 | A stale, duplicated or out-of-order command is executed | Retransmission, replay, controller restart without reset |
| H-4 | The instrument keeps executing old commands while the controller is stalled or dead | Controller freeze, CPU starvation, crash, priority inversion |
| H-5 | Motion silently resumes after a fault | Automatic recovery, a later valid message clearing the stop |
| H-6 | The supervisor cannot enforce its checks | Timer creation or arming fails, bad configuration |
| H-7 | Violent jerk or sudden reversal | Planner switching targets abruptly, noisy tracking input |
| H-8 | Instrument enters or cuts across a critical structure | Planner ignores anatomy, tracking error, deliberate bad command |
| H-9 | Instrument presses too hard on tissue | Planner steers into tissue, tracking error, faulty or disconnected force sensor |

## Safety requirements

| ID | Requirement | Mitigates | Implemented in |
|----|-------------|-----------|----------------|
| SR-1 | A move is committed only if the step length (Euclidean) is ≤ `max_step` (0.1) | H-1 | `SafetyCore::check` ([safety_core.cpp](../safety_core.cpp)) |
| SR-2 | A move is committed only if the target is finite and inside the workspace bounds | H-2 | `inside_bounds`, `SafetyCore::check` |
| SR-3 | A move is committed only if its cycle number is exactly the next expected one | H-3 | `SafetyCore::check` |
| SR-4 | Any violation latches a safe stop; no later message can clear it, and the first cause is always reported | H-5 | `SafetyCore::latch` |
| SR-5 | If no accepted request arrives within the heartbeat deadline of the previous accepted one (absolute deadline; 15 ms on the `hardware` profile, 50 ms on the `qemu` profile), the monitor latches a safe stop, whether or not the timer event has been processed yet | H-4 | `SafetyCore::on_tick`, `check`; QNX timer pulse in `WatchdogTimer` ([safety_monitor.cpp](../safety_monitor.cpp)) |
| SR-6 | If the monitor cannot create its watchdog it refuses to start; if it cannot arm it, or starts outside the envelope, it latches a safe stop | H-6 | `main` and `WatchdogTimer` in safety_monitor.cpp; `SafetyCore` constructor and `on_fault` |
| SR-7 | Only the monitor changes the instrument position; the controller only proposes | H-1–H-8 | Architecture: position lives only in `SafetyCore` |
| SR-8 | A move is committed only if the step differs from the previous step by ≤ `max_step_change` (0.02) | H-7 | `SafetyCore::check` |
| SR-9 | A move is committed only if the straight path from the current position to the target does not enter any no-go zone | H-8 | `segment_hits_zone`, `SafetyCore::check` |
| SR-10 | If the measured contact force exceeds `max_contact_force_n` (5 N), or the reading is not a valid number, the monitor latches a safe stop; the arm then holds where the tip physically is and retracts the instrument | H-9 | `SafetyCore::on_force`; force from the MuJoCo simulation (`sim/mujoco/bridge.py`) |

When several rules are broken at once, the reported reason follows this
priority: deadline (SR-5), workspace (SR-2), no-go zone (SR-9), speed
(SR-1), acceleration (SR-8), sequence (SR-3).

## Verification

| Requirement | Unit tests (`make test`) | Property tests | Fault campaign (`make campaign`) | On QNX (pending SDP access) |
|-------------|--------------------------|----------------|----------------------------------|-----------------------------|
| SR-1 | `sr1_*` | every accepted move obeys the speed limit | `overspeed` | `./controller --scenario overspeed` |
| SR-2 | `sr2_*` | position always inside the envelope | `leave_workspace`, `nan_command` | same scenarios |
| SR-3 | `sr3_*` | every accepted move has the expected cycle | `replay`, `skip_cycle` | same scenarios |
| SR-4 | `sr4_*` | once stopped, state and reason never change | every stop scenario re-sends after the stop | Inspection |
| SR-5 | `sr5_*` | a tick stops iff the deadline passed; shaped controller never stops over 100,000 cycles | `jitter`, `freeze_inside` (no stop), `freeze_past`, `freeze_long`, `crash`, each for both timing profiles | `make qnx-scenarios`: all 14 scenarios × 8 runs passed on QNX 8.0 under QEMU (`qemu` profile); worst-case latency on real hardware still to measure |
| SR-6 | `sr6_*` | a fault always stops | — | Inspection; forced timer failure |
| SR-7 | All of the above | All of the above | All of the above | Inspection |
| SR-8 | `sr8_*` | every accepted move obeys the acceleration limit | `reversal` | `./controller --scenario reversal` |
| SR-9 | `sr9_*` (incl. a path that clips a zone between two legal endpoints) | no accepted path enters a zone | `nogo_intrusion` | same scenario |
| SR-10 | `sr10_*` (limit, over limit, NaN/negative/infinite readings) | — | — | MuJoCo arm pushed into tissue: stop at the tissue edge, measured peak 10.3 N (see note) |
| All | — | — | `rogue_ai`: random and hostile commands every cycle; 0 envelope breaches required | same scenario |

Each test name starts with the requirement it verifies. The test suite was
also checked by deliberately breaking the core (removing the speed,
acceleration, zone or late-request checks, checking zones at the endpoint
only, checking only one axis of the bounds, not tracking velocity, using
`>=` for the deadline, letting a later stop overwrite the reason); every one
of those changes makes the test suite fail.

The campaign runs in **simulated time** on the host: it proves the logic
reacts correctly to each fault, not real-time performance. Timing evidence
must come from runs on QNX.

**SR-10 note (honest finding):** at full speed (about 0.45 m/s in the
simulated arm) the instrument hits the tissue hard enough that force passes
the 5 N limit within a few milliseconds; with force reported as a 100 Hz
peak value the stop latched at a measured 10.3 N. Limiting force reliably
needs the approach speed limited near tissue as well, which a real system
would add (e.g. a lower speed limit inside a margin around soft tissue).

## Out of scope (known limitations)

- No physical actuator, motor driver, or independent hardware safety circuit.
- Timing on QEMU is not representative of real hardware.
- The controller is not authenticated; any local process can open the service.
- Scheduling priorities and CPU partitioning are not yet configured.
- No-go zones are static circles; real systems track moving anatomy.
- The force sensor reading reaches the simulator over the same WebSocket as
  the planner's targets; on a real system the sensor would be wired to the
  monitor directly, and a stale-sensor timeout would be needed.
