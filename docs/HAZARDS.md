# Hazards, safety requirements and verification

SurgiSafe-RT is an **educational simulation**. This table is written in the
style of a medical-device risk analysis (inspired by ISO 14971 and
IEC 62304) to show how each safety rule traces to a hazard and to the
evidence that it works. It is **not** a certified analysis, and the project
must not be used with a real instrument or patient.

## Hazards

| ID | Hazard | Possible causes |
|----|--------|-----------------|
| H-1 | Instrument moves too far in one control cycle | Controller bug, corrupted message, AI planner output outside expectations |
| H-2 | Instrument leaves its permitted workspace | Accumulated error, NaN/infinity in the command, deliberate bad command |
| H-3 | A stale, duplicated or out-of-order command is executed | Retransmission, replay, controller restart without reset |
| H-4 | The instrument keeps executing old commands while the controller is stalled or dead | Controller freeze, CPU starvation, crash, priority inversion |
| H-5 | Motion silently resumes after a fault | Automatic recovery, a later valid message clearing the stop |
| H-6 | The supervisor cannot enforce its checks | Timer creation or arming fails |

## Safety requirements

| ID | Requirement | Mitigates | Implemented in |
|----|-------------|-----------|----------------|
| SR-1 | A move is committed only if \|proposed − current\| ≤ 0.1 | H-1 | `SafetyCore::on_request` ([safety_core.cpp](../safety_core.cpp)) |
| SR-2 | A move is committed only if the target is finite and within [0, 8.0] | H-2 | `SafetyCore::on_request` |
| SR-3 | A move is committed only if its cycle number is exactly the next expected one | H-3 | `SafetyCore::on_request` |
| SR-4 | Any violation latches a safe stop; no later message can clear it, and the first cause is always reported | H-5 | `SafetyCore::latch` |
| SR-5 | If no accepted request arrives within 15 ms of the previous accepted one (absolute deadline), the monitor latches a safe stop, whether or not the timer event has been processed yet | H-4 | `SafetyCore::on_tick`, `on_request`; QNX timer pulse in `WatchdogTimer` ([safety_monitor.cpp](../safety_monitor.cpp)) |
| SR-6 | If the monitor cannot create its watchdog it refuses to start; if it cannot arm it, it latches a safe stop | H-6 | `main` and `WatchdogTimer` in safety_monitor.cpp; `SafetyCore::on_fault` |
| SR-7 | Only the monitor changes the instrument position; the controller only proposes | H-1–H-5 | Architecture: position lives only in `SafetyCore` |

## Verification

| Requirement | Host unit tests (`make test`) | Property tests | On QNX (pending SDP access) |
|-------------|-------------------------------|----------------|-----------------------------|
| SR-1 | `sr1_*` | every accepted move obeys the step limit | — |
| SR-2 | `sr2_*` | position always finite and within [0, 8.0] | — |
| SR-3 | `sr3_*` | every accepted move has the expected cycle | — |
| SR-4 | `sr4_*` | once stopped, state and reason never change | — |
| SR-5 | `sr5_*` (incl. stale/forged tick, late request, freeze scenario) | a tick stops iff the deadline passed; on-time controller never stops over 100,000 cycles | Fault-injection run (`./controller`); worst-case detection latency measurement |
| SR-6 | `sr6_*` | a fault always stops | Inspection; forced timer failure |
| SR-7 | All of the above | All of the above | Inspection |

Each test name starts with the requirement it verifies. The test suite was
also checked by deliberately breaking the core (removing the late-request
check, using `>=` instead of `>` for the deadline, ignoring the deadline on
ticks, not refreshing the deadline, letting a later stop overwrite the
reason); every one of those changes makes the suite fail.

## Out of scope (known limitations)

- No physical actuator, motor driver, or independent hardware safety circuit.
- Timing on QEMU is not representative of real hardware.
- The controller is not authenticated; any local process can open the service.
- Scheduling priorities and CPU partitioning are not yet configured.
