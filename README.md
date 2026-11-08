# SurgiSafe-RT

**Educational QNX 8.0 surgical-instrument motion and safety simulation** (no clinical or physical-robot use).

Two native QNX processes communicate via `name_attach`/`name_open` and
`MsgSend`/`MsgReceive`/`MsgReply`:

- `controller`: proposes a movement every 10 ms; optionally freezes for 20 ms at cycle 40.
- `safety_monitor`: is the *sole owner* of the virtual instrument position; rejects invalid movement requests and latches a safe-stop state when a 15 ms message-receive timeout expires.
- `protocol.hpp`: IPC message structures and motion limits.

This is a **learning prototype**. It has not been compiled or validated on QNX in the generation environment. Neither physical actuator safety nor hard real-time behavior is demonstrated by this repository alone.

## Requirements

- QNX SDP 8.0 with the `q++` cross-compiler and access to QNX OS 8.0 x86-64, for example the QNX QEMU target.
- A build host supported by the installed QNX SDP (such as x86-64 Linux), or the official QNX macOS Docker workflow.

## Build (from QNX SDP environment)

```sh
q++ -Vgcc_ntox86_64 -std=c++17 -O2 -Wall -Wextra -pthread safety_monitor.cpp -o safety_monitor
q++ -Vgcc_ntox86_64 -std=c++17 -O2 -Wall -Wextra -pthread controller.cpp -o controller
```

If your SDP exposes a different compiler variant, check `q++ -V` and adapt the target flag. Copy both binaries to the QNX **target**; these are not macOS binaries.

## Run on QNX (two sessions)

Terminal A:
```sh
./safety_monitor
```
Terminal B (fault injection):
```sh
./controller
```

Expected behavior: accepted moves through cycle 39, then a 20 ms injected delay causes a receive timeout in the monitor; the next request is rejected. Exact timing is host/target dependent. On QEMU, failure to trigger reliably may reflect virtualization scheduling behavior.

For normal operation, restart the monitor (state is latched) and use:
```sh
./controller --normal
```

## Design notes and limitations

- Timeout is measured **from each MsgReceive call**, not from the most recent scheduled release or a hardware watchdog; pulses and scheduling delays can affect the observed time.
- `MsgSend` blocks the controller awaiting the monitor reply. No timeout is implemented on that block in this starter version.
- The monitor assumes one controller and monotonic cycle numbers; it doesn't authenticate a remote client or detect forged requests.
- No motor driver, independent physical safety circuit, or clinical safety assurance is included.
- In a real safety-rated system, implement and validate scheduling priorities, absolute deadlines, fault recovery, client identity, supervisor lifecycle and output inhibition.

## Next milestones

1. Log measured controller start lateness, IPC round-trip time, and monitor response latency to CSV.
2. Test QNX priorities and CPU load under QEMU (report the limitations of virtualized measurements).
3. Add independent service supervision and an explicit authenticated reset procedure.
4. Add a separate visualization of the virtual instrument's accepted moves.

## License

MIT — see [LICENSE](LICENSE).
