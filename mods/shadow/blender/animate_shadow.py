"""Shadow's own animations, keyframed on prepare_shadow.py's Mixamo rig after the poses and timing of
his One Must Fall 2097 sprites (FIGHTR1.AF, read with af_moves.py; FINDINGS 741-746). Each action
exports as a glTF animation of its name: the robot import replaces the base robot's sequence of that
name (Standing) and adds the others as new sequences, which Shadow's script plays (seq<Name>) or
the import poses his afterimages from (--import-ghost).

A pose is a dict per bone of either armature-space turns, children inheriting them ((axis, degrees)
or a list of them), or a direction (x, y, z) the bone points in, in armature space whatever its
parents do (a limb), or ("abs", axis, degrees), a turn from the rest whatever its parents do (a
foot). "<Side>FootAt": (offset, knee) places that ankle at an offset from the hip, in leg lengths,
the knee bending towards `knee`. Armature space: Z up, the robot faces -Y, his left is +X; (1,0,0)
+a on the spine leans forward, (0,0,1) +a on the hips turns his left side back.
"""
import math

import bpy
from mathutils import Quaternion, Vector

P = "mixamorig:"
FPS = 30

# 2097 move 11 (stance, A3-B2-C2-D2-E3...): a deep, wide lunge, side-on (chest to the camera, the left
# side leading), the front knee bent forward, the rear leg reaching back, the front claw low and
# forward, the rear claw out behind.
STANCE = {
    "Hips": ((0, 0, 1), -55), "Spine1": ((1, 0, 0), 15), "Spine2": ((0, 0, 1), -20), "Head": ((0, 0, 1), 65),
    "LeftFootAt": ((0.02, -0.36, -0.52), (0.3, -1, 0)), "RightFootAt": ((-0.02, 0.84, -0.52), (-0.3, -1, 0)),
    "LeftFoot": ("abs", (0, 0, 1), 0), "RightFoot": ("abs", (0, 0, 1), -55),
    "LeftArm": (0.3, -0.4, -0.87), "LeftForeArm": (0.05, -1, -0.12),
    "RightArm": (-0.2, 0.6, -0.78), "RightForeArm": (-0.1, 1, 0.05),
}


def mix(base, **over):
    d = dict(base)
    d.update(over)
    return d


BREATHE = mix(STANCE, Spine1=((1, 0, 0), 19), LeftFootAt=((0.02, -0.37, -0.49), (0.3, -1, 0)), RightFootAt=((-0.02, 0.85, -0.49), (-0.3, -1, 0)),
              LeftForeArm=(0.05, -1, 0.0), RightForeArm=(-0.1, 0.98, 0.18))

# 2097 moves 21 / 24 (shadow kick and punch, nbtA3-nbtB4-[spawn]B1-nbtA6): from the stance he pulls
# both claws in, thrusts them forward as the projection leaves, and holds the cast.
CAST_PULL = mix(STANCE, Hips=((0, 0, 1), -40), Spine1=((1, 0, 0), 5), Spine2=((0, 0, 1), -10), Head=((0, 0, 1), 50), LeftArm=(0.7, -0.2, -0.7), LeftForeArm=(-0.3, -0.9, 0.3),
                RightArm=(-0.7, -0.2, -0.7), RightForeArm=(0.3, -0.9, 0.3))
CAST_THRUST = mix(STANCE, Hips=((0, 0, 1), -25), Spine1=((1, 0, 0), 22), Spine2=((0, 0, 1), 0), Head=((0, 0, 1), 25),
                  LeftArm=(0.15, -1, 0.0), LeftForeArm=(0.05, -1, 0.05), RightArm=(-0.15, -1, 0.0), RightForeArm=(-0.05, -1, 0.05))

# 2097 move 16 (shadow grab, A1-[md39]A1-B1-C1-[spawn]C1-C3-B3-A4): the rear claw thrown straight up.
GRAB_CROUCH = mix(STANCE, Spine1=((1, 0, 0), 28), LeftFootAt=((0.02, -0.38, -0.45), (0.3, -1, 0)), RightFootAt=((-0.02, 0.86, -0.45), (-0.3, -1, 0)),
                  RightArm=(-0.5, 0.3, -0.8), RightForeArm=(-0.2, -0.5, -0.85))
