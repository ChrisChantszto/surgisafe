# Building and running SurgiSafe-RT on QNX 8.0 from a Mac (Apple Silicon)

QNX SDP 8.0 host tools run only on x86-64 Linux or Windows, so on a Mac:

```
Mac (Apple Silicon)
├─ Docker container "qnx-build" (Ubuntu 22.04, x86-64 via emulation)
│    └─ QNX SDP 8.0 in ~/qnx800 (shared with the Mac): q++, mkqnximage
└─ QEMU (Homebrew) ── runs ──► QNX 8.0 aarch64le target
                                 ├─ safety_monitor
                                 └─ controller --scenario …
```

Tested with: QNX SDP 8.0 (GCC 12.2, `gcc_ntoaarch64le`), QNX Software Center
2.0.4 (Linux), QEMU 11.1.3, macOS 15.5 on an Apple M4.

## One-time setup

1. **Docker Desktop** for Mac; enable *Settings → General → Use Rosetta for
   x86_64/amd64 emulation*.
2. **Build container**: `qnx/docker/setup.sh` (Ubuntu 22.04; replaces QNX's
   docker-mac example, whose Ubuntu 20.04 + deadsnakes PPA no longer builds).
3. **QNX Software Center for Linux** (there is no macOS build for SDP 8.0):
   download *QNX Software Center 2.0.4 – Linux Hosts* from myQNX, then inside
   the container (`docker exec -it qnx-build bash --rcfile /etc/qnx-bashrc`):
   ```sh
   ./qnx-setup-2.0.4-*-linux.run force-override disable-auto-start agree-to-license-terms ~/qnxinstall
   # myQNX login in a private options file (one argument per line)
   read -p "myQNX email: " QE && read -s -p "myQNX password: " QP && echo && \
     printf '%s\n' "-myqnx.user=$QE" "-myqnx.password=$QP" > ~/.qnx/myqnx.opts && chmod 600 ~/.qnx/myqnx.opts && unset QP
   cd ~/qnxinstall/qnxsoftwarecenter
   ./qnxsoftwarecenter_clt @$HOME/.qnx/myqnx.opts -mirrorBaseline qnx800
   ./qnxsoftwarecenter_clt @$HOME/.qnx/myqnx.opts -syncLicenseKeys -activateAll -listLicenseKeys
   ./qnxsoftwarecenter_clt @$HOME/.qnx/myqnx.opts -destination ~/qnx800 -installBaseline com.qnx.qnx800
   # QEMU "virt" board support, not part of the baseline:
   ./qnxsoftwarecenter_clt @$HOME/.qnx/myqnx.opts -destination ~/qnx800 \
     -installPackage com.qnx.qnx800.target.qemuvirt,com.qnx.qnx800.target.driver.virtio,com.qnx.qnx800.target.driver.virtio.startup
   rm -f ~/.qnx/myqnx.opts
   ```
4. **QEMU**: `brew install qemu`.
5. **QNX image**: `make qnx-image` (runs `mkqnximage --type=qemu
   --arch=aarch64le` in the container; creates a dedicated SSH key in
   `~/qnx-tools/vm/keys`).

## Everyday commands (from the repository root on the Mac)

| Command | What it does |
|---|---|
| `make qnx-boot` | Boot QNX in QEMU with the console in this terminal (quit: Ctrl-A, X) |
| `make qnx-build` | Cross-compile monitor, controller and unit tests for aarch64le |
| `make qnx-test` | Build, copy to QNX, run the 55 unit/property tests **on QNX** |
| `make qnx-scenarios REPEATS=8` | Run every fault scenario through the real QNX processes (`PROFILE=qemu` by default) |
| `make qnx-report` | Fetch the last results and build `build/qnx-report.html`: outcome matrix, heartbeat-gap chart, tip path and wake-up lateness histogram (self-contained, opens offline) |
| `make qnx-live` | **QNX in charge of the live demo**: starts `safety_monitor` and `qnx_gateway` on QNX. Then open http://localhost:8088 (dashboard served by QNX) and run `make mujoco PORT=8088` for the 3D arm |
| `qnx/vm/target.sh ssh` | Shell on the QNX target (`/data/surgisafe` holds the programs) |

## Live demo with QNX in charge

```
Mac: browser dashboard ─┐                    ┌─ QNX 8.0 (QEMU) ───────────────────┐
     (mouse/camera/AI)  ├─ WebSocket :8088 ──┤ qnx_gateway ──MsgSend──► safety_monitor
Mac: MuJoCo arm ────────┘   (port forward)   │ (10 ms loop)             (decides; timer-
     (physics, force)                        │                           pulse watchdog)
                                             └────────────────────────────────────┘
```

