"""Generate a deterministic skinned, textured, animated test humanoid and export it as glTF.

blender -b --factory-startup -t 4 --python make_character.py -- \
    --naming mixamo|rigify --textures atlas|multi --out out.glb|out.gltf [--no-anim] [--preview prefix]
"""
import argparse
import math
import os
import sys

import bpy
import numpy as np
from mathutils import Matrix, Quaternion, Vector

FPS = 30
SIDES = (("L", 1.0), ("R", -1.0))
TINT = {"L": np.array([0.25, 0.85, 0.35]), "R": np.array([1.0, 0.55, 0.12])}


def V(*a):
    return Vector(a)


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    p = argparse.ArgumentParser()
    p.add_argument("--naming", choices=["mixamo", "rigify"], required=True)
    p.add_argument("--textures", choices=["atlas", "multi"], required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--no-anim", action="store_true")
    p.add_argument("--preview", default=None, help="PNG path prefix for preview renders")
    return p.parse_args(argv)


def orth(v, axis):
    v = v - axis * v.dot(axis)
    return v.normalized()


def rotate_toward(d, target, deg):
    a = math.radians(deg)
    return (d * math.cos(a) + orth(target, d) * math.sin(a)).normalized()


def rx(deg):
    return Quaternion((1, 0, 0), math.radians(deg))


def ry(deg):
    return Quaternion((0, 1, 0), math.radians(deg))


def rz(deg):
    return Quaternion((0, 0, 1), math.radians(deg))


# ---------------------------------------------------------------- skeleton

def joints(apose):
    spine = [(0.92, 0.0), (1.02, 0.01), (1.14, 0.02), (1.27, 0.015), (1.42, 0.0),
             (1.50, -0.01), (1.58, -0.01), (1.80, 0.0)]
    J = {"spine": [V(0, y, z) for z, y in spine]}
    for s, sx in SIDES:
        S = V(0.21 * sx, 0.0, 1.44)
        d = V(sx * math.cos(math.radians(45)), 0, -math.sin(math.radians(45))) if apose else V(sx, 0, 0)
        E = S + d * 0.33
        d2 = rotate_toward(d, V(0, -1, 0), 10) if apose else d
        W = E + d2 * 0.30
        K = W + d2 * 0.10
        TB = W + d2 * 0.025 + V(0, -0.03, 0)
        J[s] = dict(
            C=V(0.03 * sx, -0.01, 1.41), S=S, E=E, W=W, K=K, F=K + d2 * 0.09,
            TB=TB, TT=TB + (d2 * 0.55 + V(0, -0.8, 0)).normalized() * 0.065,
            H=V(0.10 * sx, 0, 0.88), N=V(0.10 * sx, -0.025 if apose else 0.0, 0.48),
            A=V(0.10 * sx, 0, 0.08), B=V(0.10 * sx, -0.12, 0.03), T=V(0.10 * sx, -0.20, 0.03))
    return J


def skeleton(naming):
    """Return (bones, ctrl, segs): bones = [(name, head, tail, parent, roll_vec)],
    ctrl = semantic key -> bone that receives animation, segs = semantic key -> weighted bones."""
    J = joints(apose=(naming == "rigify"))
    sp = J["spine"]
    bones, ctrl, segs = [], {}, {}
    fwd, back, up = V(0, -1, 0), V(0, 1, 0), V(0, 0, 1)
    if naming == "mixamo":
        P = "mixamorig:"
        spn = ["Hips", "Spine", "Spine1", "Spine2"]
        for i, n in enumerate(spn):
            bones.append((P + n, sp[i], sp[i + 1], P + spn[i - 1] if i else None, fwd))
        bones += [(P + "Neck", sp[4], sp[6], P + "Spine2", fwd),
                  (P + "Head", sp[6], sp[7], P + "Neck", fwd),
                  (P + "HeadTop_End", sp[7], sp[7] + V(0, 0, 0.08), P + "Head", fwd)]
        for k, n in zip(["root", "spine1", "spine2", "chest", "neck", "head"],
                        spn + ["Neck", "Head"]):
            ctrl[k] = P + n
            segs[k] = [P + n]
        segs["breast"] = segs["pelvisx"] = []
        for s, _ in SIDES:
            j, N = J[s], P + ("Left" if s == "L" else "Right")
            i1 = j["K"] + (j["F"] - j["K"]) * 0.55
            bones += [
                (N + "Shoulder", j["C"], j["S"], P + "Spine2", back),
                (N + "Arm", j["S"], j["E"], N + "Shoulder", back),
                (N + "ForeArm", j["E"], j["W"], N + "Arm", back),
                (N + "Hand", j["W"], j["K"], N + "ForeArm", up),
                (N + "HandIndex1", j["K"], i1, N + "Hand", up),
                (N + "HandIndex2", i1, j["F"], N + "HandIndex1", up),
                (N + "HandThumb1", j["TB"], j["TT"], N + "Hand", up),
                (N + "UpLeg", j["H"], j["N"], P + "Hips", fwd),
                (N + "Leg", j["N"], j["A"], N + "UpLeg", fwd),
                (N + "Foot", j["A"], j["B"], N + "Leg", up),
                (N + "ToeBase", j["B"], j["T"], N + "Foot", up),
                (N + "Toe_End", j["T"], j["T"] + V(0, -0.03, 0), N + "ToeBase", up)]
            for k, n in [("clav", "Shoulder"), ("upper", "Arm"), ("fore", "ForeArm"), ("hand", "Hand"),
                         ("thumb", "HandThumb1"), ("thigh", "UpLeg"), ("shin", "Leg"),
                         ("foot", "Foot"), ("toe", "ToeBase")]:
                ctrl[k + "." + s] = N + n
                segs[k + "." + s] = [N + n]
            segs["index." + s] = [N + "HandIndex1", N + "HandIndex2"]
        return bones, ctrl, segs, J

    D = "DEF-spine"
    names = [D] + ["%s.%03d" % (D, i) for i in range(1, 7)]
    for i, n in enumerate(names):
        roll = V(0.15 if i % 2 else -0.15, -1, 0)
        bones.append((n, sp[i], sp[i + 1], names[i - 1] if i else None, roll))
    for k, idx in [("root", [0]), ("spine1", [1]), ("spine2", [2]), ("chest", [3]), ("neck", [4, 5]),
                   ("head", [6])]:
        ctrl[k] = names[idx[0]]
        segs[k] = [names[i] for i in idx]
    segs["breast"], segs["pelvisx"] = [], []
    for s, sx in SIDES:
        j, x = J[s], "." + s

        def half(a, b):
            return a + (b - a) * 0.5
        arm_roll = V(0.2 * sx, 1, 0.35)
        bones += [
            ("DEF-shoulder" + x, j["C"], j["S"], names[3], V(0, 0.3, 1)),
            ("DEF-upper_arm" + x, j["S"], half(j["S"], j["E"]), "DEF-shoulder" + x, arm_roll),
            ("DEF-upper_arm" + x + ".001", half(j["S"], j["E"]), j["E"], "DEF-upper_arm" + x, arm_roll),
            ("DEF-forearm" + x, j["E"], half(j["E"], j["W"]), "DEF-upper_arm" + x + ".001", arm_roll),
            ("DEF-forearm" + x + ".001", half(j["E"], j["W"]), j["W"], "DEF-forearm" + x, arm_roll),
            ("DEF-hand" + x, j["W"], j["K"], "DEF-forearm" + x + ".001", V(0.3 * sx, -1, 0.2)),
            ("DEF-f_index.01" + x, j["K"], j["F"], "DEF-hand" + x, V(0.3 * sx, -1, 0.2)),
            ("DEF-thumb.01" + x, j["TB"], j["TT"], "DEF-hand" + x, V(sx, 0.2, 1)),
            ("DEF-thigh" + x, j["H"], half(j["H"], j["N"]), D, V(0.05 * sx, -1, 0)),
            ("DEF-thigh" + x + ".001", half(j["H"], j["N"]), j["N"], "DEF-thigh" + x, V(0.05 * sx, -1, 0)),
            ("DEF-shin" + x, j["N"], half(j["N"], j["A"]), "DEF-thigh" + x + ".001", V(0.05 * sx, -1, 0)),
            ("DEF-shin" + x + ".001", half(j["N"], j["A"]), j["A"], "DEF-shin" + x, V(0.05 * sx, -1, 0)),
            ("DEF-foot" + x, j["A"], j["B"], "DEF-shin" + x + ".001", V(0.1 * sx, 0, 1)),
            ("DEF-toe" + x, j["B"], j["T"], "DEF-foot" + x, V(0.1 * sx, 0, 1)),
            ("DEF-breast" + x, V(0.10 * sx, -0.07, 1.32), V(0.10 * sx, -0.17, 1.30), names[3], V(0, 0, 1)),
            ("DEF-pelvis" + x, V(0, -0.01, 0.94), V(0.14 * sx, -0.08, 1.04), D, V(0, -1, 0.3))]
        for k, n in [("clav", "DEF-shoulder"), ("upper", "DEF-upper_arm"), ("fore", "DEF-forearm"),
                     ("hand", "DEF-hand"), ("index", "DEF-f_index.01"), ("thumb", "DEF-thumb.01"),
                     ("thigh", "DEF-thigh"), ("shin", "DEF-shin"), ("foot", "DEF-foot"), ("toe", "DEF-toe")]:
            ctrl[k + x] = n + x
            segs[k + x] = [n + x] + ([n + x + ".001"] if k in ("upper", "fore", "thigh", "shin") else [])
        segs["breast"].append("DEF-breast" + x)
        segs["pelvisx"].append("DEF-pelvis" + x)
    return bones, ctrl, segs, J


# ---------------------------------------------------------------- mesh parts

class Part:
    def __init__(self, name, a, b, r0, r1, ref, segs, group, side=None, aspect=1.0, seg=16, nb=4,
                 nc=4, capf=1.0, size=1, color=(0.7, 0.7, 0.7)):
        self.__dict__.update(locals())
        del self.__dict__["self"]


def body_parts(J):
    X, Y = V(1, 0, 0), V(0, -1, 0)
    P = [Part("head", V(0, -0.005, 1.635), V(0, -0.005, 1.705), .095, .095, X, ["neck", "head"], "skin",
              aspect=1.15, seg=24, nb=3, nc=7, size=2, color=(0.95, 0.78, 0.62)),
         Part("neck", V(0, 0, 1.40), V(0, -0.005, 1.60), .05, .045, X, ["chest", "neck", "head"], "skin",
              capf=.5, seg=14, nb=3, color=(0.85, 0.68, 0.55)),
         Part("chest", V(0, 0.01, 1.20), V(0, 0.005, 1.37), .14, .175, X,
              ["spine2", "chest", "neck", "clav.L", "clav.R", "breast"], "cloth", aspect=.62, capf=.55,
              seg=24, nb=5, color=(0.25, 0.4, 0.85)),
         Part("abdomen", V(0, .01, 1.02), V(0, .015, 1.20), .125, .135, X, ["spine1", "spine2", "chest"],
              "cloth", aspect=.72, capf=.5, seg=20, color=(0.55, 0.35, 0.8)),
         Part("pelvis", V(0, 0, .86), V(0, 0.01, 1.0), .15, .13, X,
              ["root", "spine1", "thigh.L", "thigh.R", "pelvisx"], "metal", aspect=.68, capf=.6, seg=20,
              color=(0.35, 0.35, 0.4))]
    for s, sx in SIDES:
        j, k = J[s], "." + s
        ad = (j["E"] - j["S"]).normalized()
        palm = V(0, -1, 0)
        P += [
            Part("eye" + k, V(.035 * sx, -.098, 1.69), V(.035 * sx, -.0985, 1.69), .019, .019, X, ["head"],
                 "visor", seg=10, nb=1, nc=4, color=(1, 1, 1)),
            Part("shoulder" + k, j["S"] - ad * 0.012, j["S"] + ad * 0.012, .066, .066, Y,
                 ["clav" + k, "upper" + k], "cloth", seg=14, nb=1, nc=5, color=(0.3, 0.5, 0.9)),
            Part("upperarm" + k, j["S"], j["E"], .056, .045, Y, ["clav" + k, "upper" + k, "fore" + k], "cloth",
                 color=(0.4, 0.6, 0.95)),
            Part("forearm" + k, j["E"], j["W"], .045, .034, Y, ["upper" + k, "fore" + k, "hand" + k], "metal",
                 color=(0.7, 0.7, 0.75)),
            Part("hand" + k, j["W"], j["K"], .038, .043, palm,
                 ["fore" + k, "hand" + k, "index" + k, "thumb" + k], "skin", aspect=.45, capf=.6, seg=12,
                 nb=3, color=(0.95, 0.75, 0.6)),
            Part("fingers" + k, j["K"], j["F"], .038, .032, palm, ["hand" + k, "index" + k], "skin",
                 aspect=.42, capf=.7, seg=12, nb=3, nc=3, color=(0.9, 0.7, 0.55)),
            Part("thumb" + k, j["TB"], j["TT"], .017, .014, X, ["hand" + k, "thumb" + k], "skin", seg=10,
                 nb=2, nc=3, color=(0.9, 0.65, 0.5)),
            Part("thigh" + k, j["H"], j["N"], .085, .06, X, ["root", "thigh" + k, "shin" + k, "pelvisx"],
                 "cloth", seg=18, color=(0.2, 0.3, 0.6)),
            Part("shin" + k, j["N"], j["A"], .057, .04, X, ["thigh" + k, "shin" + k, "foot" + k], "metal",
                 color=(0.6, 0.6, 0.65)),
            Part("foot" + k, V(j["A"].x, 0.045, 0.045), V(j["A"].x, -0.11, 0.04), .048, .045, X,
                 ["shin" + k, "foot" + k, "toe" + k], "rubber", aspect=.82, capf=.8, seg=14,
                 color=(0.2, 0.2, 0.22)),
            Part("toes" + k, V(j["A"].x, -0.11, 0.032), V(j["A"].x, -0.185, 0.03), .036, .031, X,
                 ["foot" + k, "toe" + k], "rubber", aspect=.72, capf=.7, seg=12, nb=2, nc=3,
                 color=(0.9, 0.85, 0.2))]
        for p in P[-11:]:
            p.side = s if not p.name.startswith("eye") else None
    return P


def capsule(p):
    """Rings around segment a->b with elliptic cross-section and capped ends.
    Returns verts (n,3), faces, per-corner (s,t) in 0..1, profile arc length."""
    a, b = p.a, p.b
    w = (b - a).normalized()
    u = orth(p.ref, w)
    v = w.cross(u)
    prof = []  # (center, radius)
    for i in range(1, p.nc):
        phi = -math.pi / 2 + math.pi / 2 * i / p.nc
        prof.append((a + w * (p.r0 * p.capf * math.sin(phi)), p.r0 * math.cos(phi)))
    for i in range(p.nb + 1):
        t = i / p.nb
        prof.append((a.lerp(b, t), p.r0 + (p.r1 - p.r0) * t))
    for i in range(1, p.nc):
        phi = math.pi / 2 * i / p.nc
        prof.append((b + w * (p.r1 * p.capf * math.sin(phi)), p.r1 * math.cos(phi)))
    poleA, poleB = a - w * p.r0 * p.capf, b + w * p.r1 * p.capf
    pts2 = [((poleA - a).dot(w), 0.0)] + [((c - a).dot(w), r) for c, r in prof] + [((poleB - a).dot(w), 0.0)]
    arc = np.concatenate([[0], np.cumsum([math.hypot(pts2[i + 1][0] - pts2[i][0], pts2[i + 1][1] - pts2[i][1])
                                          for i in range(len(pts2) - 1)])])
    tv = arc / arc[-1]
    n = p.seg
    verts = [poleA]
    for c, r in prof:
        for j in range(n):
            th = 2 * math.pi * j / n
            verts.append(c + (u * math.cos(th) + v * (p.aspect * math.sin(th))) * r)
    verts.append(poleB)
    nr = len(prof)
    last = len(verts) - 1

    def rv(k, j):
        return 1 + k * n + (j % n)
    faces, uvs = [], []
    for j in range(n):
        faces.append((0, rv(0, j + 1), rv(0, j)))
        uvs.append((((j + .5) / n, 0.0), ((j + 1) / n, tv[1]), (j / n, tv[1])))
        for k in range(nr - 1):
            faces.append((rv(k, j), rv(k, j + 1), rv(k + 1, j + 1), rv(k + 1, j)))
            uvs.append(((j / n, tv[k + 1]), ((j + 1) / n, tv[k + 1]), ((j + 1) / n, tv[k + 2]), (j / n, tv[k + 2])))
        faces.append((rv(nr - 1, j), rv(nr - 1, j + 1), last))
        uvs.append(((j / n, tv[nr]), ((j + 1) / n, tv[nr]), ((j + .5) / n, 1.0)))
    return np.array([tuple(x) for x in verts]), faces, uvs, float(arc[-1])


def pack(items):
    units = sum(s * s for _, s in items)
    n = max(2, math.ceil(math.sqrt(units)))
    while True:
        occ, out = np.zeros((n, n), bool), {}
        for name, s in items:
            spot = next(((r, c) for r in range(n - s + 1) for c in range(n - s + 1)
                         if not occ[r:r + s, c:c + s].any()), None)
            if spot is None:
                break
            occ[spot[0]:spot[0] + s, spot[1]:spot[1] + s] = True
            out[name] = (spot[1] / n, spot[0] / n, s / n)
        else:
            return out
        n += 1


# ---------------------------------------------------------------- textures

MATS = {  # name: (base colour, image size or 0, metallic, roughness)
    "skin": ((0.86, 0.66, 0.53), 512, 0.0, 0.6),
    "cloth": ((0.16, 0.24, 0.55), 512, 0.0, 0.85),
    "metal": ((0.62, 0.63, 0.66), 256, 1.0, 0.35),
    "rubber": ((0.07, 0.07, 0.08), 0, 0.0, 0.95),
    "visor": ((0.15, 0.85, 1.0), 0, 0.2, 0.15),
}


def paint(style, p, s, t, rng):
    shp = s.shape
    if p.name.startswith("eye"):
        col = np.ones(shp + (3,))
        col[t > 0.72] = (0.05, 0.05, 0.1)
        col[(t > 0.6) & (t <= 0.72)] = (0.1, 0.45, 0.8)
        return col
    if style == "atlas":
        chk = (np.floor(s * 8) + np.floor(t * 6)) % 2
        col = np.array(p.color) * ((0.72 + 0.28 * chk) * (0.65 + 0.35 * t))[..., None]
        col[(t * 4) % 1 < 0.05] = (0.95, 0.95, 0.95)
    elif style == "skin":
        noise = rng.random(shp) * 0.05
        col = np.array(MATS["skin"][0]) * (0.95 + 0.05 * np.sin(s * 60) * np.sin(t * 45) + noise)[..., None]
    elif style == "cloth":
        lines = (((s * 10) % 1 < 0.14) | ((t * 8) % 1 < 0.14)).astype(float)
        diag = (np.floor(s * 40 + t * 40) % 2) * 0.06
        col = np.array(MATS["cloth"][0]) * (1 + diag)[..., None] + lines[..., None] * np.array([0.5, 0.15, 0.1])
    else:  # metal
        brushed = np.repeat(rng.random((1, shp[1])), shp[0], 0) * 0.35 + 0.08 * np.sin(t * 50)
        col = np.array(MATS["metal"][0]) * (0.75 + brushed)[..., None]
        riv = (((s * 6) % 1 - .5) ** 2 + ((t * 5) % 1 - .5) ** 2) < 0.012
        col[riv] = (0.3, 0.3, 0.32)
    if p.side:
        col = col * 0.55 + TINT[p.side] * 0.45
    col[s < 0.035] = (1.0, 1.0, 1.0)
    col[np.abs(s - 0.5) < 0.018] = (0.02, 0.02, 0.02)
    if p.name == "head":
        x = (s - 0.75) * 2 * math.pi * p.r0
        y = t * p.arc
        hair = (y > 0.27) | ((np.abs(s - 0.25) < 0.2) & (y > 0.17))
        col[hair] = (0.25, 0.14, 0.07)
        for ex in (-0.035, 0.035):
            col[np.hypot(x - ex, y - 0.204) < 0.024] = (0.1, 0.1, 0.15)
            col[(np.abs(x - ex) < 0.022) & (np.abs(y - 0.238) < 0.005)] = (0.2, 0.1, 0.05)
        col[(np.abs(x) < 0.008) & (y > 0.15) & (y < 0.19)] *= 0.75
        mouth = (np.abs(x) < 0.03) & (np.abs(y - 0.122 - 6 * x * x) < 0.005)
        col[mouth] = (0.7, 0.1, 0.12)
    return np.clip(col, 0, 1)


def make_image(name, size, style, parts, rects, arcs):
    rng = np.random.default_rng(1234)
    buf = np.zeros((size, size, 4), np.float32)
    buf[..., 3] = 1.0
    buf[..., :3] = 0.5
    for p in parts:
        x0, y0, wh = rects[p.name]
        px0, py0, pw = int(round(x0 * size)), int(round(y0 * size)), int(round(wh * size))
        pad = 4 / size
        xs = ((np.arange(px0, px0 + pw) + 0.5) / size - x0 - pad) / (wh - 2 * pad)
        ys = ((np.arange(py0, py0 + pw) + 0.5) / size - y0 - pad) / (wh - 2 * pad)
        s, t = np.meshgrid(np.clip(xs, 0, 1), np.clip(ys, 0, 1))
        p.arc = arcs[p.name]
        buf[py0:py0 + pw, px0:px0 + pw, :3] = paint(style, p, s, t, rng)
    img = bpy.data.images.new(name, size, size, alpha=False)
    img.pixels.foreach_set(buf.ravel())
    img.file_format = "PNG"
    img.pack()
    return img


def make_material(name, color, img, metallic, rough):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = rough
    if img is not None:
        tex = m.node_tree.nodes.new("ShaderNodeTexImage")
        tex.image = img
        m.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    return m


# ---------------------------------------------------------------- build

def build(args):
    bones, ctrl, segs, J = skeleton(args.naming)
    parts = body_parts(J)
    groups = ["atlas"] if args.textures == "atlas" else list(MATS)
    gof = (lambda p: "atlas") if args.textures == "atlas" else (lambda p: p.group)

    stem = os.path.splitext(os.path.basename(args.out))[0]
    geo = {p.name: capsule(p) for p in parts}
    arcs = {k: g[3] for k, g in geo.items()}
    materials, rects = [], {}
    for gname in groups:
        gparts = [p for p in parts if gof(p) == gname]
        r = pack([(p.name, p.size) for p in gparts])
        rects.update(r)
        if gname == "atlas":
            img = make_image(stem + "_atlas", 1024, "atlas", gparts, r, arcs)
            materials.append(make_material("CharacterAtlas", (1, 1, 1), img, 0.0, 0.7))
        else:
            col, size, met, rough = MATS[gname]
            img = make_image(stem + "_" + gname, size, gname, gparts, r, arcs) if size else None
            materials.append(make_material(gname.capitalize(), col, img, met, rough))

    verts, faces, uvs, mat_idx, ranges = [], [], [], [], {}
    for p in parts:
        pv, pf, puv, _ = geo[p.name]
        base = sum(len(x) for x in verts)
        ranges[p.name] = (base, base + len(pv))
        verts.append(pv)
        x0, y0, wh = rects[p.name]
        size = 1024 if args.textures == "atlas" else (MATS[p.group][1] or 256)
        pad = 4 / size
        for f, fu in zip(pf, puv):
            faces.append(tuple(i + base for i in f))
            uvs.extend((x0 + pad + u * (wh - 2 * pad), y0 + pad + v * (wh - 2 * pad)) for u, v in fu)
            mat_idx.append(groups.index(gof(p)))
    verts = np.concatenate(verts)

    me = bpy.data.meshes.new("Body")
    me.from_pydata(verts.tolist(), [], faces)
    me.update()
    uvl = me.uv_layers.new(name="UVMap")
    uvl.data.foreach_set("uv", np.array(uvs, np.float32).ravel())
    me.polygons.foreach_set("material_index", mat_idx)
    me.shade_smooth()
    for m in materials:
        me.materials.append(m)
    mo = bpy.data.objects.new("Body", me)
    bpy.context.scene.collection.objects.link(mo)

    arm = bpy.data.armatures.new("Armature")
    ao = bpy.data.objects.new("Armature" if args.naming == "mixamo" else "rig", arm)
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

    # Distance-based skinning: inverse distance to bone segments of each part's candidate bones.
    weights = {}
    for p in parts:
        cand = [b for k in p.segs for b in segs[k]]
        cand = list(dict.fromkeys(cand))
        lo, hi = ranges[p.name]
        P = verts[lo:hi]
        dist = []
        for bn in cand:
            h, t = np.array(arm.bones[bn].head_local), np.array(arm.bones[bn].tail_local)
            ab = t - h
            tt = np.clip((P - h) @ ab / (ab @ ab), 0, 1)
            dist.append(np.linalg.norm(P - (h + tt[:, None] * ab), axis=1))
        w = 1.0 / (np.stack(dist, 1) + 0.01) ** 5
        if w.shape[1] > 4:
            w[np.arange(len(w))[:, None], np.argsort(-w, 1)[:, 4:]] = 0
        w /= w.sum(1, keepdims=True)
        for ci, bn in enumerate(cand):
            for vi in np.nonzero(w[:, ci] > 1e-4)[0]:
                weights.setdefault(bn, []).append((lo + int(vi), float(w[vi, ci])))
    for bn in arm.bones.keys():
        vg = mo.vertex_groups.new(name=bn)
        for vi, wv in weights.get(bn, []):
            vg.add([vi], wv, "REPLACE")
    mo.parent = ao
    mod = mo.modifiers.new("Armature", "ARMATURE")
    mod.object = ao
    print("BUILD parts=%d verts=%d tris=%d minz=%.4f maxz=%.4f" % (
        len(parts), len(verts), sum(len(f) - 2 for f in faces), verts[:, 2].min(), verts[:, 2].max()))
    return ao, mo, ctrl


# ---------------------------------------------------------------- posing

class Rig:
    def __init__(self, ao, ctrl):
        self.ao, self.ctrl = ao, ctrl
        bones = ao.data.bones
        self.order = []

        def walk(b):
            self.order.append(b.name)
            for c in b.children:
                walk(c)
        for b in bones:
            if b.parent is None:
                walk(b)
        self.parent = {b.name: (b.parent.name if b.parent else None) for b in bones}
        self.M = {b.name: b.matrix_local.to_quaternion() for b in bones}
        self.head = {b.name: b.head_local.copy() for b in bones}
        self.root = ctrl["root"]

    def propagate(self, cD):
        D = {}
        for n in self.order:
            p = self.parent[n]
            D[n] = cD.get(n, D[p] if p else Quaternion())
        return D

    def positions(self, D, offset):
        pos = {}
        for n in self.order:
            p = self.parent[n]
            pos[n] = self.head[n] + offset if p is None else pos[p] + D[p] @ (self.head[n] - self.head[p])
        return pos

    def ik(self, k1, k2, k3, start, target, pole, hint):
        """Two-bone IK in armature space; returns global deltas for the bones of k1 and k2."""
        b1, b2, b3 = (self.ctrl[k] for k in (k1, k2, k3))
        L1 = (self.head[b2] - self.head[b1]).length
        L2 = (self.head[b3] - self.head[b2]).length
        r1 = (self.head[b2] - self.head[b1]).normalized()
        r2 = (self.head[b3] - self.head[b2]).normalized()
        d = target - start
        dist = min(max(d.length, abs(L1 - L2) + 1e-3), (L1 + L2) * 0.999)
        n = d.normalized()
        a = (L1 * L1 - L2 * L2 + dist * dist) / (2 * dist)
        h = math.sqrt(max(L1 * L1 - a * a, 0.0))
        pdir = orth(pole, n)
        knee = start + n * a + pdir * h
        d1 = (knee - start).normalized()
        d2 = (start + n * dist - knee).normalized()

        def frame(x, y):
            y = orth(y, x)
            return Matrix((x, y, x.cross(y))).transposed()
        D1 = (frame(d1, pdir) @ frame(r1, hint).transposed()).to_quaternion()
        D2 = (D1 @ r2).rotation_difference(d2) @ D1
        return {b1: D1, b2: D2}

    def solve(self, spec):
        offset = spec.get("offset", Vector())
        rel = spec.get("rel", {})
        cD, acc = {}, Quaternion()
        for k in ("root", "spine1", "spine2", "chest", "neck", "head"):
            acc = acc @ rel.get(k, Quaternion())
            cD[self.ctrl[k]] = acc.copy()
            if k == "chest":
                chest = acc.copy()
        for s, _ in SIDES:
            cD[self.ctrl["clav." + s]] = chest @ rel.get("clav." + s, Quaternion())
        pos = self.positions(self.propagate(cD), offset)
        for s, sx in SIDES:
            if "arm." + s in spec:
                off, pole = spec["arm." + s]
                S = pos[self.ctrl["upper." + s]]
                cD.update(self.ik("upper." + s, "fore." + s, "hand." + s, S, S + off, pole, V(0, 1, 0)))
            if "leg." + s in spec:
                ank, pitch, toe = spec["leg." + s]
                H = pos[self.ctrl["thigh." + s]]
                cD.update(self.ik("thigh." + s, "shin." + s, "foot." + s, H, ank, V(0, -1, 0), V(0, -1, 0)))
                cD[self.ctrl["foot." + s]] = rx(pitch)
                cD[self.ctrl["toe." + s]] = rx(pitch) @ rx(toe)
        return self.propagate(cD), offset

    def key(self, spec, frame, prev):
        D, offset = self.solve(spec)
        for n in self.order:
            pb = self.ao.pose.bones[n]
            p = self.parent[n]
            Dp = D[p] if p else Quaternion()
            q = self.M[n].inverted() @ (Dp.inverted() @ D[n]) @ self.M[n]
            if n in prev:
                q.make_compatible(prev[n])
            prev[n] = q
            pb.rotation_quaternion = q
            pb.keyframe_insert("rotation_quaternion", frame=frame, group=n)
            if n == self.root:
                pb.location = self.M[n].inverted() @ offset
                pb.keyframe_insert("location", frame=frame, group=n)


def ease(keys, t):
    for (t0, v0), (t1, v1) in zip(keys, keys[1:]):
        if t <= t1:
            u = (t - t0) / (t1 - t0)
            u = u * u * (3 - 2 * u)
            return v0 * (1 - u) + v1 * u
    return keys[-1][1]


def rest_ankle(rig, s):
    return rig.head[rig.ctrl["foot." + s]].copy()


def hang(rig, s, sx, fwd_deg, reach=0.95, out=0.2):
    L = (rig.head[rig.ctrl["hand." + s]] - rig.head[rig.ctrl["upper." + s]]).length
    a = math.radians(fwd_deg)
    return (V(sx * out, -math.sin(a), -math.cos(a)).normalized() * L * reach, V(0.3 * sx, 1, -0.2))


def anim_idle(rig, ph):
    b = math.sin(2 * math.pi * ph)
    spec = {"offset": V(0.012 * b, 0, -0.025 + 0.004 * math.cos(4 * math.pi * ph)),
            "rel": {"spine1": rz(1.5 * b), "chest": rx(-1.5 * math.cos(2 * math.pi * ph)),
                    "neck": rx(1.0 * b), "head": rz(-2.5 * b),
                    "clav.L": ry(-1.5 * math.cos(2 * math.pi * ph)), "clav.R": ry(1.5 * math.cos(2 * math.pi * ph))}}
    for s, sx in SIDES:
        spec["arm." + s] = hang(rig, s, sx, 4 + 3 * sx * b, 0.94, 0.22)
        spec["leg." + s] = (rest_ankle(rig, s), 0.0, 0.0)
    return spec


def anim_walk(rig, ph):
    S, LIFT, STANCE = 0.22, 0.09, 0.6
    c = math.cos(2 * math.pi * ph)
    spec = {"offset": V(0.02 * math.sin(2 * math.pi * ph), 0, -0.03 - 0.012 * math.cos(4 * math.pi * ph)),
            "rel": {"spine1": rx(4) @ rz(-3 * c), "chest": rz(-4 * c), "head": rz(5 * c) @ rx(-4)}}
    for s, sx in SIDES:
        p = (ph + (0.0 if s == "L" else 0.5)) % 1.0
        ank = rest_ankle(rig, s)
        if p < STANCE:
            y, z, pitch = -S + 2 * S * p / STANCE, 0.0, 0.0
        else:
            q = (p - STANCE) / (1 - STANCE)
            sm = q * q * (3 - 2 * q)
            y, z, pitch = S - 2 * S * sm, LIFT * math.sin(math.pi * q), 18 * math.sin(math.pi * q) - 8 * q
        spec["leg." + s] = (ank + V(0, y, z), pitch, -8 * math.sin(math.pi * min(1, p / STANCE)) * (p > 0.45))
        spec["arm." + s] = hang(rig, s, sx, -22 * c * sx, 0.93, 0.18)
    return spec


def anim_punch(rig, t):
    twist = ease([(0, 0.0), (0.28, -8.0), (0.55, 22.0), (0.7, 22.0), (1, 0.0)], t)
    offR = ease([(0, V(0.10, -0.30, 0.0)), (0.28, V(0.10, -0.18, -0.02)), (0.55, V(0.13, -0.62, 0.04)),
                 (0.7, V(0.13, -0.62, 0.04)), (1, V(0.10, -0.30, 0.0))], t)
    spec = {"offset": V(0, -0.01, -0.035),
            "rel": {"spine1": rz(twist * 0.4) @ rx(3), "chest": rz(twist * 0.6), "head": rz(-twist)},
            "arm.R": (offR, V(-0.6, 0.3, -1)), "arm.L": (V(-0.09, -0.30, 0.0), V(0.6, 0.3, -1))}
    for s, sx in SIDES:
        spec["leg." + s] = (rest_ankle(rig, s), 0.0, 0.0)
    return spec


def make_actions(ao, ctrl):
    rig = Rig(ao, ctrl)
    ao.animation_data_create()
    acts = []
    for name, fn, frames in [("Idle", anim_idle, 60), ("Walk", anim_walk, 30), ("Punch", anim_punch, 18)]:
        act = bpy.data.actions.new(name)
        act.use_fake_user = True
        ao.animation_data.action = act
        prev = {}
        for f in range(frames + 1):
            rig.key(fn(rig, f / frames), f, prev)
        ao.animation_data.action = None
        track = ao.animation_data.nla_tracks.new()
        track.name = name
        strip = track.strips.new(name, 0, act)
        track.mute = True
        acts.append(act)
    for pb in ao.pose.bones:
        pb.rotation_quaternion = Quaternion()
        pb.location = Vector()
    return acts


# ---------------------------------------------------------------- export / preview

def export(args):
    glb = args.out.lower().endswith(".glb")
    kw = dict(filepath=os.path.abspath(args.out), export_format="GLB" if glb else "GLTF_SEPARATE",
              export_yup=True, export_skins=True, export_animations=not args.no_anim,
              export_animation_mode="ACTIONS", export_cameras=False, export_lights=False,
              export_image_format="AUTO", export_optimize_animation_size=False, export_apply=False)
    bpy.ops.export_scene.gltf(**kw)


def preview(args, ao, acts):
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.display.shading.light = "STUDIO"
    sc.display.shading.color_type = "TEXTURE"
    sc.display.shading.show_backface_culling = True
    sc.render.resolution_x = sc.render.resolution_y = 640
    sc.render.film_transparent = False
    bpy.ops.mesh.primitive_plane_add(size=3)
    cam = bpy.data.objects.new("PreviewCam", bpy.data.cameras.new("PreviewCam"))
    sc.collection.objects.link(cam)
    sc.camera = cam
    shots = [("rest", "REST", None, 0, V(2.0, -2.9, 1.4)),
             ("walk", "POSE", "Walk", 8, V(2.0, -2.9, 1.4)),
             ("walk_side", "POSE", "Walk", 8, V(3.6, -0.2, 1.0)),
             ("punch", "POSE", "Punch", 10, V(1.6, -3.1, 1.4))]
    for tag, mode, act, frame, loc in shots:
        if act and not acts:
            continue
        ao.data.pose_position = mode
        ao.animation_data.action = bpy.data.actions.get(act) if act else None
        if act:
            ao.animation_data.action_slot = ao.animation_data.action.slots[0]
        sc.frame_set(frame)
        cam.location = loc
        cam.rotation_euler = (V(0, 0, 0.9) - loc).to_track_quat("-Z", "Y").to_euler()
        sc.render.filepath = "%s_%s.png" % (args.preview, tag)
        bpy.ops.render.render(write_still=True)


def main():
    args = parse_args()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.render.fps = FPS
    sc.frame_start, sc.frame_end = 0, 60
    ao, mo, ctrl = build(args)
    acts = [] if args.no_anim else make_actions(ao, ctrl)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    export(args)
    print("EXPORTED", args.out)
    if args.preview:
        preview(args, ao, acts)


main()
