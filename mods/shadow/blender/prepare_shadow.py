"""Prepare Shadow, the One Must Fall 2097 robot, from the user's model (shadow-model.fbx) for the
editor's robot import: scaled, rigged with Mixamo bones, weighted, textured, animated (his 2097
stance and moves, animate_shadow.py) and exported as a skinned glTF. The mesh keeps all its
triangles unless --tris asks for fewer.

blender -b --factory-startup --python prepare_shadow.py -- --fbx shadow-model.fbx --out shadow.glb
    [--blend shadow.blend] [--texture tripo.png] [--paint-out dir] [--picture choose_creature.png]
    [--preview prefix] [--height 3.1] [--tris N]

The FBX is a single untextured, unrigged mesh in a T-pose, 1 m tall, facing -Y. Joints are measured
from its surface; the texture is painted per texel in its own UV layout (silver armour, striped
limbs, a dark face, cyan studs and blades) unless --texture gives one. Faces go to six materials
sharing that texture, each with a `de_paint` custom property (FINDINGS 710) and metallic for the
reflection (FINDINGS 711).
"""
import argparse
import math
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Quaternion, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import animate_shadow  # noqa: E402

P = "mixamorig:"
SIDES = (("Left", 1.0), ("Right", -1.0))
TEX = 1024

ARMOUR, LIMB, HAND, DARK, GLOW, BLADE = range(6)
#   name: (category, paint: -1 own colours, 0-2 the pilot's colour; metallic, roughness, glow strength)
MATERIALS = {
    "Armour": (ARMOUR, 0, 0.85, 0.30, 0.0),
    "Limbs": (LIMB, 1, 0.75, 0.35, 0.0),
    "Hands": (HAND, 2, 0.80, 0.35, 0.0),
    "Dark": (DARK, -1, 0.50, 0.45, 0.0),
    "Glow": (GLOW, -1, 0.00, 0.50, 1.0),
    "Blade": (BLADE, -1, 0.30, 0.30, 1.0),
}


