"""Build Shadow, the One Must Fall 2097 robot, as a low-poly skinned glTF for the editor's robot import.

blender -b --factory-startup --python make_shadow.py -- --out shadow.glb [--blend shadow.blend]
    [--preview prefix] [--paint-out dir] [--picture choose_creature.png]

Original geometry and procedural textures (no game assets): a slim silver robot with a boxy, angled
chest plate, a crested helmet, ribbed limbs, glowing cyan studs and spikes. Mixamo bone names in an
A-pose, rigid weights per armour part. The material's base colour is the robot as it looks; images
named PaintMask (the pilot's three colours, R G B) and ReflectionMask go to the import as such.
"""
import argparse
import math
import os
import sys

import bpy
import numpy as np
from mathutils import Matrix, Quaternion, Vector

SIDES = (("Left", 1.0), ("Right", -1.0))
P = "mixamorig:"
ATLAS = 1024
SCALE = 1.15  # modelled at 2.7 m; Katana and Jaguar stand about 3 m


def V(*a):
    return Vector(a)


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    p = argparse.ArgumentParser()
    p.add_argument("--out", required=True)
    p.add_argument("--blend", default=None)
    p.add_argument("--preview", default=None, help="PNG path prefix for turntable and pose renders")
    p.add_argument("--paint-out", default=None, help="directory for the texture and a second paint job as PNG")
    p.add_argument("--picture", default=None, help="Choose Creature's picture, a 512 x 128 PNG")
    return p.parse_args(argv)


# ---------------------------------------------------------------- skeleton (metres, Z up, facing -Y)

ARM_DROP = math.radians(50)


def joints():
    J = {"hips": V(0, 0, 1.50), "spine": V(0, 0, 1.60), "spine1": V(0, 0, 1.72), "spine2": V(0, 0, 1.90),
         "neck": V(0, 0.0, 2.32), "head": V(0, -0.005, 2.41), "top": V(0, -0.005, 2.66)}
    for s, sx in SIDES:
        d = V(sx * math.cos(ARM_DROP), 0, -math.sin(ARM_DROP))
        S = V(0.40 * sx, 0.0, 2.19)
        E = S + d * 0.47
        W = E + d * 0.43
        J[s] = dict(C=V(0.07 * sx, 0, 2.17), S=S, E=E, W=W, K=W + d * 0.19, d=d,
                    H=V(0.135 * sx, 0, 1.44), N=V(0.16 * sx, -0.02, 0.80), A=V(0.175 * sx, 0.02, 0.15),
                    B=V(0.175 * sx, -0.16, 0.05), T=V(0.175 * sx, -0.30, 0.04))
    return J


def skeleton(J):
    fwd, up = V(0, -1, 0), V(0, 0, 1)
    b = [(P + "Hips", J["hips"], J["spine"], None, fwd),
         (P + "Spine", J["spine"], J["spine1"], P + "Hips", fwd),
         (P + "Spine1", J["spine1"], J["spine2"], P + "Spine", fwd),
         (P + "Spine2", J["spine2"], J["neck"], P + "Spine1", fwd),
         (P + "Neck", J["neck"], J["head"], P + "Spine2", fwd),
         (P + "Head", J["head"], J["top"], P + "Neck", fwd),
         (P + "HeadTop_End", J["top"], J["top"] + V(0, 0, 0.08), P + "Head", fwd)]
    for s, _ in SIDES:
        j, n = J[s], P + s
        b += [(n + "Shoulder", j["C"], j["S"], P + "Spine2", up),
              (n + "Arm", j["S"], j["E"], n + "Shoulder", V(0, 1, 0)),
              (n + "ForeArm", j["E"], j["W"], n + "Arm", V(0, 1, 0)),
              (n + "Hand", j["W"], j["K"], n + "ForeArm", V(0, -1, 0)),
              (n + "UpLeg", j["H"], j["N"], P + "Hips", fwd),
              (n + "Leg", j["N"], j["A"], n + "UpLeg", fwd),
              (n + "Foot", j["A"], j["B"], n + "Leg", up),
              (n + "ToeBase", j["B"], j["T"], n + "Foot", up),
              (n + "Toe_End", j["T"], j["T"] + V(0, -0.05, 0), n + "ToeBase", up)]
    return b


# ---------------------------------------------------------------- geometry

class Mesh:
    """Triangles and quads with per-corner UVs, a style and a bone per part."""

    def __init__(self):
        self.verts, self.faces, self.uvs, self.part = [], [], [], []
        self.parts = []  # (name, style, bone, rect key)

    def add_part(self, name, style, bone, key=None):
        key = key or name.replace("_Left", "").replace("_Right", "")
        self.parts.append((name, style, bone, key))
        return len(self.parts) - 1

    def add(self, pid, verts, faces, uvs):
        base = len(self.verts)
        self.verts += [Vector(v) for v in verts]
        for f, fu in zip(faces, uvs):
            self.faces.append(tuple(base + i for i in f))
            self.uvs.append(list(fu))
            self.part.append(pid)


