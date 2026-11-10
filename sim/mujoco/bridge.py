#!/usr/bin/env python3
"""MuJoCo bridge: a physical robot arm driven by SurgiSafe-RT's safety monitor.

    make demo          # terminal 1: simulator + dashboard (http://localhost:8080)
    make mujoco        # terminal 2: this bridge, with the 3D viewer

Data flow:
    dashboard (mouse / camera hand tracking / autopilot) -> targets
      -> controller -> safety monitor (approves or stops every 10 ms)
      -> approved tip position --(WebSocket telemetry)--> this bridge
      -> SCARA inverse kinematics -> MuJoCo position actuators -> physics
      <- contact force on the instrument tip --(WebSocket)-- back to the monitor

The bridge only ever moves the arm to positions the monitor approved. On a
safe stop (or if telemetry goes quiet) it retracts the instrument and holds,
which is the physical "safe state". It also measures the contact force
between the instrument tip and the tissue and reports it ~50 times a second,
so the monitor can enforce the contact-force limit (SR-10).

Run with `mjpython` on macOS (required by MuJoCo's viewer); `--headless`
runs without a window and can save a PNG snapshot.
"""

import argparse
import json
import math
import os
import signal
import struct
import sys
import threading
import time
import zlib

import mujoco
import numpy as np
from websockets.sync.client import connect

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scene as S  # noqa: E402

STALE_S = 0.5  # telemetry older than this -> retract and hold


class Link:
    """WebSocket link to the simulator (or, later, the QNX gateway)."""

    def __init__(self, url):
        self.url = url
        self.lock = threading.Lock()
        self.ws = None
        self.state = None
        self.state_time = 0.0
        self.envelope = None
        self.events = []
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while True:
            try:
                with connect(self.url, open_timeout=3, max_size=2**20) as ws:
                    with self.lock:
                        self.ws = ws
                    print(f"[bridge] connected to {self.url}", flush=True)
                    for raw in ws:
                        try:
                            msg = json.loads(raw)
                        except ValueError:
                            continue
                        with self.lock:
                            if msg.get("type") == "state":
                                self.state = msg
                                self.state_time = time.monotonic()
                            elif msg.get("type") == "envelope":
                                self.envelope = msg
                            elif msg.get("type") == "event":
                                self.events.append(msg.get("text", ""))
            except Exception as exc:  # connection refused, closed, ...
                with self.lock:
                    self.ws = None
                print(f"[bridge] waiting for simulator at {self.url} ({exc.__class__.__name__})",
                      flush=True)
                time.sleep(1.0)

    def snapshot(self):
        with self.lock:
            fresh = self.state is not None and time.monotonic() - self.state_time < STALE_S
            return (dict(self.state) if self.state else None), fresh, self.events[:]

    def drain_events(self):
        with self.lock:
            ev, self.events = self.events, []
        return ev

    def send_force(self, newtons):
        with self.lock:
            ws = self.ws
        if ws is None:
            return
        try:
            ws.send(json.dumps({"cmd": "force", "n": round(newtons, 3)}))
        except Exception:
            pass


def write_png(path, rgb):
    """Minimal PNG writer (no Pillow needed)."""
    h, w, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


class Arm:
    def __init__(self):
        self.m = mujoco.MjModel.from_xml_string(S.build_xml())
        self.d = mujoco.MjData(self.m)
        gid = lambda n: mujoco.mj_name2id(self.m, mujoco.mjtObj.mjOBJ_GEOM, n)
        self.tip = gid("tip")
        self.tissue = gid("tissue")
        self.lamp = gid("lamp")
        self.target_mocap = self.m.body_mocapid[
            mujoco.mj_name2id(self.m, mujoco.mjtObj.mjOBJ_BODY, "target")]
        self.retracted = False
        self.hold_at = None  # Physical tip position frozen at a safe stop.
        self._f6 = np.zeros(6)
        # Start over workspace unit (1, 1), like the simulator, and settle.
        self.command(1.0, 1.0, retract=False)
        self.d.qpos[:3] = self.d.ctrl[:3]
        mujoco.mj_forward(self.m, self.d)

    def command(self, ux, uy, retract):
        x, y = S.unit_to_world(ux, uy)
        q1, q2 = S.scara_ik(x, y)
        z = S.TOOL_RETRACT_Z if retract else S.TOOL_WORK_Z
        self.d.ctrl[:] = [q1, q2, S.slide_for_tip_height(z)]

    def over_tissue(self, ux, uy):
        margin = S.TOOL_RADIUS / S.UNIT + 0.15
        return math.hypot(ux - S.TISSUE["x"], uy - S.TISSUE["y"]) < S.TISSUE["r"] + margin

    def contact_force(self):
        total = 0.0
        for i in range(self.d.ncon):
            c = self.d.contact[i]
            if {c.geom1, c.geom2} == {self.tip, self.tissue}:
                mujoco.mj_contactForce(self.m, self.d, i, self._f6)
                total += abs(self._f6[0])  # normal component
        return total

    def tip_units(self):
        p = self.d.geom_xpos[self.tip]
        return ((p[0] - S.WS_ORIGIN[0]) / S.UNIT, (p[1] - S.WS_ORIGIN[1]) / S.UNIT, p[2])

    def set_lamp(self, rgba):
        self.m.geom_rgba[self.lamp] = rgba

    def set_target(self, ux, uy):
        x, y = S.unit_to_world(ux, uy)
        self.d.mocap_pos[self.target_mocap] = [x, y, 0.002]