def V(*a):
    return Vector(a)


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    p = argparse.ArgumentParser()
    p.add_argument("--fbx", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--blend", default=None)
    p.add_argument("--texture", default=None, help="the model's own texture, laid out in its UVs, instead of the generated one")
    p.add_argument("--paint-out", default=None, help="directory for ShadowColours.png and the Stealth paint job ShadowDark.png")
    p.add_argument("--picture", default=None, help="Choose Creature's picture, a 512 x 128 PNG")
    p.add_argument("--preview", default=None, help="PNG path prefix for turntable and pose renders")
    p.add_argument("--height", type=float, default=3.1, help="metres")
    p.add_argument("--tris", type=int, default=0, help="decimate LOD0 to this many triangles (0: keep the model's)")
    return p.parse_args(argv)


# ---------------------------------------------------------------- the mesh

def load(path):
    bpy.ops.import_scene.fbx(filepath=os.path.abspath(path))
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    for o in bpy.data.objects:
        o.select_set(o in meshes)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    mo = bpy.context.view_layer.objects.active
    for o in [o for o in bpy.data.objects if o != mo]:
        bpy.data.objects.remove(o)
    mo.name = mo.data.name = "Shadow"
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    me = mo.data
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.to_mesh(me)
    bm.free()
    while len(me.uv_layers) > 1:
        me.uv_layers.remove(me.uv_layers[-1])
    me.uv_layers[0].name = "UVMap"
    me.materials.clear()
    return mo


def arrays(me):
    n, f = len(me.vertices), len(me.polygons)
    co = np.empty(n * 3, np.float32)
    me.vertices.foreach_get("co", co)
    nr = np.empty(n * 3, np.float32)
    me.vertices.foreach_get("normal", nr)
    tri = np.empty(f * 3, np.int64)
    me.polygons.foreach_get("vertices", tri)
    uv = np.empty(f * 6, np.float32)
    me.uv_layers[0].data.foreach_get("uv", uv)
    return co.reshape(-1, 3), nr.reshape(-1, 3), tri.reshape(-1, 3), uv.reshape(-1, 3, 2)


def normalise(mo, height):
    """Feet on the floor, centred, `height` metres tall."""
    co, _, _, _ = arrays(mo.data)
    lo, hi = co.min(0), co.max(0)
    k = height / (hi[2] - lo[2])
    off = np.array([-(lo[0] + hi[0]) / 2, -(lo[1] + hi[1]) / 2, -lo[2]], np.float32)
    mo.data.vertices.foreach_set("co", ((co + off) * k).ravel())
    mo.data.update()
    return k


def components(tri, n):
    parent = np.arange(n)

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    for a, b, c in tri:
        ra, rb, rc = find(a), find(b), find(c)
        parent[rb] = ra
        parent[find(rc)] = ra
    roots = np.array([find(i) for i in range(n)])
    _, label, size = np.unique(roots, return_inverse=True, return_counts=True)
    return label, size[label]


def smooth(a, edges, deg, it):
    for _ in range(it):
        s = np.zeros_like(a)
        np.add.at(s, edges[:, 0], a[edges[:, 1]])
        np.add.at(s, edges[:, 1], a[edges[:, 0]])
        s = s / (np.maximum(deg, 1)[:, None] if a.ndim == 2 else np.maximum(deg, 1))
        a = 0.5 * a + 0.5 * s
    return a


def relief(co, nr, tri):
    """Per vertex: how far it stands out of (+) or sinks into (-) the surface around it."""
    e = np.concatenate([tri[:, [0, 1]], tri[:, [1, 2]], tri[:, [2, 0]]])
    e = np.unique(np.sort(e, 1), axis=0)
    deg = np.bincount(e.ravel(), minlength=len(co)).astype(np.float32)
    d = ((co - smooth(co, e, deg, 6)) * nr).sum(1)
    return d - smooth(d, e, deg, 12)


# ---------------------------------------------------------------- joints, measured from the surface

def surface_points(co, tri, n=300000, seed=1):
    a, b, c = co[tri[:, 0]], co[tri[:, 1]], co[tri[:, 2]]
    area = np.linalg.norm(np.cross(b - a, c - a), axis=1)
    rng = np.random.default_rng(seed)
    f = rng.choice(len(tri), n, p=area / area.sum())
    r1, r2 = rng.random(n), rng.random(n)
    s = np.sqrt(r1)
    return a[f] * (1 - s)[:, None] + b[f] * (s * (1 - r2))[:, None] + c[f] * (s * r2)[:, None]


def scan(pts, axis, lo, hi, step, sel, measure):
    out = []
    for t in np.arange(lo, hi + 1e-9, step):
        m = sel & (np.abs(pts[:, axis] - t) < step * 0.75)
        out.append((t, measure(pts[m]) if m.sum() > 8 else np.inf))
    return out


def joints(co, tri, H):
    """Joint positions (metres, Z up, facing -Y) of the T-posed mesh, `H` tall."""
    p = surface_points(co, tri)
    x, y, z = p.T
    h = lambda f: f * H
    # Legs: their centre line, the crotch, the knee pads' front, the ankle's waist.
    leg = (x > h(0.02)) & (z > h(0.12)) & (z < h(0.45))
    lx = float(np.median(x[leg]))
    # Narrow: the inner thighs come within 4 cm of the midline 15 cm below the crotch.
    centre = np.abs(x) < h(0.005)
    crotch = min(t for t, c in scan(p, 2, h(0.30), h(0.70), h(0.01), centre, len) if c != np.inf)
    side = (x > 0) & (np.abs(x - lx) < h(0.06))
    knee = min(scan(p, 2, h(0.22), h(0.40), h(0.005), side, lambda q: q[:, 1].min()), key=lambda r: r[1])[0]
    ankle = min(scan(p, 2, h(0.06), h(0.20), h(0.005), side, lambda q: np.ptp(q[:, 1])), key=lambda r: r[1])[0]
    lyc = lambda t: float(np.median(y[side & (np.abs(z - t) < h(0.01))]))
    foot = side & (z < h(0.05))
    toe_y, heel_y = float(y[foot].min()), float(y[foot].max())
    # Arms: their axis, then the waists at elbow and wrist, out to the fingertips.
    band = (x > h(0.20)) & (x < h(0.36))
    az = float(np.median(z[band]))
    ay = float(np.median(y[band]))
    arm = (x > 0) & (np.abs(z - az) < h(0.05)) & (np.abs(y - ay) < h(0.06))
    radius = lambda q: np.percentile(np.hypot(q[:, 1] - ay, q[:, 2] - az), 90)
    tip = float(x[arm].max())
    elbow = min(scan(p, 0, h(0.22), h(0.30), h(0.005), arm, radius), key=lambda r: r[1])[0]
    wrist = min(scan(p, 0, h(0.33), h(0.40), h(0.005), arm, radius), key=lambda r: r[1])[0]
    upper = np.median([r for t, r in scan(p, 0, h(0.18), elbow, h(0.005), arm, radius) if r != np.inf])
    shoulder = next((t for t, r in scan(p, 0, h(0.08), h(0.20), h(0.005), arm, radius) if r < upper * 1.6), h(0.15))
    # Neck: the narrowest slice between the chest plate and the head.
    head = np.abs(x) < h(0.1)
    neck = min(scan(p, 2, h(0.86), h(0.93), h(0.005), head, lambda q: np.ptp(q[:, 0])), key=lambda r: r[1])[0]
    waist = min(scan(p, 2, h(0.55), h(0.72), h(0.005), head, lambda q: np.ptp(q[:, 0])), key=lambda r: r[1])[0]
    top = float(z.max())
    J = {"waist": waist, "hips": V(0, lyc(crotch + h(0.03)), crotch + h(0.03)), "top": V(0, 0, top), "neck": V(0, 0, neck - h(0.02)),
         "head": V(0, 0, neck + h(0.01))}
    J["spine"] = J["hips"].lerp(J["neck"], 0.22)
    J["spine1"] = J["hips"].lerp(J["neck"], 0.45)
    J["spine2"] = J["hips"].lerp(J["neck"], 0.68)
    for s, sx in SIDES:
        S = V(sx * shoulder, ay, az)
        J[s] = dict(C=V(sx * h(0.03), ay, az + h(0.01)), S=S, E=V(sx * elbow, ay, az), W=V(sx * wrist, ay, az),
                    F=V(sx * tip, ay, az),
                    H=V(sx * lx, lyc(crotch), crotch), N=V(sx * lx, lyc(knee) - h(0.01), knee),
                    A=V(sx * lx, lyc(ankle), ankle), B=V(sx * lx, toe_y * 0.45 + heel_y * 0.55 - h(0.02), h(0.02)),
                    T=V(sx * lx, toe_y, h(0.02)))
    return J


def skeleton(J):
    fwd, up = V(0, -1, 0), V(0, 0, 1)
    b = [("Hips", J["hips"], J["spine"], None, fwd),
         ("Spine", J["spine"], J["spine1"], "Hips", fwd),
         ("Spine1", J["spine1"], J["spine2"], "Spine", fwd),
         ("Spine2", J["spine2"], J["neck"], "Spine1", fwd),
         ("Neck", J["neck"], J["head"], "Spine2", fwd),
         ("Head", J["head"], J["top"], "Neck", fwd),
         ("HeadTop_End", J["top"], J["top"] + V(0, 0, 0.08), "Head", fwd)]
    for s, _ in SIDES:
        j = J[s]
        b += [(s + "Shoulder", j["C"], j["S"], "Spine2", up),
              (s + "Arm", j["S"], j["E"], s + "Shoulder", V(0, 1, 0)),
              (s + "ForeArm", j["E"], j["W"], s + "Arm", V(0, 1, 0)),
              (s + "Hand", j["W"], j["F"], s + "ForeArm", V(0, -1, 0)),
              (s + "UpLeg", j["H"], j["N"], "Hips", fwd),
              (s + "Leg", j["N"], j["A"], s + "UpLeg", fwd),
              (s + "Foot", j["A"], j["B"], s + "Leg", up),
              (s + "ToeBase", j["B"], j["T"], s + "Foot", up),
              (s + "Toe_End", j["T"], j["T"] + V(0, -0.05, 0), s + "ToeBase", up)]
    return [(P + n, h, t, P + parent if parent else None, roll) for n, h, t, parent, roll in b]


# ---------------------------------------------------------------- what each part of the surface is

def segment_distance(pts, a, b):
    a, b = np.array(a, np.float32), np.array(b, np.float32)
    ab = b - a
    t = np.clip(((pts - a) @ ab) / max(ab @ ab, 1e-9), 0, 1)
    return np.linalg.norm(pts - (a + t[:, None] * ab), axis=1), t


def nearest_bone(pts, bones, skip=("HeadTop_End", "Toe_End")):
    use = [b for b in bones if not b[0].endswith(skip)]
    d = np.stack([segment_distance(pts, h, t)[0] for _, h, t, _, _ in use], 1)
    k = d.argmin(1)
    return [use[i][0][len(P):] for i in range(len(use))], k, d[np.arange(len(pts)), k]


def classify(co, nr, tri, J, bones, H):
    """A category per triangle, and the bone it lies nearest."""
    label, size = components(tri, len(co))
    c = co[tri].mean(1)
    n = np.cross(co[tri[:, 1]] - co[tri[:, 0]], co[tri[:, 2]] - co[tri[:, 0]])
    n /= np.maximum(np.linalg.norm(n, axis=1), 1e-12)[:, None]
    names, k, _ = nearest_bone(c, bones)
    bone = np.array(names, object)[k]
    rel = relief(co, nr, tri)[tri].mean(1) / H
    cat = np.full(len(tri), ARMOUR)
    h = lambda f: f * H
    small = size[tri[:, 0]] < 0.03 * len(co)
    for i, b in enumerate(bone):
        x, y, z = c[i]
        if small[i]:
            cat[i] = GLOW
        elif b in ("Head",):
            face = n[i, 1] < -0.25 and z < J["head"].z + h(0.045) and y < -h(0.02)
            cat[i] = (GLOW if rel[i] < -0.0025 and z > J["head"].z + h(0.015) else DARK) if face else ARMOUR
        elif b == "Neck":
            cat[i] = DARK
        elif b in ("Hips", "Spine", "Spine1", "Spine2"):
            # The chest plate's lower edge is a V from its tip above the waist out to the shoulders.
            edge = J["waist"] + h(0.04) + 1.1 * abs(x)
            if z < J["waist"] - h(0.08):
                cat[i] = ARMOUR
            elif z < edge:
                cat[i] = LIMB
            elif abs(y) < h(0.05) and z < J["neck"].z:
                cat[i] = DARK
        elif b.endswith(("Arm", "UpLeg", "Leg")) and not b.endswith("ForeArm"):
            cat[i] = LIMB
        elif b.endswith("Hand"):
            cat[i] = BLADE if abs(x) > J["Left"]["F"].x - h(0.022) else HAND
        elif b.endswith(("Foot", "ToeBase")):
            cat[i] = BLADE
    # Shoulder caps, forearm cuffs and knee pads stand out of the limb: armour, not ribs.
    for s, sx in SIDES:
        j = J[s]
        for b, a0, a1, k_ in (("Arm", j["S"], j["E"], 1.35), ("ForeArm", j["E"], j["W"], 1.12),
                              ("UpLeg", j["H"], j["N"], 1.35), ("Leg", j["N"], j["A"], 1.25)):
            m = bone == s + b
            if not m.any():
                continue
            r, t = segment_distance(c[m], a0, a1)
            big = r > np.median(r) * k_
            if b == "Leg":
                big &= t < 0.35
            if b == "ForeArm":
                cat[np.flatnonzero(m)[~big & (t > 0.15)]] = LIMB
            cat[np.flatnonzero(m)[big]] = ARMOUR
    return cat, bone, rel


# ---------------------------------------------------------------- the texture

def rasterise(tri, uv, size):
    """Per texel: the triangle covering it (-1 none) and barycentrics."""
    face = np.full((size, size), -1, np.int64)
    bary = np.zeros((size, size, 3), np.float32)
    px = np.stack([uv[..., 0] * size, (1.0 - uv[..., 1]) * size], -1)
    for i in range(len(tri)):
        a, b, c = px[i]
        x0, y0 = np.floor(np.minimum(np.minimum(a, b), c) - 1).astype(int)
        x1, y1 = np.ceil(np.maximum(np.maximum(a, b), c) + 1).astype(int)
        x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, size), min(y1, size)
        if x1 <= x0 or y1 <= y0:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
        det = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(det) < 1e-12:
            continue
        l0 = ((b[1] - c[1]) * (gx - c[0]) + (c[0] - b[0]) * (gy - c[1])) / det
        l1 = ((c[1] - a[1]) * (gx - c[0]) + (a[0] - c[0]) * (gy - c[1])) / det
        l2 = 1 - l0 - l1
        tol = 0.6 / max(np.abs(a - b).max(), np.abs(b - c).max(), np.abs(c - a).max(), 1.0)
        m = (l0 >= -tol) & (l1 >= -tol) & (l2 >= -tol)
        sub = face[y0:y1, x0:x1]
        m &= sub < 0
        sub[m] = i
        bary[y0:y1, x0:x1][m] = np.stack([l0[m], l1[m], l2[m]], -1)
    return face, bary