GRAB_REACH = mix(STANCE, Hips=((0, 0, 1), -35), Spine1=((1, 0, 0), -6), Spine2=((0, 0, 1), -10), Head=[((0, 0, 1), 45), ((1, 0, 0), -20)],
                 LeftArm=(0.6, 0.1, -0.8), LeftForeArm=(0.4, -0.3, -0.85), RightArm=(-0.15, -0.2, 1), RightForeArm=(-0.05, -0.15, 1))

# 2097 move 61 (scrap, bm10...A1-B1-A4-B6-C10-[spawn]C1-C115): standing tall, legs crossed, both arms
# spread up to call the shadows.
SUMMON = {"Spine1": ((1, 0, 0), -8), "Head": ((1, 0, 0), -12),
          "LeftUpLeg": (-0.05, -0.15, -1), "LeftLeg": (-0.1, 0.05, -1), "RightUpLeg": (0.1, 0.1, -1), "RightLeg": (0.05, 0.25, -1),
          "LeftArm": (0.8, 0, 0.6), "LeftForeArm": (0.6, -0.1, 0.8), "RightArm": (-0.8, 0, 0.6), "RightForeArm": (-0.6, -0.1, 0.8)}

# Afterimage poses (one key each): the projection's sprites.
GHOSTS = {
    # 2097 move 25 A (held 40 ticks while it closes in): a long lunge, the front claw low.
    "GhostPunchLunge": mix(STANCE, Spine1=((1, 0, 0), 25), LeftUpLeg=(0.2, -0.85, -0.45), LeftLeg=(0, -0.2, -1),
                           RightUpLeg=(-0.2, 0.8, -0.55), RightLeg=(-0.1, 0.85, -0.5),
                           LeftArm=(0.2, -0.9, -0.35), LeftForeArm=(0.1, -1, -0.2), RightArm=(-0.6, 0.6, 0.5), RightForeArm=(-0.3, 0.3, 0.9)),
    # 2097 moves 25 C / 26 (the strike): upright, the rear claw driven up in an uppercut.
    "GhostPunchUppercut": mix(STANCE, Hips=((0, 0, 1), -20), Spine1=((1, 0, 0), -10), Head=[((0, 0, 1), 40), ((1, 0, 0), -15)],
                              LeftUpLeg=(0.2, -0.55, -0.8), LeftLeg=(0.05, -0.1, -1), RightUpLeg=(-0.2, 0.5, -0.85), RightLeg=(-0.1, 0.4, -0.9),
                              LeftArm=(0.5, 0.6, -0.6), LeftForeArm=(0.3, 0.2, -0.9), RightArm=(-0.1, -0.5, 0.85), RightForeArm=(0, -0.3, 1)),
    # 2097 move 22 B (held 40 ticks as it flies): a flying side kick, the front leg straight out.
    "GhostKick": {"Hips": ((0, 0, 1), 70), "Spine1": ((1, 0, 0), -20), "Head": ((0, 0, 1), -50),
                  "LeftUpLeg": (0.05, -1, 0.05), "LeftLeg": (0, -1, 0.1), "RightUpLeg": (0, -0.6, -0.8), "RightLeg": (0, 0.8, -0.6),
                  "LeftArm": (0.5, 0.6, 0.3), "LeftForeArm": (0.3, 0.3, 0.9), "RightArm": (-0.7, 0.3, 0.4), "RightForeArm": (-0.5, -0.5, 0.7)},
    # 2097 move 37 / 52 A: leaping up, the claw raised, knees drawn in.
    "GhostGrabLeap": {"Spine1": ((1, 0, 0), -10), "Head": ((1, 0, 0), -25),
                      "LeftUpLeg": (0.2, -0.9, -0.1), "LeftLeg": (0.1, 0.2, -1), "RightUpLeg": (-0.2, -0.6, -0.6), "RightLeg": (-0.1, 0.5, -0.85),
                      "LeftArm": (0.7, -0.2, 0.6), "LeftForeArm": (0.5, -0.4, 0.75), "RightArm": (-0.1, -0.2, 1), "RightForeArm": (0, -0.1, 1)},
    # 2097 moves 40 C / 50 C (the hold): a lunge, both claws locked forward.
    "GhostGrabHold": mix(STANCE, Hips=((0, 0, 1), -25), Spine1=((1, 0, 0), 20), Head=((0, 0, 1), 45),
                         LeftArm=(0.15, -1, 0.1), LeftForeArm=(0, -1, 0.1), RightArm=(-0.15, -1, 0.1), RightForeArm=(0, -1, 0.1)),
}