GREEN = (0.1, 0.75, 0.3, 1)
AMBER = (0.95, 0.65, 0.1, 1)
RED = (0.9, 0.15, 0.15, 1)
GRAY = (0.5, 0.5, 0.5, 1)


def control_step(arm, link):
    """Apply the latest approved position; return a one-line status."""
    st, fresh, _ = link.snapshot()
    if st is None:
        arm.set_lamp(GRAY)
        arm.command(*arm.tip_units()[:2], retract=True)
        return "no telemetry"
    ux, uy = st["x"], st["y"]
    stopped = bool(st.get("stopped"))
    if st.get("has_target"):
        arm.set_target(st["tx"], st["ty"])
    # On a stop, freeze where the tip physically is (it may lag the last
    # approved position, e.g. when blocked by tissue), not where it was
    # heading. Released on reset.
    if stopped or not fresh:
        if arm.hold_at is None:
            arm.hold_at = arm.tip_units()[:2]
        ux, uy = arm.hold_at
    else:
        arm.hold_at = None
    # Safe state: retract on a stop or when telemetry goes stale. After a
    # reset, stay retracted until the tip is clear of the tissue.
    retract = stopped or not fresh or (arm.retracted and arm.over_tissue(ux, uy))
    arm.retracted = retract
    arm.command(ux, uy, retract)
    if stopped or not fresh:
        arm.set_lamp(RED)
    elif st.get("holding"):
        arm.set_lamp(AMBER)
    else:
        arm.set_lamp(GREEN)
    state = "STOPPED: " + st.get("reason", "?") if stopped else ("stale" if not fresh else "running")
    return state


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--url", default="ws://127.0.0.1:8080/ws")
    ap.add_argument("--headless", action="store_true", help="no viewer window")
    ap.add_argument("--seconds", type=float, default=0, help="stop after N seconds (0 = run forever)")
    ap.add_argument("--snapshot", help="save a PNG of the final frame (headless)")
    ap.add_argument("--force-hz", type=float, default=100.0)
    args = ap.parse_args()

    arm = Arm()
    link = Link(args.url)
    renderer = None
    viewer = None
    if args.headless:
        if args.snapshot:
            renderer = mujoco.Renderer(arm.m, 480, 640)
    else:
        from mujoco import viewer as mj_viewer
        viewer = mj_viewer.launch_passive(arm.m, arm.d, show_left_ui=False,
                                              show_right_ui=False)
        cx, cy = S.unit_to_world(4, 4)
        viewer.cam.lookat[:] = [cx * 0.8, cy, 0.05]
        viewer.cam.distance = 1.25
        viewer.cam.azimuth = 150
        viewer.cam.elevation = -32

    # Ctrl-C or kill: leave the loop cleanly so the snapshot/viewer close run.
    def _stop(*_):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, _stop)

    start = time.monotonic()
    next_force = start
    next_print = start
    peak_force = 0.0  # Peak since the last report, so no spike is missed.
    last_state = ""
    try:
        while True:
            now = time.monotonic()
            if args.seconds and now - start > args.seconds:
                break
            if viewer is not None and not viewer.is_running():
                break
            state = control_step(arm, link)
            # Keep simulated time in step with wall-clock time.
            target_t = now - start
            while arm.d.time < target_t:
                mujoco.mj_step(arm.m, arm.d)
                peak_force = max(peak_force, arm.contact_force())
            force = arm.contact_force()
            if now >= next_force:
                link.send_force(peak_force)
                peak_force = 0.0
                next_force = now + 1.0 / args.force_hz
            for ev in link.drain_events():
                print(f"[monitor] {ev}", flush=True)
            if state != last_state or now >= next_print:
                ux, uy, z = arm.tip_units()
                print(f"[bridge] t={target_t:6.1f}s  {state:<32} tip=({ux:5.2f},{uy:5.2f}) "
                      f"z={z * 100:4.1f}cm  force={force:5.2f} N", flush=True)
                last_state = state
                next_print = now + 2.0
            if viewer is not None:
                viewer.sync()
            time.sleep(0.004)
    except KeyboardInterrupt:
        print("[bridge] stopping", flush=True)
    finally:
        if renderer is not None:
            cam = mujoco.MjvCamera()
            cx, cy = S.unit_to_world(4, 4)
            cam.lookat[:] = [cx * 0.8, cy, 0.05]
            cam.distance, cam.azimuth, cam.elevation = 1.25, 150, -32
            renderer.update_scene(arm.d, camera=cam)
            write_png(args.snapshot, renderer.render())
            print(f"[bridge] snapshot saved to {args.snapshot}", flush=True)
        if viewer is not None:
            viewer.close()


if __name__ == "__main__":
    main()