def dilate(img, filled, steps):
    img, filled = img.copy(), filled.copy()
    for _ in range(steps):
        acc = np.zeros_like(img)
        cnt = np.zeros(filled.shape, np.float32)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0), (1, 1), (-1, -1), (1, -1), (-1, 1)):
            s = np.roll(np.roll(filled, dy, 0), dx, 1)
            acc += np.roll(np.roll(img, dy, 0), dx, 1) * s[..., None]
            cnt += s
        new = (~filled) & (cnt > 0)
        img[new] = acc[new] / cnt[new][:, None]
        filled |= new
    return img


def paint(co, nr, tri, uv, cat, bone, rel, J, H):
    """The robot's colours in its UV layout, what of them glows, and the category map (per texel)."""
    face, bary = rasterise(tri, uv, TEX)
    on = face >= 0
    f = face[on]
    w = bary[on]
    pos = (co[tri[f]] * w[..., None]).sum(1)
    nrm = (nr[tri[f]] * w[..., None]).sum(1)
    nrm /= np.maximum(np.linalg.norm(nrm, axis=1), 1e-9)[:, None]
    rv = rel[f]
    c = cat[f]
    b = bone[f]
    z = pos[:, 2] / H
    out = np.zeros((len(f), 3), np.float32)
    emit = np.zeros((len(f), 3), np.float32)
    shade = np.clip(1.0 + rv * 60.0, 0.55, 1.15)  # sunken joints darker, edges brighter

    m = c == ARMOUR
    lum = (0.80 + 0.10 * z[m] + 0.06 * nrm[m, 2]) * shade[m]
    out[m] = lum[:, None] * np.array([0.90, 0.92, 0.98])
    m = c == HAND
    out[m] = ((0.74 + 0.08 * nrm[m, 2]) * shade[m])[:, None] * np.array([0.88, 0.90, 0.96])

    # Ribbed limbs: white bands with black grooves across each limb, as on the 2097 sprite.
    m = c == LIMB
    t = np.zeros(len(f), np.float32)
    for s, _ in SIDES:
        j = J[s]
        for name, a0, a1 in (("Arm", j["S"], j["E"]), ("ForeArm", j["E"], j["W"]), ("UpLeg", j["H"], j["N"]),
                             ("Leg", j["N"], j["A"])):
            sel = m & (b == s + name)
            if sel.any():
                ax = np.array(a1 - a0, np.float32)
                t[sel] = (pos[sel] - np.array(a0, np.float32)) @ (ax / np.linalg.norm(ax))
    sel = m & np.isin(b, ["Spine", "Spine1", "Hips", "Spine2"])
    t[sel] = pos[sel, 2]
    ph = (t[m] / (0.021 * H)) % 1.0
    band = (ph > 0.18) & (ph < 0.86)
    lum = np.where(band, 0.95 - 0.35 * np.abs(ph - 0.52) ** 1.5 * 2.0, 0.05) * np.clip(shade[m], 0.8, 1.1)
    out[m] = lum[:, None] * np.array([0.93, 0.95, 1.0])

    m = c == DARK
    out[m] = ((0.05 + 0.03 * nrm[m, 2]) * np.clip(shade[m], 0.7, 1.2))[:, None] * np.array([0.75, 0.85, 1.3])
    m = c == GLOW
    core = np.clip(0.5 + 0.5 * -nrm[m, 1] * 0.6 + rv[m] * 40, 0, 1)
    out[m] = np.stack([0.10 + 0.55 * core ** 2, 0.62 + 0.33 * core, np.ones(m.sum())], -1)
    emit[m] = out[m]
    m = c == BLADE
    edge = np.clip(0.35 + rv[m] * 90 + 0.4 * (1 - np.abs(nrm[m, 2])), 0, 1)
    out[m] = np.stack([0.03 + 0.25 * edge ** 2, 0.22 + 0.62 * edge, 0.55 + 0.45 * edge], -1)
    emit[m] = out[m] * np.clip((edge - 0.55) / 0.4, 0, 1)[:, None]

    img = np.zeros((TEX, TEX, 3), np.float32)
    img[on] = np.clip(out, 0, 1)
    cmap = np.full((TEX, TEX), -1, np.int64)
    cmap[on] = c
    img = dilate(img, on, 8)
    img[~(dilate(on[..., None].astype(np.float32), on, 8)[..., 0] > 0)] = (0.08, 0.09, 0.11)
    glow = np.zeros((TEX, TEX, 3), np.float32)
    glow[on] = np.clip(emit, 0, 1)
    glow = dilate(glow, on, 8)
    return img, glow, cmap