`qnx_gateway.cpp` serves the dashboard and speaks the same JSON protocol as
the Mac simulator, but makes no safety decisions: every 10 ms it sends the
next proposed step to `safety_monitor` with `MsgSend`; MuJoCo's force
readings go to the monitor as `ForceReport` messages; operator reset is a
`ResetRequest`; when it is not sending moves it asks for the monitor's status,
so it is the monitor's own QNX timer pulse that detects a silent controller.
Dashboard header: "decisions by QNX 8.0 safety monitor".

Verified end to end (`qemu` profile): 60 s of continuous motion with the
MuJoCo arm and no false stops; then, all decided by the QNX monitor:
contact force (stopped at 16.4 N), gateway crash (watchdog, seen ~60 ms after
the last approved move), rogue commands (acceleration limit), aiming at the
no-go zone (no-go zone), and operator resets. The contact-force overshoot is
larger than on the Mac simulator (10.3 N) because the force reading and the
stop each cross the Mac-QNX network hop.

## Workarounds we needed (QNX 8.0 + QEMU 11 + macOS)

- **`_QNX_SOURCE`**: QNX headers hide `clock_gettime`, `timer_create`,
  `SIGEV_PULSE_INIT` in strict ISO mode; the Makefile defines `_QNX_SOURCE`
  and uses `gnu++17`.
- **sshd-auth**: the SDP's (auto-patched) OpenSSH needs `sshd-auth`, but the
  stock `mkqnximage` template does not include it, so every SSH connection
  was reset. `qnx/vm/image.sh` adds it through `local/snippets`.
- **No networking from QNX's runimage on macOS**: `qnx/vm/boot.sh` uses QEMU
  user networking instead (`localhost:2222` → QNX ssh, `localhost:8088` →
  QNX port 8080).
- **QEMU 11 dropped `virt-4.2`** (used by QNX's runimage): we use `virt` with
  `gic-version=2` and `cortex-a57`, which is equivalent.
- **Apple hvf acceleration does not boot QNX**: hvf only provides GICv3, and
  `startup-qemu-virt` asserts (`gic_v3.c:966`). So the target runs under
  pure software emulation (TCG).

## Results so far (QNX 8.0 aarch64le, QEMU TCG on an M4)

- **All 56 unit and property tests pass on QNX**, identical to the host.
- **Every logic rule works end to end over QNX message passing**: speed,
  acceleration, workspace, NaN, sequence, no-go zone, rogue commands, and the
  watchdog catches freezes and crashes.
- **Timing under emulation is not real-time.** The controller measures how
  late each 10 ms wake-up is: on average 1.5–2.5 ms late, with spikes of
  7–15 ms (once ~36 ms in an experiment with QEMU's `-icount`). With a 15 ms
  heartbeat deadline, those spikes sometimes trigger a watchdog stop in
  scenarios that should not stop (a false alarm — the system still fails
  safe). This is a property of software emulation on a busy desktop host, not
  evidence about QNX on hardware; real-time claims need a hardware target
  (e.g. a Raspberry Pi) or hardware-accelerated virtualisation.
- **The `qemu` timing profile** makes the logic testable despite that: the
  heartbeat deadline is 50 ms (instead of 15 ms) and timing scenarios keep
  20 ms of margin either side of it. With it, **all 14 scenarios passed 8/8
  on QNX (112/112 runs)**; the largest wake-up spike in that run was 11 ms.
  The `hardware` profile (15 ms) remains the design target and the default;
  start both processes with the same `--profile`. The host campaign
  (`make campaign`) checks both profiles.
- **Lesson fed back into the design**: an early version of the host campaign
  modelled message delivery as instantaneous, so scenarios with only 1 ms of
  margin before the deadline passed in simulation and failed on QNX. The
  campaign now models delivery latency (2 ms on `hardware`, 15 ms on `qemu`),
  and timing scenarios are defined relative to the deadline with a margin
  larger than that latency (`jitter`, `freeze_inside`, `freeze_past`,
  `freeze_long`).
- **Note:** after a controller finishes normally the monitor still latches a
  watchdog stop (a silent controller is a fault), so `nominal`, `jitter` and
  `freeze_inside` show a monitor-side `WATCHDOG TIMEOUT` after passing.
