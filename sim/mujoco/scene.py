"""MuJoCo scene for SurgiSafe-RT: a SCARA-style instrument arm over a table.

The safety core works in a 2D workspace of 8 x 8 "units". Here one unit is
5 cm, so the workspace is a 40 x 40 cm square on the table. The arm moves the
instrument tip in that plane; a vertical slide raises and lowers the
instrument. A soft tissue mound gives the instrument something to press
against, which is what the contact-force rule (SR-10) is about.

Everything is defined here, in one place, so the bridge and the scene agree.
"""

import math

UNIT = 0.05                      # metres per workspace unit
WS_ORIGIN = (0.15, -0.20)        # world (x, y) of workspace unit (0, 0)
WS_SIZE = 8.0                    # workspace is 8 x 8 units

L1, L2 = 0.32, 0.30              # arm link lengths (m)
SHOULDER_Z = 0.30                # height of the arm links above the table
TOOL_WORK_Z = 0.018              # tip height while working (m above table)
TOOL_RETRACT_Z = 0.09            # tip height when retracted (safe state)
TOOL_RADIUS = 0.006

NOGO = {"x": 5.0, "y": 5.0, "r": 1.0}          # units, mirrors default_envelope()
TISSUE = {"x": 2.5, "y": 5.5, "r": 0.9, "h": 0.045}  # units (r), metres (h)


def unit_to_world(ux, uy):
    return WS_ORIGIN[0] + ux * UNIT, WS_ORIGIN[1] + uy * UNIT


def scara_ik(x, y):
    """Joint angles (q1, q2) that put the tool axis over world (x, y)."""
    r2 = x * x + y * y
    c2 = (r2 - L1 * L1 - L2 * L2) / (2 * L1 * L2)
    c2 = max(-1.0, min(1.0, c2))
    q2 = math.acos(c2)                      # elbow on one consistent side
    q1 = math.atan2(y, x) - math.atan2(L2 * math.sin(q2), L1 + L2 * math.cos(q2))
    return q1, q2


def build_xml():
    wx0, wy0 = WS_ORIGIN
    side = WS_SIZE * UNIT
    cx, cy = wx0 + side / 2, wy0 + side / 2
    zx, zy = unit_to_world(NOGO["x"], NOGO["y"])
    tx, ty = unit_to_world(TISSUE["x"], TISSUE["y"])
    # Slide joint: 0 = retracted; positive lowers the tool.
    slide_range = TOOL_RETRACT_Z - TOOL_WORK_Z + 0.03
    shaft_len = SHOULDER_Z - TOOL_RETRACT_Z
    border = "\n".join(
        f'<geom type="box" pos="{px} {py} 0.0006" size="{sx} {sy} 0.0006" '
        f'rgba="0.55 0.55 0.6 1" contype="0" conaffinity="0"/>'
        for px, py, sx, sy in [
            (cx, wy0, side / 2, 0.002), (cx, wy0 + side, side / 2, 0.002),
            (wx0, cy, 0.002, side / 2), (wx0 + side, cy, 0.002, side / 2)])
    return f"""
<mujoco model="surgisafe">
  <compiler angle="radian"/>
  <option timestep="0.002" gravity="0 0 -9.81"/>
  <visual>
    <headlight ambient="0.45 0.45 0.45" diffuse="0.6 0.6 0.6"/>
    <global offwidth="1280" offheight="960"/>
  </visual>
  <asset>
    <texture name="grid" type="2d" builtin="checker" rgb1="0.86 0.88 0.9"
             rgb2="0.8 0.82 0.85" width="256" height="256"/>
    <material name="floor" texture="grid" texrepeat="6 6"/>
  </asset>
  <worldbody>
    <light pos="0.3 0 1.6" dir="0 0 -1" diffuse="0.7 0.7 0.7"/>
    <geom name="floor" type="plane" pos="0 0 -0.75" size="2 2 0.1" material="floor"/>
    <geom name="table" type="box" pos="{cx} {cy} -0.02" size="0.42 0.42 0.02"
          rgba="0.93 0.94 0.95 1"/>
    <geom type="box" pos="{cx} {cy} -0.4" size="0.03 0.03 0.35" rgba="0.6 0.6 0.65 1"
          contype="0" conaffinity="0"/>
    {border}
    <geom name="nogo" type="cylinder" pos="{zx} {zy} 0.03" size="{NOGO['r'] * UNIT} 0.03"
          rgba="0.9 0.2 0.2 0.28" contype="0" conaffinity="0"/>
    <geom name="tissue" type="ellipsoid" pos="{tx} {ty} 0"
          size="{TISSUE['r'] * UNIT} {TISSUE['r'] * UNIT} {TISSUE['h']}"
          rgba="0.93 0.55 0.6 1" solref="0.03 1" solimp="0.85 0.95 0.004"/>
    <body name="target" mocap="true" pos="{cx} {cy} 0.002">
      <geom type="cylinder" size="0.008 0.001" rgba="0.22 0.55 0.9 0.9"
            contype="0" conaffinity="0"/>
    </body>
    <body name="base" pos="0 0 0">
      <geom type="cylinder" pos="0 0 {SHOULDER_Z / 2}" size="0.05 {SHOULDER_Z / 2}"
            rgba="0.35 0.38 0.42 1" contype="0" conaffinity="0"/>
      <geom name="lamp" type="sphere" pos="0 0 {SHOULDER_Z + 0.06}" size="0.025"
            rgba="0.1 0.7 0.3 1" contype="0" conaffinity="0"/>
      <body name="link1" pos="0 0 {SHOULDER_Z}">
        <joint name="j1" type="hinge" axis="0 0 1" damping="2"/>
        <geom type="capsule" fromto="0 0 0 {L1} 0 0" size="0.022" rgba="0.85 0.87 0.9 1"
              mass="1.2"/>
        <body name="link2" pos="{L1} 0 0">
          <joint name="j2" type="hinge" axis="0 0 1" damping="1.5" range="-2.9 2.9"/>
          <geom type="capsule" fromto="0 0 0 {L2} 0 0" size="0.018" rgba="0.85 0.87 0.9 1"
                mass="0.8"/>
          <body name="tool" pos="{L2} 0 0">
            <joint name="slide" type="slide" axis="0 0 -1" range="0 {slide_range}"
                   damping="20"/>
            <geom type="cylinder" fromto="0 0 0.02 0 0 -{shaft_len}" size="0.004"
                  rgba="0.3 0.32 0.35 1" mass="0.1" contype="0" conaffinity="0"/>
            <geom name="tip" type="sphere" pos="0 0 -{shaft_len}" size="{TOOL_RADIUS}"
                  rgba="0.15 0.15 0.18 1" mass="0.05"/>
          </body>
        </body>
      </body>
    </body>
  </worldbody>
  <actuator>
    <position name="a1" joint="j1" kp="60" kv="6" forcerange="-40 40"/>
    <position name="a2" joint="j2" kp="45" kv="4" forcerange="-30 30"/>
    <position name="az" joint="slide" kp="2000" kv="80" forcerange="-200 200"/>
  </actuator>
</mujoco>
"""


def slide_for_tip_height(z):
    """Slide joint position that puts the tool tip at height z above the table."""
    return TOOL_RETRACT_Z - z