def stealth(look, cmap):
    """The dark paint job: gunmetal armour, the glow kept."""
    dark = look.copy()
    for c, k in ((ARMOUR, (0.30, 0.31, 0.36)), (LIMB, (0.28, 0.29, 0.34)), (HAND, (0.30, 0.30, 0.35)),
                 (BLADE, (0.45, 0.30, 0.55))):
        m = cmap == c
        dark[m] = look[m] * np.array(k)
    return dark


# ---------------------------------------------------------------- decimation, materials, rig

def tri_count(mo):
    dg = bpy.context.evaluated_depsgraph_get()
    me = mo.evaluated_get(dg).to_mesh()
    n = sum(len(p.vertices) - 2 for p in me.polygons)
    mo.evaluated_get(dg).to_mesh_clear()
    return n


def decimate(mo, target):
    before = sum(len(p.vertices) - 2 for p in mo.data.polygons)
    if target <= 0 or before <= target:
        return before, before
    mod = mo.modifiers.new("Decimate", "DECIMATE")
    mod.decimate_type = "COLLAPSE"
    mod.use_symmetry, mod.symmetry_axis = True, "X"
    mod.use_collapse_triangulate = True
    lo, hi = 0.0, 1.0
    for _ in range(18):
        mod.ratio = (lo + hi) / 2
        n = tri_count(mo)
        if abs(n - target) <= 6:
            break
        lo, hi = (mod.ratio, hi) if n < target else (lo, mod.ratio)
    bpy.context.view_layer.objects.active = mo
    bpy.ops.object.modifier_apply(modifier=mod.name)
    return before, sum(len(p.vertices) - 2 for p in mo.data.polygons)