# name: [(time 0..1, pose, ground: True plants the lower foot, else a fixed lift in metres)], frames
ACTIONS = {
    "Standing": ([(0.0, STANCE, True), (0.5, BREATHE, True), (1.0, STANCE, True)], 90),
    "ShadowCast": ([(0.0, STANCE, True), (0.22, CAST_PULL, True), (0.40, CAST_THRUST, True), (0.78, CAST_THRUST, True), (1.0, STANCE, True)], 42),
    "ShadowGrabCast": ([(0.0, STANCE, True), (0.25, GRAB_CROUCH, True), (0.45, GRAB_REACH, True), (0.80, GRAB_REACH, True), (1.0, STANCE, True)], 40),
    "ShadowSummon": ([(0.0, STANCE, True), (0.18, SUMMON, True), (0.88, SUMMON, True), (1.0, STANCE, True)], 80),
}
for _name, _pose in GHOSTS.items():
    ACTIONS[_name] = ([(0.0, _pose, _name not in ("GhostKick", "GhostGrabLeap")), (1.0, _pose, _name not in ("GhostKick", "GhostGrabLeap"))], 10)


def reach(ao, turns):
    """Expands each "<Side>FootAt" into its thigh's and shin's directions (two-bone IK)."""
    turns = dict(turns)
    for s in ("Left", "Right"):
        at = turns.pop(s + "FootAt", None)
        if at is None or s + "UpLeg" in turns or s + "Leg" in turns:
            continue
        up, low = ao.data.bones[P + s + "UpLeg"], ao.data.bones[P + s + "Leg"]
        l1, l2 = up.length, low.length
        T = Vector(at[0]) * (l1 + l2)
        d = min(T.length, 0.999 * (l1 + l2))
        u = T.normalized()
        v = Vector(at[1]) - Vector(at[1]).project(u)
        v.normalize()
        a = math.acos(max(-1.0, min(1.0, (l1 * l1 + d * d - l2 * l2) / (2 * l1 * d))))
        thigh = u * math.cos(a) + v * math.sin(a)
        turns[s + "UpLeg"] = tuple(thigh)
        turns[s + "Leg"] = tuple((u * d - thigh * l1).normalized())
    return turns


def apply(ao, turns):
    turns = reach(ao, turns)
    world = {}
    for pb in sorted(ao.pose.bones, key=lambda pb: len(pb.bone.parent_recursive)):
        b = pb.bone
        parent = world.get(b.parent.name, Quaternion()) if b.parent else Quaternion()
        t = turns.get(b.name[len(P):])
        D = parent
        if isinstance(t, tuple) and len(t) == 3 and all(isinstance(x, (int, float)) for x in t):
            D = (b.tail_local - b.head_local).normalized().rotation_difference(Vector(t).normalized())
            t = None
        elif isinstance(t, tuple) and t[0] == "abs":
            D = Quaternion(t[1], math.radians(t[2]))
            t = None
        for axis, deg in ([t] if isinstance(t, tuple) else t or []):
            D = Quaternion(axis, math.radians(deg)) @ D
        world[b.name] = D
        R = b.matrix_local.to_quaternion()
        pb.rotation_mode = "QUATERNION"
        pb.rotation_quaternion = R.inverted() @ (parent.inverted() @ D) @ R
        pb.location = (0, 0, 0)
    bpy.context.view_layer.update()


def foot_low(ao):
    return min(min(ao.pose.bones[P + s + n].head.z for n in ("Foot", "ToeBase")) for s in ("Left", "Right"))


def build(ao):
    """Keyframes every action on the armature; returns their names."""
    apply(ao, {})
    rest_low = foot_low(ao)
    ao.animation_data_create()
    made = []
    for name, (keys, frames) in ACTIONS.items():
        act = bpy.data.actions.new(name)
        act.use_fake_user = True
        ao.animation_data.action = act
        for t, turns, ground in keys:
            apply(ao, turns)
            hips = ao.pose.bones[P + "Hips"]
            # The hips bone points up, so its local Y is the armature's Z.
            hips.location = (0, (rest_low - foot_low(ao)) if ground is True else 0.25, 0)
            f = 1 + round(t * (frames - 1))
            for pb in ao.pose.bones:
                pb.keyframe_insert("rotation_quaternion", frame=f)
            hips.keyframe_insert("location", frame=f)
        track = ao.animation_data.nla_tracks.new()
        track.name = name
        track.strips.new(name, 1, act)
        track.mute = True
        made.append(name)
    ao.animation_data.action = None
    apply(ao, {})
    return made
