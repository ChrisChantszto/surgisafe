# SurgiSafe-RT

[![CI](https://github.com/ChrisChantszto/surgisafe/actions/workflows/ci.yml/badge.svg)](https://github.com/ChrisChantszto/surgisafe/actions/workflows/ci.yml)

**Educational QNX 8.0 surgical-instrument motion and safety simulation** (no clinical or physical-robot use).

**The scenario: a remote, AI-assisted ultrasound robot.** A sonographer in another city scans a patient through a robot arm, steering the ultrasound probe with hand gestures (webcam hand tracking) or letting an AI auto-scan. The probe must press on the skin gently (at most 5 N) and must never enter a fresh surgical wound (a no-go zone). Neither the AI nor the network is trusted: a QNX safety monitor checks every single move and latches a safe stop when a rule is broken or the commands go silent. *AI proposes, QNX decides.*

| In the demo | Stands for |
|---|---|
| Instrument tip | Ultrasound probe |
| Pink tissue mound (MuJoCo physics) | Scan site: the probe touches the skin here; contact force limit 5 N |
| Red no-go zone | Surgical wound: the probe must never enter it |
| Watchdog | Network lag, a frozen or crashed controller |
| Rogue commands | A hacked or buggy controller |

The motion limits are scaled for a visible demo (0.1 units = 5 mm per 10 ms cycle is 0.5 m/s, much faster than a clinical robot would move).

A virtual instrument tip moves in a 2D workspace. An untrusted controller proposes every move; a small QNX safety monitor decides whether it happens. The monitor enforces a *safety envelope*: workspace bounds, a speed limit, an acceleration limit, no-go zones (simple "virtual fixtures"), in-order cycle numbers and a heartbeat deadline (15 ms on the `hardware` timing profile, the design target; 50 ms on the `qemu` profile used under QEMU software emulation). Any violation latches a safe stop.

Two native QNX processes communicate via `name_attach`/`name_open` and
`MsgSend`/`MsgReceive`/`MsgReply`:

- `controller`: proposes a movement every 10 ms by replaying one fault-injection scenario (default: `freeze_long`, a stall of twice the deadline at cycle 50). `--profile hardware|qemu` must match the monitor.
- `safety_monitor`: is the *sole owner* of the virtual instrument position; rejects invalid movement requests and latches a safe-stop state when the controller misses its heartbeat deadline (`--profile` selects 15 ms or 50 ms). The watchdog is a QNX timer (`timer_create` + `SIGEV_PULSE_INIT`) armed with an absolute `CLOCK_MONOTONIC` deadline that delivers a pulse to the monitor's own channel.
- `safety_core.hpp/.cpp`: portable, QNX-free decision logic: the `Envelope` limits, the checks, and the latched safe stop and its reason. No heap allocation after construction. The monitor feeds IPC and timer events into it.
- `motion_shaper.hpp`: controller-side helper that turns "go to this target" into steps within the speed and acceleration limits (with a 10% margin).
- `scenarios.hpp/.cpp`: 14 deterministic fault-injection scenarios, shared by the host campaign and the QNX controller.
- `tools/fault_campaign.cpp`: runs every scenario many times against the core in simulated time and writes a CSV report.
- `live_controller.hpp/.cpp`: the interactive controller: a 10 ms loop toward the latest target, holding still when the target goes stale, plus demo faults (freeze, crash, rogue commands).
- `dashboard/`: browser dashboard (the untrusted planner side): mouse, webcam hand tracking, AI autopilot.
- `sim/sim_server.cpp` and `net/`: host simulator serving the dashboard over HTTP and a WebSocket, running the real safety core.
- `cpu_stress.hpp/.cpp`: low-priority background CPU load for the "Stress the CPU" demo.
- `qnx_gateway.cpp`: the same dashboard protocol on QNX, forwarding every move, force reading and reset to `safety_monitor` with `MsgSend` (`make qnx-live`).
- `sim/mujoco/`: MuJoCo robot arm (physics + contact force) that follows the approved positions.
- `protocol.hpp`: QNX IPC message structures.
- `tests/`: host unit tests and randomized property tests for the safety core.
- `docs/HAZARDS.md`: hazards, safety requirements (SR-1…SR-10) and how each is verified.

This is a **learning prototype**. It builds and runs on QNX 8.0 (aarch64le, QEMU); see [qnx/README.md](qnx/README.md). Neither physical actuator safety nor hard real-time behavior is demonstrated by this repository alone.

## Requirements

- QNX SDP 8.0 with the `q++` cross-compiler and access to QNX OS 8.0 x86-64, for example the QNX QEMU target.
- A build host supported by the installed QNX SDP (such as x86-64 Linux), or the official QNX macOS Docker workflow.

## Host unit tests (no QNX needed)

```sh
make test
```

Builds and runs the safety-core tests on macOS or Linux with AddressSanitizer and UndefinedBehaviorSanitizer. This includes randomized property tests that drive the core with hundreds of thousands of random (often hostile) events; set `SURGISAFE_SEED=<n>` to try different random sequences. CI runs the same tests and the campaign on Linux (GCC and Clang) and macOS, plus `cppcheck`.

## Live dashboard demo (no QNX needed)

```sh
make demo
```

Then open http://localhost:8080 in Chrome or Safari. The simulator runs the real `SafetyCore` and `LiveController` with a 10 ms loop; the browser is the untrusted planner.

- **Who is steering:** *Mouse*; *Hand (camera AI)* (your index fingertip, tracked by Google MediaPipe running locally in the browser; video never leaves your machine, the model files are downloaded once from a CDN); *AI autopilot* (orbits a safe region); *Autopilot with error* (its orbit drifts into the no-go zone).
- **Inject a fault:** freeze the controller for 20 ms, crash it, or make it send rogue commands. Each ends in a latched SAFE STOP with its reason; **Operator reset** re-enables motion.
- **Two different "silences":** if the planner stops sending targets (mouse leaves the workspace, hand out of view), the controller brakes and **holds** — a safe pause, not a fault. If the controller itself stops sending heartbeats, the monitor's watchdog latches a **SAFE STOP**.

The simulator listens on 127.0.0.1 only. Its timing is best-effort on macOS/Linux (the watchdog is checked about every millisecond), so it demonstrates behaviour, not real-time guarantees. On QNX, the same network code is intended to become a gateway process that forwards requests to `safety_monitor` with `MsgSend`.

## Real-time proof: stress the CPU

The dashboard's **Stress the CPU (60 s)** button starts one busy-looping thread per CPU. On QNX they run at **priority 5**, below the gateway (50) and the safety monitor (60), so QNX's preemptive priority scheduler only gives them CPU time the safety path does not need. Every control cycle reports two measurements, charted live with stress periods shaded, plus a *without stress / with stress* table of the worst values:

- **Safety decision time:** on QNX, the `MsgSend` round trip from the gateway to `safety_monitor` and back (on the Mac simulator, the in-process `SafetyCore` call).
- **Cycle start delay:** how late each 10 ms cycle started compared with its schedule. The loop waits in `poll()` with 1 ms resolution, so about 1 ms of delay is normal with or without load.

Measured on QNX 8.0 under QEMU (2 emulated CPUs, `qemu` profile, AI auto-scan running): during 60 s of stress (6,001 cycles, 2 load threads at priority 5 doing about 350 M iterations/s) there was **no safety stop**; the worst safety decision was 0.59 ms (2.54 ms without load) and the worst cycle start delay was 11.2 ms (5.5 ms without load), well inside the 50 ms deadline. The larger start delay under load is likely emulator noise (QEMU's own threads compete on the Mac); real hardware is needed to say more.

The load switches itself off after 60 s. On macOS the load threads keep their normal priority and the simulator's loop uses Mach real-time scheduling instead. Under QEMU software emulation, emulator stalls still show up as occasional spikes with or without load: the comparison is meaningful, the absolute numbers are not real-time measurements.

## Robot arm in MuJoCo (no QNX needed)

A physics-simulated SCARA-style instrument arm follows every position the safety monitor approves, and reports the **contact force** between the instrument tip and a soft tissue mound back to the monitor, which enforces a 5 N limit (rule SR-10).

```sh
make mujoco-setup        # once: Python venv with MuJoCo
make demo                # terminal 1: simulator + dashboard (http://localhost:8080)
make mujoco              # terminal 2: 3D arm (MuJoCo viewer, via mjpython)
```

To put **real QNX** in charge instead of the Mac simulator, boot the QNX VM and run `make qnx-live`, open http://localhost:8088, and start the arm with `make mujoco PORT=8088` (see [qnx/README.md](qnx/README.md)).

Steer from the dashboard (mouse, camera hand tracking, or autopilot). Push the instrument into the pink tissue and the monitor stops on contact force; the arm holds where the tip physically is and retracts the instrument. Every other stop also retracts and holds. The simulator asks macOS for real-time thread scheduling (Mach time-constraint policy) so the 15 ms watchdog is not tripped by timer coalescing.

## Fault-injection campaign (no QNX needed)

```sh
make campaign             # 200 trials of each scenario
make campaign TRIALS=1000
build/fault_campaign --list
```

Each scenario is checked against its expected outcome (no stop, or a stop with a specific reason), and every committed move is checked against the envelope. Results go to `build/campaign.csv`. The campaign uses **simulated time** (the watchdog timer fires at the deadline plus 0–200 µs of simulated latency), so it verifies the logic, not real-time performance.

## On QNX 8.0 from a Mac (Apple Silicon)

See **[qnx/README.md](qnx/README.md)**: a Docker build container for the SDP, an aarch64le QNX image in QEMU, and one-word commands (`make qnx-boot`, `make qnx-test`, `make qnx-scenarios`). All 55 unit/property tests and every logic scenario pass on QNX; timing under QEMU software emulation is not real-time, and the measured results are documented there.

## Build (from QNX SDP environment)

```sh
make qnx
```

Binaries are written to `build/qnx/`. If your SDP exposes a different compiler variant, check `q++ -V` and override it, e.g. `make qnx QNX_VARIANT=gcc_ntoaarch64le`. Copy both binaries to the QNX **target**; these are not macOS binaries.

## Run on QNX (two sessions)

Terminal A:
```sh
./safety_monitor
```
Terminal B (fault injection):
```sh
./controller
```

Expected behavior: accepted moves through cycle 49, then a 20 ms injected delay makes the controller miss its heartbeat deadline and the watchdog pulse latches a safe stop; the next request is rejected with reason 1 (watchdog timeout) and the controller exits with status 1.

Any campaign scenario can be replayed on the target (restart the monitor between runs):
```sh
./controller --list
./controller --scenario reversal
``` Exact timing is host/target dependent. On QEMU, failure to trigger reliably may reflect virtualization scheduling behavior.

For normal operation, restart the monitor (state is latched) and use:
```sh
./controller --normal
```

When the controller finishes or exits, it stops sending heartbeats, so the monitor latches a watchdog stop about 15 ms later. This is intended: a silent controller is treated as a fault.

## Reply reason codes

| Code | Meaning |
|------|---------|
| 0 | Accepted |
| 1 | Watchdog timeout |
| 2 | Out of workspace (non-finite, or outside the 8 × 8 bounds) |
| 3 | Invalid sequence (cycle skipped, repeated or out of order) |
| 4 | Monitor fault (could not arm its watchdog, or bad start position) |
| 5 | Speed limit (step longer than 0.1 per cycle) |
| 6 | Acceleration limit (step changed by more than 0.02 per cycle) |
| 7 | No-go zone (path enters or crosses a no-go zone) |
| 8 | Contact force (force sensor above 5 N, or an invalid reading) |

The safe stop is latched: every later reply reports the reason that caused the **first** stop.

## Design notes and limitations

- The heartbeat deadline is **absolute**: it is set to (time the last request was accepted + 15 ms), so unrelated messages or pulses cannot extend it. The deadline is checked both when the timer pulse arrives and when the next request arrives, so a late request is refused even if the pulse is still queued, and a stale or forged pulse cannot stop a healthy controller. This is still a software watchdog, not a hardware one, and the reaction time depends on scheduling.
- `MsgSend` blocks the controller awaiting the monitor reply. No timeout is implemented on that block in this starter version.
- The monitor assumes one controller and monotonic cycle numbers; it doesn't authenticate a remote client or detect forged requests.
- No motor driver, independent physical safety circuit, or clinical safety assurance is included.
- In a real safety-rated system, implement and validate scheduling priorities, fault recovery, client identity, supervisor lifecycle and output inhibition.

## Next milestones

1. Run on real hardware (for example a Raspberry Pi 4 with QNX 8.0) to turn the timing measurements into real-time evidence.
2. Log the live timing measurements to CSV for the report.
3. Add independent service supervision and an explicit authenticated reset procedure.
4. Add a separate visualization of the virtual instrument's accepted moves.

## License

MIT — see [LICENSE](LICENSE).