def to_image(name, rgb, srgb=True):
    h, w = rgb.shape[:2]
    buf = np.ones((h, w, 4), np.float32)
    buf[..., :3] = rgb
    img = bpy.data.images.new(name, w, h, alpha=False)
    img.colorspace_settings.name = "sRGB" if srgb else "Non-Color"
    img.pixels.foreach_set(buf[::-1].ravel())
    img.file_format = "PNG"
    img.pack()
    return img


def load_texture(path):
    img = bpy.data.images.load(os.path.abspath(path))
    w, h = img.size
    px = np.array(img.pixels[:], np.float32).reshape(h, w, img.channels)[::-1, :, :3]
    bpy.data.images.remove(img)
    return px


def materials(mo, look, glow, cat):
    """One material per paint choice, all sampling the one texture; the glowing ones emit the glow
    image (glTF emissiveTexture), which the import turns into the game's glow layer."""
    img = to_image("ShadowColours", look)
    gimg = to_image("ShadowGlow", glow)
    me = mo.data
    for name, (_, paint_, metal, rough, glows) in MATERIALS.items():
        m = bpy.data.materials.new(name)
        m.use_nodes = True
        m["de_paint"] = paint_
        nt = m.node_tree
        bsdf = nt.nodes["Principled BSDF"]
        tex = nt.nodes.new("ShaderNodeTexImage")
        tex.image = img
        nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
        bsdf.inputs["Metallic"].default_value = metal
        bsdf.inputs["Roughness"].default_value = rough
        if glows:
            gt = nt.nodes.new("ShaderNodeTexImage")
            gt.image = gimg
            nt.links.new(gt.outputs["Color"], bsdf.inputs["Emission Color"])
            bsdf.inputs["Emission Strength"].default_value = glows
        me.materials.append(m)
    order = [v[0] for v in MATERIALS.values()]
    idx = [order.index(c) for c in cat]
    me.polygons.foreach_set("material_index", idx)
    me.update()
    return np.bincount(idx, minlength=len(MATERIALS))