def frame_of(axis, ref):
    w = axis.normalized()
    u = (ref - w * ref.dot(w)).normalized()
    return u, w.cross(u), w


def loft(rings, cap0=True, cap1=True, closed=True):
    """rings: [(centre, u, v, [(x, y), ...])] with the same point count. Returns verts, faces, uvs
    (s along the profile, t along the rings, both 0..1)."""
    n = len(rings[0][3])
    verts, faces, uvs = [], [], []
    for c, u, v, prof in rings:
        verts += [c + u * x + v * y for x, y in prof]
    seg = n if closed else n - 1
    prof0 = [Vector((x, y)) for x, y in rings[len(rings) // 2][3]]
    cum = [0.0]
    for k in range(seg):
        cum.append(cum[-1] + (prof0[(k + 1) % n] - prof0[k]).length)
    s_of = [c / cum[-1] for c in cum]
    tl = [0.0]
    for i in range(1, len(rings)):
        tl.append(tl[-1] + (rings[i][0] - rings[i - 1][0]).length + 1e-6)
    t_of = [t / tl[-1] for t in tl]
    for i in range(len(rings) - 1):
        for k in range(seg):
            a, b = i * n + k, i * n + (k + 1) % n
            faces.append((a, b, b + n, a + n))
            uvs.append(((s_of[k], t_of[i]), (s_of[k + 1], t_of[i]), (s_of[k + 1], t_of[i + 1]), (s_of[k], t_of[i + 1])))
    for cap, ri, rev in ((cap0, 0, True), (cap1, len(rings) - 1, False)):
        if not cap or not closed:
            continue
        ci = len(verts)
        verts.append(rings[ri][0])
        tv = 0.0 if ri == 0 else 1.0
        for k in range(n):
            a, b = ri * n + k, ri * n + (k + 1) % n
            faces.append((ci, b, a) if rev else (ci, a, b))
            uvs.append(((0.5, tv), (s_of[k + 1], tv), (s_of[k], tv)) if rev else ((0.5, tv), (s_of[k], tv), (s_of[k + 1], tv)))
    return verts, faces, uvs


def poly(n, rx, ry, phase=0.0):
    return [(rx * math.cos(phase + 2 * math.pi * k / n), ry * math.sin(phase + 2 * math.pi * k / n)) for k in range(n)]


def ribbed(a, b, ref, ribs, r_out, r_in, sides=6, gap=0.35, taper=1.0):
    """A limb of `ribs` rings between a and b: each rib a bevelled band, grooves between."""
    u, v, w = frame_of(b - a, ref)
    L = (b - a).length
    rings = []
    for i in range(ribs):
        t0, t1 = i / ribs, (i + 1) / ribs
        k = 1.0 + (taper - 1.0) * (i + 0.5) / ribs
        g = (t1 - t0) * gap * 0.5
        for t, r in ((t0 + g * 0.2, r_in), (t0 + g, r_out), (t1 - g, r_out), (t1 - g * 0.2, r_in)):
            rings.append((a + w * (L * t), u, v, poly(sides, r * k, r * k, math.pi / sides)))
    return loft(rings, cap0=False, cap1=False)


def box_loft(a, b, ref, sections):
    """sections: [(t, half_width, half_depth, bevel)] -> octagonal (bevelled box) rings."""
    u, v, w = frame_of(b - a, ref)
    L = (b - a).length
    rings = []
    for t, hw, hd, bv in sections:
        e = min(bv, hw * 0.9, hd * 0.9)
        prof = [(hw, -hd + e), (hw, hd - e), (hw - e, hd), (-hw + e, hd), (-hw, hd - e), (-hw, -hd + e),
                (-hw + e, -hd), (hw - e, -hd)]
        rings.append((a + w * (L * t), u, v, prof))
    return loft(rings)


def spike(base, tip, r, sides=4, ref=V(0, 0, 1)):
    u, v, w = frame_of(tip - base, ref if abs((tip - base).normalized().dot(ref)) < 0.95 else V(1, 0, 0))
    verts = [base + (u * math.cos(2 * math.pi * k / sides + math.pi / sides) + v * math.sin(2 * math.pi * k / sides + math.pi / sides)) * r
             for k in range(sides)] + [tip]
    faces, uvs = [], []
    # Open underneath: the base sits inside the part it grows from.
    for k in range(sides):
        faces.append((k, (k + 1) % sides, sides))
        uvs.append(((k / sides, 0.2), ((k + 1) / sides, 0.2), ((k + 0.5) / sides, 1.0)))
    return verts, faces, uvs


def stud(c, normal, r, h=None):
    r *= 1.3
    return spike(c - normal * r * 0.5, c + normal * (h if h is not None else r * 2.0), r, 4)


def fin(points, thick, side):
    """A flat blade: a polygon (counter-clockwise, in 3D) extruded by `thick` along `side`."""
    n = len(points)
    half = side.normalized() * thick * 0.5
    verts = [p + half for p in points] + [p - half for p in points]
    cen = sum(points, Vector()) / n
    verts += [cen + half * 1.6, cen - half * 1.6]
    faces, uvs = [], []
    mn = Vector((min(p.x for p in points), min(p.y for p in points), min(p.z for p in points)))
    ext = max(max(p.x for p in points) - mn.x, max(p.y for p in points) - mn.y, max(p.z for p in points) - mn.z, 1e-6)
    def uv(p):
        q = (p - mn) / ext
        return (min(1, max(0, q.y if abs(side.x) > 0.5 else q.x)), min(1, max(0, q.z)))
    for k in range(n):
        k2 = (k + 1) % n
        faces.append((k, k2, 2 * n))
        uvs.append((uv(points[k]), uv(points[k2]), uv(cen)))
        faces.append((n + k2, n + k, 2 * n + 1))
        uvs.append((uv(points[k2]), uv(points[k]), uv(cen)))
        faces.append((k2, k, n + k, n + k2))
        uvs.append(((0.0, 0.5), (1.0, 0.5), (1.0, 0.55), (0.0, 0.55)))
    return verts, faces, uvs


# ---------------------------------------------------------------- the robot

def build_shadow(J):
    M = Mesh()
    X, Y, Z = V(1, 0, 0), V(0, -1, 0), V(0, 0, 1)

    # Helmet: rounded, narrow face, swept-back crest.
    pid = M.add_part("helmet", "armour", "Head")
    rings = []
    for z, rx, ry, cy in ((2.36, 0.065, 0.075, 0.0), (2.40, 0.10, 0.12, 0.005), (2.47, 0.114, 0.136, 0.01),
                          (2.54, 0.11, 0.134, 0.015), (2.60, 0.09, 0.114, 0.025), (2.65, 0.052, 0.072, 0.035)):
        prof = [(x * 1.12, y * 1.12 * (0.82 if y < 0 else 1.0)) for x, y in poly(8, rx, ry, -math.pi / 2)]
        rings.append((V(0, cy, z), X, -Y, prof))
    M.add(pid, *loft(rings, cap0=True, cap1=True))
    pid = M.add_part("visor", "visor", "Head")
    rings = [(V(0, -0.118 - 0.01 * i, z), X, -Y, [(x * k, y) for x, y in poly(6, 0.08, 0.022)])
             for i, (z, k) in enumerate(((2.46, 0.75), (2.50, 1.0), (2.535, 0.85)))]
    M.add(pid, *loft(rings))
    pid = M.add_part("crest", "armour", "Head")
    M.add(pid, *fin([V(0, -0.09, 2.62), V(0, 0.02, 2.70), V(0, 0.20, 2.75), V(0, 0.40, 2.70), V(0, 0.52, 2.57),
                     V(0, 0.27, 2.555), V(0, 0.11, 2.46)], 0.06, X))
    pid = M.add_part("crest_glow", "glow", "Head", "glow")
    M.add(pid, *stud(V(0, -0.045, 2.625), V(0, -0.6, 0.8).normalized(), 0.018))

    # Neck: three ribs.
    pid = M.add_part("neck", "ribs", "Neck", "ribs_neck")
    M.add(pid, *ribbed(V(0, 0.0, 2.27), V(0, 0, 2.39), X, 2, 0.050, 0.034, 6))

    # Chest: an angled, inverted-trapezoid plate, keel at the front.
    pid = M.add_part("chest", "armour", "Spine2")
    rings = []
    for z, hw, hd, keel in ((1.74, 0.11, 0.09, 0.02), (1.88, 0.24, 0.13, 0.04), (2.06, 0.41, 0.16, 0.06),
                            (2.21, 0.46, 0.15, 0.05), (2.29, 0.40, 0.11, 0.02)):
        prof = [(hw, 0.0), (hw * 0.72, -hd), (0.0, -hd - keel), (-hw * 0.72, -hd), (-hw, 0.0), (-hw * 0.7, hd * 0.85),
                (0.0, hd), (hw * 0.7, hd * 0.85)]
        rings.append((V(0, 0.01, z), X, -Y, prof))
    M.add(pid, *loft(rings))
    pid = M.add_part("chest_studs", "glow", "Spine2", "glow")
    for t in (0.0, 0.33, 0.66, 1.0):
        for sx in (1, -1):
            p = V(sx * (0.08 + 0.26 * t), -0.17 - 0.05 * (1 - abs(t - 0.5) * 2) + 0.03 * t, 1.88 + 0.32 * t)
            M.add(pid, *stud(p, V(sx * 0.3, -1, 0.15).normalized(), 0.02))
    M.add(pid, *stud(V(0, -0.215, 2.06), V(0, -1, 0), 0.024))
    M.add(pid, *stud(V(0, -0.175, 1.85), V(0, -1, 0), 0.02))

    # Waist: ribs with a glowing belt, then the flared hip plate.
    pid = M.add_part("waist", "ribs", "Spine", "ribs_waist")
    M.add(pid, *ribbed(V(0, 0.01, 1.57), V(0, 0.01, 1.75), X, 2, 0.105, 0.08, 6))
    pid = M.add_part("belt", "glow", "Hips", "glow")
    rings = [(V(0, 0.01, z), X, -Y, poly(8, r, r * 0.8, math.pi / 8)) for z, r in ((1.565, 0.13), (1.585, 0.14), (1.605, 0.13))]
    M.add(pid, *loft(rings, cap0=False, cap1=False))
    pid = M.add_part("pelvis", "armour", "Hips")
    rings = []
    for z, hw, hd in ((1.60, 0.14, 0.10), (1.53, 0.19, 0.12), (1.45, 0.205, 0.12), (1.38, 0.09, 0.08)):
        prof = [(hw, 0.0), (hw * 0.75, -hd), (0.0, -hd * 1.15), (-hw * 0.75, -hd), (-hw, 0.0), (-hw * 0.75, hd),
                (0.0, hd * 1.05), (hw * 0.75, hd)]
        rings.append((V(0, 0.01, z), X, -Y, prof))
    M.add(pid, *loft(rings))
    pid = M.add_part("pelvis_studs", "glow", "Hips", "glow")
    for sx in (1, -1):
        M.add(pid, *stud(V(sx * 0.17, -0.135, 1.47), V(sx * 0.4, -1, 0).normalized(), 0.02))

    for s, sx in SIDES:
        j = J[s]
        d = j["d"]
        side_out = V(sx, 0, 0)
        # Shoulder: an angular pauldron with a stud and an upswept spike.
        pid = M.add_part("shoulder_" + s, "armour", s + "Arm")
        sh = j["S"] + d * 0.02
        M.add(pid, *box_loft(sh - V(0, 0, 0.0) - d * 0.05 + V(0, 0, 0.07), sh + d * 0.13 - V(0, 0, 0.02), Y,
                             [(0.0, 0.10, 0.12, 0.04), (0.45, 0.115, 0.135, 0.05), (1.0, 0.085, 0.10, 0.04)]))
        pid = M.add_part("shoulder_glow_" + s, "glow", s + "Arm", "glow")
        top = sh + V(0, 0, 0.085) + d * 0.04
        M.add(pid, *spike(top, top + V(sx * 0.13, 0.03, 0.12), 0.025))
        M.add(pid, *stud(sh + d * 0.06 + V(sx * 0.11, -0.03, 0.03), V(sx, -0.3, 0.3).normalized(), 0.02))
        M.add(pid, *stud(sh + d * 0.06 + V(0, -0.13, 0.03), V(0, -1, 0.2).normalized(), 0.018))

        # Upper arm: ribs, elbow cap with a stud.
        pid = M.add_part("upperarm_" + s, "ribs", s + "Arm", "ribs_arm")
        M.add(pid, *ribbed(j["S"] + d * 0.10, j["E"] - d * 0.02, Y, 4, 0.062, 0.044, 6, taper=0.9))
        pid = M.add_part("elbow_" + s, "armour", s + "ForeArm")
        M.add(pid, *box_loft(j["E"] - d * 0.05, j["E"] + d * 0.08, Y,
                             [(0.0, 0.05, 0.05, 0.02), (0.4, 0.066, 0.07, 0.03), (1.0, 0.055, 0.055, 0.02)]))
        pid = M.add_part("elbow_glow_" + s, "glow", s + "ForeArm", "glow")
        M.add(pid, *stud(j["E"] + V(0, 0.07, 0) + d * 0.01, V(0, 1, 0), 0.02, 0.05))

        # Forearm: ribs into an angular gauntlet; spikes out of its back and outside.
        pid = M.add_part("forearm_" + s, "ribs", s + "ForeArm", "ribs_arm")
        M.add(pid, *ribbed(j["E"] + d * 0.08, j["E"] + d * 0.24, Y, 2, 0.055, 0.040, 6))
        pid = M.add_part("gauntlet_" + s, "armour", s + "ForeArm")
        M.add(pid, *box_loft(j["E"] + d * 0.24, j["W"] + d * 0.01, Y,
                             [(0.0, 0.050, 0.055, 0.02), (0.35, 0.068, 0.072, 0.03), (1.0, 0.052, 0.058, 0.02)]))
        pid = M.add_part("gauntlet_glow_" + s, "glow", s + "ForeArm", "glow")
        g = j["E"] + d * 0.30
        out = (V(sx, 0, 0) - d * d.x * sx).normalized()
        M.add(pid, *spike(g + V(0, 0.06, 0), g + V(0, 0.20, 0.03) - d * 0.06, 0.022))
        M.add(pid, *spike(g + d * 0.08 + V(0, 0.06, 0), g + d * 0.03 + V(0, 0.17, 0.02), 0.018))
        M.add(pid, *spike(g + d * 0.06 + out * 0.06, g + d * 0.02 + out * 0.15 + V(0, 0, 0.02), 0.018))

        # Hand: a blocky palm with glowing claws.
        pid = M.add_part("hand_" + s, "armour", s + "Hand")
        M.add(pid, *box_loft(j["W"], j["W"] + d * 0.11, V(0, -1, 0),
                             [(0.0, 0.035, 0.05, 0.012), (0.6, 0.032, 0.06, 0.012), (1.0, 0.025, 0.05, 0.01)]))
        pid = M.add_part("claws_" + s, "glow", s + "Hand", "glow")
        tip = j["W"] + d * 0.11
        for k, dy in enumerate((-0.035, 0.0, 0.035)):
            M.add(pid, *spike(tip + V(0, dy, 0), tip + d * (0.12 if k == 1 else 0.10) + V(0, dy * 1.5, 0), 0.012))
        M.add(pid, *spike(j["W"] + d * 0.05 + V(0, -0.055, 0), j["W"] + d * 0.11 + V(0, -0.13, -0.01), 0.012))

        # Thigh plate, knee cap, ribbed shin, angular foot with fins.
        pid = M.add_part("thigh_" + s, "armour", s + "UpLeg")
        M.add(pid, *box_loft(j["H"] - V(0, 0, 0.01), j["N"] + (j["H"] - j["N"]).normalized() * 0.07, X,
                             [(0.0, 0.095, 0.10, 0.03), (0.3, 0.10, 0.105, 0.035), (1.0, 0.058, 0.065, 0.02)]))
        pid = M.add_part("knee_" + s, "armour", s + "Leg")
        M.add(pid, *box_loft(j["N"] + V(0, 0, 0.06), j["N"] - V(0, 0, 0.11), X,
                             [(0.0, 0.065, 0.06, 0.02), (0.4, 0.075, 0.085, 0.03), (1.0, 0.055, 0.06, 0.02)]))
        pid = M.add_part("knee_glow_" + s, "glow", s + "Leg", "glow")
        M.add(pid, *stud(j["N"] + V(0, -0.085, -0.02), V(0, -1, 0.1).normalized(), 0.022, 0.05))
        M.add(pid, *stud(j["N"] + V(sx * 0.075, -0.02, -0.03), V(sx, -0.2, 0).normalized(), 0.016))
        pid = M.add_part("shin_" + s, "ribs", s + "Leg", "ribs_leg")
        M.add(pid, *ribbed(j["N"] - V(0, 0, 0.10), j["A"] + V(0, 0, 0.06), X, 6, 0.058, 0.040, 6, taper=0.85))
        pid = M.add_part("ankle_" + s, "armour", s + "Leg")
        M.add(pid, *box_loft(j["A"] + V(0, 0, 0.08), j["A"] - V(0, 0, 0.02), X,
                             [(0.0, 0.045, 0.045, 0.015), (1.0, 0.055, 0.06, 0.02)]))
        pid = M.add_part("foot_" + s, "armour", s + "Foot")
        ax = j["A"].x
        M.add(pid, *box_loft(V(ax, 0.10, 0.07), V(ax, -0.16, 0.05), Z,
                             [(0.0, 0.045, 0.06, 0.02), (0.35, 0.06, 0.07, 0.025), (1.0, 0.055, 0.045, 0.02)]))
        pid = M.add_part("toe_" + s, "armour", s + "ToeBase")
        M.add(pid, *box_loft(V(ax, -0.16, 0.045), V(ax, -0.27, 0.03), Z,
                             [(0.0, 0.05, 0.04, 0.015), (1.0, 0.035, 0.025, 0.01)]))
        pid = M.add_part("foot_glow_" + s, "glow", s + "ToeBase", "glow")
        for dx in (-0.03, 0.03):
            M.add(pid, *spike(V(ax + dx, -0.25, 0.03), V(ax + dx * 1.8, -0.37, 0.012), 0.016))
        pid = M.add_part("heel_glow_" + s, "glow", s + "Foot", "glow")
        M.add(pid, *spike(V(ax, 0.09, 0.05), V(ax, 0.20, 0.015), 0.018))
        M.add(pid, *spike(V(ax + sx * 0.05, 0.0, 0.12), V(ax + sx * 0.15, 0.03, 0.10), 0.016))
    return M


# ---------------------------------------------------------------- textures

BIG = {"helmet", "chest", "pelvis", "thigh", "shoulder", "gauntlet", "foot", "ribs_leg", "ribs_arm"}


def pack(items, n):
    occ, out = np.zeros((n, n), bool), {}
    for name, s in sorted(items, key=lambda i: -i[1]):
        spot = next(((r, c) for r in range(n - s + 1) for c in range(n - s + 1) if not occ[r:r + s, c:c + s].any()), None)
        if spot is None:
            raise RuntimeError("atlas full")
        occ[spot[0]:spot[0] + s, spot[1]:spot[1] + s] = True
        out[name] = (spot[1] / n, spot[0] / n, s / n)
    return out


def paint_rect(style, key, s, t):
    """The colour of texel coordinates s, t in 0..1 of a part's rectangle."""
    one = np.ones_like(s)
    if style == "armour":
        lum = 0.80 + 0.12 * t - 0.10 * (np.abs(s - 0.5) * 2) ** 4
        line = ((np.abs(((s * 4) % 1) - 0.5) > 0.47) | (np.abs(t - 0.5) > 0.47)).astype(float)
        panel = ((np.abs(((s * 8) % 1) - 0.5) < 0.04) & (t > 0.25) & (t < 0.75)).astype(float)
        lum = lum * (1 - 0.45 * line) * (1 - 0.25 * panel)
        trim = ((t > 0.08) & (t < 0.12)).astype(float)[..., None]
        look = np.stack([lum * 0.86, lum * 0.89, lum * 0.95], -1)
        return look * (1 - trim) + trim * np.array([0.42, 0.46, 0.55]) * lum[..., None]
    if style == "ribs":
        ribs = {"ribs_leg": 6, "ribs_arm": 4, "ribs_neck": 2, "ribs_waist": 2}[key]
        ph = (t * ribs) % 1
        band = (ph > 0.12) & (ph < 0.88)
        edge = np.minimum(ph, 1 - ph)
        lum = np.where(band, 0.92 - 0.25 * np.abs(ph - 0.5), 0.08 + 0.3 * edge)
        return np.stack([lum * 0.9, lum * 0.92, lum * 0.97], -1)
    if style == "visor":
        look = np.zeros(s.shape + (3,)) + np.array([0.03, 0.035, 0.05])
        eye = (np.abs(t - 0.5) < 0.18) & (np.abs(np.abs(s - 0.5) - 0.22) < 0.09)
        look[eye] = (0.35, 0.95, 1.0)
        return look
    core = np.clip(t, 0, 1) ** 2
    return np.stack([0.10 + 0.75 * core, 0.75 + 0.25 * core, 1.0 * one], -1)


def make_images(M):
    keys = {}
    for name, style, bone, key in M.parts:
        keys.setdefault(key, style)
    rects = pack([(k, 2 if k in BIG else 1) for k in keys], 8)
    look = np.zeros((ATLAS, ATLAS, 3), np.float32)
    look[...] = (0.08, 0.09, 0.11)
    pad = 6 / ATLAS
    for key, style in keys.items():
        x0, y0, wh = rects[key]
        px0, py0, pw = int(round(x0 * ATLAS)), int(round(y0 * ATLAS)), int(round(wh * ATLAS))
        xs = ((np.arange(px0, px0 + pw) + 0.5) / ATLAS - x0 - pad) / (wh - 2 * pad)
        ys = ((np.arange(py0, py0 + pw) + 0.5) / ATLAS - y0 - pad) / (wh - 2 * pad)
        s, t = np.meshgrid(np.clip(xs, 0, 1), np.clip(ys, 0, 1))
        look[py0:py0 + pw, px0:px0 + pw] = paint_rect(style, key, s, t)
    return rects, np.clip(look, 0, 1)


def to_image(name, rgb):
    h, w = rgb.shape[:2]
    buf = np.ones((h, w, 4), np.float32)
    buf[..., :3] = rgb if rgb.ndim == 3 else rgb[..., None]
    img = bpy.data.images.new(name, w, h, alpha=False)
    img.colorspace_settings.name = "sRGB" if name == "ShadowColours" else "Non-Color"
    # Blender's pixel rows run bottom-up; the atlas is laid out top-down like glTF UVs.
    img.pixels.foreach_set(buf[::-1].ravel())
    img.file_format = "PNG"
    img.pack()
    return img


#   name: (paint: -1 own colours, 0-2 the pilot's colour; metallic, roughness, glows)
MATERIALS = {
    "Armour": (0, 0.85, 0.30, False),
    "Ribs": (0, 0.90, 0.25, False),
    "Accent": (1, 0.85, 0.30, False),
    "Claws": (2, 0.60, 0.40, False),
    "Glow": (-1, 0.0, 0.50, True),
    "Visor": (-1, 0.9, 0.10, True),
}
ACCENT = {"crest", "elbow", "knee", "ankle"}


def material_of(part_name, style):
    base = part_name.replace("_Left", "").replace("_Right", "")
    if style == "glow":
        return "Glow"
    if style == "visor":
        return "Visor"
    if style == "ribs":
        return "Ribs"
    if base in ACCENT:
        return "Accent"
    if base == "hand":
        return "Claws"
    return "Armour"


def make_materials(look):
    """One material per paint choice, all sampling the one texture atlas. The editor's robot import
    reads each material's custom property de_paint (glTF extras) and metallic (the reflection)."""
    img = to_image("ShadowColours", look)
    out = []
    for name, (paint, metal, rough, glows) in MATERIALS.items():
        m = bpy.data.materials.new(name)
        m.use_nodes = True
        m["de_paint"] = paint
        nt = m.node_tree
        bsdf = nt.nodes["Principled BSDF"]
        tex = nt.nodes.new("ShaderNodeTexImage")
        tex.image = img
        nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
        bsdf.inputs["Metallic"].default_value = metal
        bsdf.inputs["Roughness"].default_value = rough
        if glows:
            nt.links.new(tex.outputs["Color"], bsdf.inputs["Emission Color"])
            bsdf.inputs["Emission Strength"].default_value = 3.0
        out.append(m)
    return out


# ---------------------------------------------------------------- build

def build(args):
    J = joints()
    bones = [(n, h * SCALE, t * SCALE, parent, roll) for n, h, t, parent, roll in skeleton(J)]
    M = build_shadow(J)
    M.verts = [v * SCALE for v in M.verts]
    rects, look = make_images(M)
    mats = make_materials(look)
    names = list(MATERIALS)
    pad = 6 / ATLAS
    uvs = []
    for f, fu, pid in zip(M.faces, M.uvs, M.part):
        x0, y0, wh = rects[M.parts[pid][3]]
        uvs.extend((x0 + pad + u * (wh - 2 * pad), 1.0 - (y0 + pad + v * (wh - 2 * pad))) for u, v in fu)
    me = bpy.data.meshes.new("Shadow")
    me.from_pydata([tuple(v) for v in M.verts], [], M.faces)
    me.validate(clean_customdata=False)
    me.update()
    uvl = me.uv_layers.new(name="UVMap")
    uvl.data.foreach_set("uv", np.array(uvs, np.float32).ravel())
    for m in mats:
        me.materials.append(m)
    me.polygons.foreach_set("material_index", [names.index(material_of(M.parts[pid][0], M.parts[pid][1])) for pid in M.part])
    mo = bpy.data.objects.new("Shadow", me)
    bpy.context.scene.collection.objects.link(mo)
    bpy.context.view_layer.objects.active = mo
    mo.select_set(True)
    bpy.ops.object.shade_auto_smooth(angle=math.radians(35))
    bpy.ops.object.modifier_apply(modifier=mo.modifiers[-1].name) if mo.modifiers else None

    arm = bpy.data.armatures.new("Armature")
    ao = bpy.data.objects.new("Armature", arm)
    bpy.context.scene.collection.objects.link(ao)
    bpy.context.view_layer.objects.active = ao
    bpy.ops.object.mode_set(mode="EDIT")
    for name, h, t, parent, roll in bones:
        eb = arm.edit_bones.new(name)
        eb.head, eb.tail = h, t
        eb.align_roll(roll)
        if parent:
            eb.parent = arm.edit_bones[parent]
            eb.use_connect = (eb.parent.tail - h).length < 1e-6
    bpy.ops.object.mode_set(mode="OBJECT")

    groups = {b[0]: mo.vertex_groups.new(name=b[0]) for b in bones}
    vert_part = {}
    for f, pid in zip(M.faces, M.part):
        for i in f:
            vert_part[i] = pid
    for i, pid in vert_part.items():
        groups[P + M.parts[pid][2]].add([i], 1.0, "REPLACE")
    mo.parent = ao
    mod = mo.modifiers.new("Armature", "ARMATURE")
    mod.object = ao
    tris = sum(len(f) - 2 for f in M.faces)
    vs = np.array([tuple(v) for v in M.verts])
    print("SHADOW parts=%d verts=%d tris=%d height=%.3f width=%.3f" % (len(M.parts), len(M.verts), tris, vs[:, 2].max() - vs[:, 2].min(), vs[:, 0].max() - vs[:, 0].min()))
    return ao, mo, look


# ---------------------------------------------------------------- export / preview

def export(args):
    glb = args.out.lower().endswith(".glb")
    bpy.ops.export_scene.gltf(filepath=os.path.abspath(args.out), export_format="GLB" if glb else "GLTF_SEPARATE",
                              export_yup=True, export_skins=True, export_animations=False, export_cameras=False,
                              export_lights=False, export_image_format="AUTO", export_apply=False, export_extras=True)


def save_png(path, rgb):
    img = to_image(os.path.splitext(os.path.basename(path))[0] + "_out", rgb)
    img.filepath_raw = path
    img.save()


def fight_pose(ao):
    """The reference sprite's stance, as armature-space turns per bone (children inherit them)."""
    turns = {"Hips": ((0, 0, 1), 25), "Spine2": ((0, 0, 1), 10),
             "LeftArm": ((0, 0, 1), -55), "LeftForeArm": ((0, 1, 0), -50),
             "RightArm": ((0, 0, 1), 35), "RightForeArm": ((0, 1, 0), 60),
             "LeftUpLeg": ((1, 0, 0), -30), "LeftLeg": ((1, 0, 0), 45),
             "RightUpLeg": ((1, 0, 0), 25), "RightLeg": ((1, 0, 0), 30)}
    world = {}
    for pb in sorted(ao.pose.bones, key=lambda pb: len(pb.bone.parent_recursive)):
        b = pb.bone
        parent = world.get(b.parent.name, Quaternion()) if b.parent else Quaternion()
        t = turns.get(b.name[len(P):])
        D = (Quaternion(t[0], math.radians(t[1])) @ parent) if t else parent
        world[b.name] = D
        R = b.matrix_local.to_quaternion()
        pb.rotation_mode = "QUATERNION"
        pb.rotation_quaternion = R.inverted() @ (parent.inverted() @ D) @ R
    bpy.context.view_layer.update()


def stage():
    sc = bpy.context.scene
    engines = {e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items}
    sc.render.engine = "BLENDER_EEVEE" if "BLENDER_EEVEE" in engines else "BLENDER_EEVEE_NEXT"
    world = bpy.data.worlds.new("W")
    sc.world = world
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.03, 0.035, 0.05, 1)
    world.node_tree.nodes["Background"].inputs["Strength"].default_value = 1.0
    for name, loc, energy, colour in (("Key", V(3, -4, 5), 900, (1, 1, 1)), ("Fill", V(-4, -2, 3), 300, (0.8, 0.85, 1)),
                                      ("Rim", V(0, 5, 4), 900, (0.5, 0.85, 1))):
        l = bpy.data.objects.new(name, bpy.data.lights.new(name, "AREA"))
        l.data.energy, l.data.size, l.data.color = energy, 3, colour
        l.location = loc
        l.rotation_euler = (V(0, 0, 1.4) - loc).to_track_quat("-Z", "Y").to_euler()
        sc.collection.objects.link(l)
    bpy.ops.mesh.primitive_plane_add(size=6)
    floor = bpy.context.active_object
    fm = bpy.data.materials.new("Floor")
    fm.use_nodes = True
    fm.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.18, 0.14, 0.11, 1)
    floor.data.materials.append(fm)
    cam = bpy.data.objects.new("Cam", bpy.data.cameras.new("Cam"))
    cam.data.lens = 60
    sc.collection.objects.link(cam)
    sc.camera = cam
    return cam, floor


