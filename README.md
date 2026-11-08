# SurgiSafe-RT

[![CI](https://github.com/ChrisChantszto/surgisafe/actions/workflows/ci.yml/badge.svg)](https://github.com/ChrisChantszto/surgisafe/actions/workflows/ci.yml)

**Educational QNX 8.0 surgical-instrument motion and safety simulation** (no clinical or physical-robot use).

Two native QNX processes communicate via `name_attach`/`name_open` and
`MsgSend`/`MsgReceive`/`MsgReply`:

- `controller`: proposes a movement every 10 ms; optionally freezes for 20 ms at cycle 40.
- `safety_monitor`: is the *sole owner* of the virtual instrument position; rejects invalid movement requests and latches a safe-stop state when the controller misses its 15 ms heartbeat deadline. The watchdog is a QNX timer (`timer_create` + `SIGEV_PULSE_INIT`) armed with an absolute `CLOCK_MONOTONIC` deadline that delivers a pulse to the monitor's own channel.
- `safety_core.hpp/.cpp`: portable, QNX-free decision logic (motion limits, sequence checks, latched safe-stop and its reason). The monitor feeds IPC and timer events into it.
- `protocol.hpp`: QNX IPC message structures.
- `tests/`: host unit tests and randomized property tests for the safety core.
- `docs/HAZARDS.md`: hazards, safety requirements (SR-1…SR-7) and how each is verified.

This is a **learning prototype**. It has not been compiled or validated on QNX in the generation environment. Neither physical actuator safety nor hard real-time behavior is demonstrated by this repository alone.

## Requirements

- QNX SDP 8.0 with the `q++` cross-compiler and access to QNX OS 8.0 x86-64, for example the QNX QEMU target.
- A build host supported by the installed QNX SDP (such as x86-64 Linux), or the official QNX macOS Docker workflow.

## Host unit tests (no QNX needed)

```sh
make test
```

Builds and runs the safety-core tests on macOS or Linux with AddressSanitizer and UndefinedBehaviorSanitizer. This includes randomized property tests that drive the core with hundreds of thousands of random (often hostile) events; set `SURGISAFE_SEED=<n>` to try different random sequences. CI runs the same tests on Linux (GCC and Clang) and macOS, plus `cppcheck`.

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

Expected behavior: accepted moves through cycle 39, then a 20 ms injected delay makes the controller miss its heartbeat deadline and the watchdog pulse latches a safe stop; the next request is rejected with reason 1 (watchdog timeout) and the controller exits with status 1. Exact timing is host/target dependent. On QEMU, failure to trigger reliably may reflect virtualization scheduling behavior.

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
| 2 | Invalid motion (non-finite, out of `[0, 8.0]`, or step > 0.1) |
| 3 | Invalid sequence (cycle skipped, repeated or out of order) |
| 4 | Monitor fault (monitor could not arm its watchdog) |

The safe stop is latched: every later reply reports the reason that caused the **first** stop.

## Design notes and limitations

- The heartbeat deadline is **absolute**: it is set to (time the last request was accepted + 15 ms), so unrelated messages or pulses cannot extend it. The deadline is checked both when the timer pulse arrives and when the next request arrives, so a late request is refused even if the pulse is still queued, and a stale or forged pulse cannot stop a healthy controller. This is still a software watchdog, not a hardware one, and the reaction time depends on scheduling.
- `MsgSend` blocks the controller awaiting the monitor reply. No timeout is implemented on that block in this starter version.
- The monitor assumes one controller and monotonic cycle numbers; it doesn't authenticate a remote client or detect forged requests.
- No motor driver, independent physical safety circuit, or clinical safety assurance is included.
- In a real safety-rated system, implement and validate scheduling priorities, fault recovery, client identity, supervisor lifecycle and output inhibition.

## Next milestones

1. Log measured controller start lateness, IPC round-trip time, and monitor response latency to CSV.
2. Test QNX priorities and CPU load under QEMU (report the limitations of virtualized measurements).
3. Add independent service supervision and an explicit authenticated reset procedure.
4. Add a separate visualization of the virtual instrument's accepted moves.

## License

MIT — see [LICENSE](LICENSE).