def armature(bones):
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
    for eb in arm.edit_bones:
        eb.use_deform = not eb.name.endswith(("HeadTop_End", "Toe_End"))
    bpy.ops.object.mode_set(mode="OBJECT")
    return ao


def read_weights(mo):
    names = [g.name for g in mo.vertex_groups]
    W = np.zeros((len(mo.data.vertices), len(names)), np.float32)
    for v in mo.data.vertices:
        for g in v.groups:
            W[v.index, g.group] = g.weight
    return names, W


def parent_to(mo, ao, kind):
    for o in bpy.data.objects:
        o.select_set(o in (mo, ao))
    bpy.context.view_layer.objects.active = ao
    bpy.ops.object.parent_set(type=kind)


def proxy_weights(mo, ao, H):
    """Heat weighting on a watertight voxel remesh of the model, transferred back to it: AI meshes'
    holes and overlapping shells make it fail on the model itself."""
    px = mo.copy()
    px.data = mo.data.copy()
    px.vertex_groups.clear()
    px.modifiers.clear()
    px.parent = None
    bpy.context.scene.collection.objects.link(px)
    rm = px.modifiers.new("Remesh", "REMESH")
    rm.mode, rm.voxel_size = "VOXEL", 0.012 * H
    bpy.context.view_layer.objects.active = px
    bpy.ops.object.modifier_apply(modifier=rm.name)
    parent_to(px, ao, "ARMATURE_AUTO")
    names, W = read_weights(px)
    missed = int((W.sum(1) < 1e-4).sum())
    bpy.context.view_layer.objects.active = mo
    dt = mo.modifiers.new("Transfer", "DATA_TRANSFER")
    dt.object = px
    dt.use_vert_data = True
    dt.data_types_verts = {"VGROUP_WEIGHTS"}
    dt.vert_mapping = "POLYINTERP_NEAREST"
    dt.layers_vgroup_select_src, dt.layers_vgroup_select_dst = "ALL", "NAME"
    while dt != mo.modifiers[0]:
        bpy.ops.object.modifier_move_up(modifier=dt.name)
    bpy.ops.object.modifier_apply(modifier=dt.name)
    print("SHADOW proxy: %d voxel-remesh vertices, %d without weights" % (len(px.data.vertices), missed))
    bpy.data.objects.remove(px)


def weights(mo, ao, bones, H):
    """Blender's automatic weights (on a voxel proxy when they fail on the model), then fixes: loose
    parts and anything left unweighted follow the nearest body bone rigidly, and no limb takes weight
    from the other side's."""
    parent_to(mo, ao, "ARMATURE_AUTO")
    names, W = read_weights(mo)
    direct = int((W.sum(1) < 1e-4).sum())
    if direct > 0.01 * len(W):
        print("SHADOW heat weighting left %d of %d vertices unweighted: weighting a voxel proxy" % (direct, len(W)))
        proxy_weights(mo, ao, H)
        names, W = read_weights(mo)
    me = mo.data
    co, _, tri, _ = arrays(me)
    label, size = components(tri, len(co))
    body = size >= size.max()
    gi = {n: i for i, n in enumerate(names)}
    _, near, _ = nearest_bone(co, bones)
    use = [b for b in bones if not b[0].endswith(("HeadTop_End", "Toe_End"))]
    rigid = np.zeros_like(W)
    rigid[np.arange(len(co)), [gi[use[k][0]] for k in near]] = 1
    side_of = np.array([1 if "Left" in n else -1 if "Right" in n else 0 for n in names])
    x = co[:, 0]
    crotch = next(h for n, h, _, _, _ in bones if n == P + "LeftUpLeg")[2]
    # Below the crotch the legs are apart, so even the inner thighs take nothing from the other leg.
    apart = (np.abs(x) > 0.02 * co[:, 2].max()) | (co[:, 2] < crotch)
    wrong = (side_of[None, :] != 0) & (np.sign(x)[:, None] != side_of[None, :]) & apart[:, None]
    sided = int((wrong & (W > 1e-4)).any(1).sum())
    W[wrong] = 0
    empty = W.sum(1) < 1e-4
    W[empty] = rigid[empty]
    # Each loose part (studs) moves rigidly with the bone that carries the body vertex nearest it.
    fixed = int(empty.sum())
    bco = co[body]
    bW = W[body]
    for lab in np.unique(label[~body]):
        m = label == lab
        cen = co[m].mean(0)
        k = np.linalg.norm(bco - cen, axis=1).argmin()
        W[m] = 0
        W[m, bW[k].argmax()] = 1
    # At most four influences, normalised.
    keep = np.argsort(-W, 1)[:, :4]
    W4 = np.zeros_like(W)
    rows = np.arange(len(co))[:, None]
    W4[rows, keep] = W[rows, keep]
    W4 /= np.maximum(W4.sum(1, keepdims=True), 1e-9)
    for g in mo.vertex_groups:
        g.remove(list(range(len(co))))
    for i, g in enumerate(mo.vertex_groups):
        nz = np.flatnonzero(W4[:, i] > 1e-4)
        for v in nz:
            g.add([int(v)], float(W4[v, i]), "REPLACE")
    return fixed, sided, int((~body).sum())