def shoot(cam, path, deg, dist, height, target):
    a = math.radians(deg)
    cam.location = V(dist * math.sin(a), -dist * math.cos(a), height)
    cam.rotation_euler = (V(0, 0, target) - cam.location).to_track_quat("-Z", "Y").to_euler()
    bpy.context.scene.render.filepath = path
    bpy.ops.render.render(write_still=True)


def rest_pose(ao):
    for pb in ao.pose.bones:
        pb.rotation_quaternion = Quaternion()
    bpy.context.view_layer.update()


def preview(args, ao, cam):
    sc = bpy.context.scene
    sc.render.resolution_x, sc.render.resolution_y = 560, 720
    for tag, deg in (("front", 0), ("front34", 35), ("side", 90), ("back34", 145), ("back", 180)):
        shoot(cam, "%s_%s.png" % (args.preview, tag), deg, 8.6, 1.9, 1.62)
    fight_pose(ao)
    for tag, deg in (("pose_side", 90), ("pose_34", 55)):
        shoot(cam, "%s_%s.png" % (args.preview, tag), deg, 8.6, 1.8, 1.45)
    rest_pose(ao)


def picture(args, ao, cam, floor):
    """Choose Creature's picture: four 128 x 128 views side by side, as the shipped ones are."""
    sc = bpy.context.scene
    sc.render.resolution_x = sc.render.resolution_y = 256
    floor.hide_render = True
    sc.world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.002, 0.003, 0.012, 1)
    tiles = []
    shots = [(False, 30, 5.2, 2.2, 1.75), (True, 70, 6.0, 1.9, 1.45), (True, 330, 6.0, 2.1, 1.5), (False, 15, 2.2, 3.0, 2.85)]
    for k, (posed, deg, dist, height, target) in enumerate(shots):
        fight_pose(ao) if posed else rest_pose(ao)
        path = "%s.tile%d.png" % (args.picture, k)
        shoot(cam, path, deg, dist, height, target)
        img = bpy.data.images.load(path)
        px = np.array(img.pixels[:], np.float32).reshape(256, 256, 4)[::-1, :, :3]
        tiles.append(px.reshape(128, 2, 128, 2, 3).mean((1, 3)))
        bpy.data.images.remove(img)
        os.remove(path)
    strip = np.concatenate(tiles, 1)
    img = bpy.data.images.new("ChooseCreature", 512, 128, alpha=False)
    buf = np.ones((128, 512, 4), np.float32)
    buf[..., :3] = strip
    img.pixels.foreach_set(buf[::-1].ravel())
    img.filepath_raw = args.picture
    img.file_format = "PNG"
    img.save()
    floor.hide_render = False
    rest_pose(ao)


def main():
    args = parse_args()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    ao, mo, look = build(args)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    export(args)
    print("EXPORTED", args.out)
    if args.paint_out:
        os.makedirs(args.paint_out, exist_ok=True)
        save_png(os.path.join(args.paint_out, "ShadowColours.png"), look)
        dark = look.copy()
        sel = (look[..., 2] > 0.4) & (look[..., 2] < 0.97) & (np.abs(look[..., 0] - look[..., 2]) < 0.2)
        dark[sel] = look[sel] * np.array([0.32, 0.30, 0.36])
        save_png(os.path.join(args.paint_out, "ShadowDark.png"), dark)
    if args.blend:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(args.blend))
    if args.preview or args.picture:
        cam, floor = stage()
        if args.preview:
            preview(args, ao, cam)
        if args.picture:
            picture(args, ao, cam, floor)


main()