# ---------------------------------------------------------------- export, renders, picture

def export(path):
    glb = path.lower().endswith(".glb")
    bpy.ops.export_scene.gltf(filepath=os.path.abspath(path), export_format="GLB" if glb else "GLTF_SEPARATE",
                              export_yup=True, export_skins=True, export_animations=True, export_animation_mode="ACTIONS",
                              export_force_sampling=True, export_frame_step=1, export_cameras=False,
                              export_lights=False, export_image_format="AUTO", export_apply=False, export_extras=True,
                              use_selection=False)


def save_png(path, rgb):
    img = to_image(os.path.splitext(os.path.basename(path))[0] + "_out", rgb)
    img.filepath_raw = path
    img.save()


# Armature-space turns per bone (children inherit them): (axis, degrees); the stance is animate_shadow's.
POSES = {
    "guard": {"Hips": ((0, 0, 1), 20), "Spine2": ((0, 0, 1), -10), "LeftArm": [((0, 1, 0), 65), ((0, 0, 1), -35)],
              "LeftForeArm": ((1, 0, 0), -125), "RightArm": [((0, 1, 0), -65), ((0, 0, 1), 35)], "RightForeArm": ((1, 0, 0), -110),
              "LeftUpLeg": [((0, 1, 0), 8), ((1, 0, 0), -20)], "LeftLeg": ((1, 0, 0), 25),
              "RightUpLeg": [((0, 1, 0), -8), ((1, 0, 0), 15)], "RightLeg": ((1, 0, 0), 15), "RightFoot": ((1, 0, 0), -20)},
    "kick": {"Hips": ((0, 0, 1), 60), "Spine": ((0, 1, 0), 12), "Spine2": ((0, 1, 0), 10),
             "LeftArm": [((0, 1, 0), 30), ((0, 0, 1), 30)], "LeftForeArm": ((0, 0, 1), -60),
             "RightArm": [((0, 1, 0), -20), ((0, 0, 1), 10)], "RightForeArm": ((0, 0, 1), 70),
             "LeftUpLeg": [((1, 0, 0), -95), ((0, 1, 0), -30)], "LeftLeg": ((1, 0, 0), 10), "LeftFoot": ((1, 0, 0), -30),
             "RightUpLeg": ((0, 1, 0), 8), "RightLeg": ((1, 0, 0), 8)},
}


def pose(ao, name, drop=0.0):
    if name == "stance":
        ao.location.z = 0
        animate_shadow.apply(ao, {})
        low = animate_shadow.foot_low(ao)
        animate_shadow.apply(ao, animate_shadow.STANCE)
        ao.location.z = low - animate_shadow.foot_low(ao)
        return
    turns = POSES.get(name, {})
    ao.location.z = drop
    world = {}
    for pb in sorted(ao.pose.bones, key=lambda pb: len(pb.bone.parent_recursive)):
        b = pb.bone
        parent = world.get(b.parent.name, Quaternion()) if b.parent else Quaternion()
        t = turns.get(b.name[len(P):])
        D = parent
        for axis, deg in ([t] if isinstance(t, tuple) else t or []):
            D = Quaternion(axis, math.radians(deg)) @ D
        world[b.name] = D
        R = b.matrix_local.to_quaternion()
        pb.rotation_mode = "QUATERNION"
        pb.rotation_quaternion = R.inverted() @ (parent.inverted() @ D) @ R
    bpy.context.view_layer.update()


def stage(H):
    sc = bpy.context.scene
    engines = {e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items}
    sc.render.engine = "BLENDER_EEVEE" if "BLENDER_EEVEE" in engines else "BLENDER_EEVEE_NEXT"
    world = bpy.data.worlds.new("W")
    sc.world = world
    sc.view_settings.view_transform = "Standard"
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.16, 0.17, 0.21, 1)
    for name, loc, energy, colour in (("Key", V(3, -4, 5), 520, (1, 1, 1)), ("Fill", V(-4, -2, 3), 180, (0.8, 0.85, 1)),
                                      ("Rim", V(0, 5, 4), 600, (0.5, 0.85, 1))):
        l = bpy.data.objects.new(name, bpy.data.lights.new(name, "AREA"))
        l.data.energy, l.data.size, l.data.color = energy, 3, colour
        l.location = loc * (H / 2.7)
        l.rotation_euler = (V(0, 0, H * 0.5) - l.location).to_track_quat("-Z", "Y").to_euler()
        sc.collection.objects.link(l)
    bpy.ops.mesh.primitive_plane_add(size=H * 2.2)
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


def preview(prefix, ao, cam, H):
    sc = bpy.context.scene
    sc.render.resolution_x, sc.render.resolution_y = 560, 720
    k = H / 3.1
    for tag, deg in (("front", 0), ("front34", 35), ("side", 90), ("back34", 145), ("back", 180)):
        shoot(cam, "%s_%s.png" % (prefix, tag), deg, 9.4 * k, 1.9 * k, 1.6 * k)
    for tag, name, deg, drop in (("stance", "stance", -55, 0), ("stance_side", "stance", -90, 0), ("guard", "guard", 20, -0.12),
                                 ("kick", "kick", 70, 0.0)):
        pose(ao, name, drop * k)
        shoot(cam, "%s_%s.png" % (prefix, tag), deg, 9.4 * k, 1.8 * k, 1.45 * k)
    pose(ao, "rest")


def picture(path, ao, cam, floor, H):
    """Choose Creature's picture: four 128 x 128 staged poses side by side, as the shipped ones are."""
    sc = bpy.context.scene
    sc.render.resolution_x = sc.render.resolution_y = 256
    floor.hide_render = True
    bg = sc.world.node_tree.nodes["Background"].inputs["Color"]
    old = tuple(bg.default_value)
    bg.default_value = (0.002, 0.003, 0.012, 1)
    k = H / 3.1
    tiles = []
    for n, (name, drop, deg, dist, height, target) in enumerate((("guard", -0.12, 25, 6.2, 1.8, 1.42), ("stance", 0, 300, 5.9, 1.7, 1.35),
                                                                 ("kick", 0.0, 80, 6.4, 1.7, 1.45), ("stance", 0, 330, 5.9, 1.8, 1.35))):
        pose(ao, name, drop * k)
        tile = "%s.tile%d.png" % (path, n)
        shoot(cam, tile, deg, dist * k, height * k, target * k)
        img = bpy.data.images.load(tile)
        px = np.array(img.pixels[:], np.float32).reshape(256, 256, 4)[::-1, :, :3]
        tiles.append(px.reshape(128, 2, 128, 2, 3).mean((1, 3)))
        bpy.data.images.remove(img)
        os.remove(tile)
    img = bpy.data.images.new("ChooseCreature", 512, 128, alpha=False)
    buf = np.ones((128, 512, 4), np.float32)
    buf[..., :3] = np.concatenate(tiles, 1)
    img.pixels.foreach_set(buf[::-1].ravel())
    img.filepath_raw = path
    img.file_format = "PNG"
    img.save()
    floor.hide_render = False
    bg.default_value = old
    pose(ao, "rest")


# ---------------------------------------------------------------- main

def main():
    args = parse_args()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    H = args.height
    mo = load(args.fbx)
    normalise(mo, H)
    co, nr, tri, uv = arrays(mo.data)
    J = joints(co, tri, H)
    bones = skeleton(J)
    before, after = decimate(mo, args.tris)
    co, nr, tri, uv = arrays(mo.data)
    cat, bone, rel = classify(co, nr, tri, J, bones, H)
    look, glow, cmap = paint(co, nr, tri, uv, cat, bone, rel, J, H)
    if args.texture:
        look = load_texture(args.texture)
        if look.shape[:2] != (TEX, TEX):
            ys = (np.arange(TEX) * look.shape[0] / TEX).astype(int)
            xs = (np.arange(TEX) * look.shape[1] / TEX).astype(int)
            look = look[ys][:, xs]
    counts = materials(mo, look, glow, cat)
    ao = armature(bones)
    fixed, sided, loose = weights(mo, ao, bones, H)
    bpy.context.scene.render.fps = animate_shadow.FPS
    actions = animate_shadow.build(ao)
    print("SHADOW actions " + ", ".join(actions))
    vs = np.array([v.co[:] for v in mo.data.vertices])
    print("SHADOW triangles %d -> %d, vertices %d, height %.3f m, width %.3f m" % (before, after, len(vs), np.ptp(vs[:, 2]), np.ptp(vs[:, 0])))
    print("SHADOW materials " + ", ".join("%s %d" % (n, c) for n, c in zip(MATERIALS, counts)))
    print("SHADOW weights: %d vertices left unweighted (now nearest bone), %d had the other side's limbs, %d in loose parts (rigid)" % (fixed, sided, loose))
    print("SHADOW joints " + ", ".join("%s %.2f %.2f %.2f" % ((n[len(P):],) + tuple(h)) for n, h, _, _, _ in bones))
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    export(args.out)
    print("EXPORTED", args.out)
    if args.paint_out:
        os.makedirs(args.paint_out, exist_ok=True)
        save_png(os.path.join(args.paint_out, "ShadowColours.png"), look)
        save_png(os.path.join(args.paint_out, "ShadowDark.png"), stealth(look, cmap))
        save_png(os.path.join(args.paint_out, "ShadowGlow.png"), glow)
    if args.blend:
        bpy.context.preferences.filepaths.save_version = 0
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(args.blend))
    if args.preview or args.picture:
        cam, floor = stage(H)
        if args.preview:
            preview(args.preview, ao, cam, H)
        if args.picture:
            picture(args.picture, ao, cam, floor, H)


main()
