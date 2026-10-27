"""Flat-shaded software render of Mesh\\Standard extended contents to PNG (verification only).
Usage: render_mesh.py <mesh ObjExtCon> <out.png> [yaw] [texturemanager ObjExtCon] [texture.png]"""
import math
import struct
import sys
import zlib

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import vmc2


def load(path):
    data = open(path, 'rb').read()
    r = vmc2.R(data)
    r.u8(); r.size()
    nv = r.u16()
    verts = [struct.unpack_from('<3f', data, r.p + 12 * i) for i in range(nv)]
    r.p += 12 * nv
    faces = []
    for _ in range(r.u16()):
        c = [(r.u16(), r.u16()) for _ in range(3)]
        r.u32()
        faces.append([a for a, _ in c])
    return verts, faces


def load_uv(path):
    """TextureManager ObjExtCon (Core 0x70131f80): v1, i32 n, n * {u32 face, 3 * (f32 u, f32 v)}."""
    data = open(path, 'rb').read()
    r = vmc2.R(data)
    r.u8(); r.size()
    uv = {}
    for _ in range(r.i32()):
        face = r.u32()
        uv[face] = [struct.unpack_from('<2f', data, r.p + 8 * k) for k in range(3)]
        r.p += 24
    return uv


def load_png(path):
    d = open(path, 'rb').read()
    w, h = struct.unpack('>II', d[16:24])
    p, idat = 8, b''
    while p < len(d):
        n = struct.unpack('>I', d[p:p + 4])[0]; t = d[p + 4:p + 8]
        if t == b'IDAT': idat += d[p + 8:p + 8 + n]
        p += 12 + n
    raw = zlib.decompress(idat)
    rows = [raw[y * (w * 4 + 1) + 1:(y + 1) * (w * 4 + 1)] for y in range(h)]  # filter 0 only (ours)
    return w, h, b''.join(rows)


def render(verts, faces, size=512, yaw=30.0, pitch=-15.0, uv=None, tex=None):
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    def xf(v):
        x, y, z = v
        x, z = x * cy - y * sy, z
        y = v[0] * sy + v[1] * cy
        y, z = y * cp - z * sp, y * sp + z * cp
        return (x, z, y)  # screen x, screen up = z, depth = y
    pts = [xf(v) for v in verts]
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    span = max(max(xs) - min(xs), max(ys) - min(ys)) or 1
    sc = size * 0.9 / span
    ox = size / 2 - (max(xs) + min(xs)) / 2 * sc
    oy = size / 2 + (max(ys) + min(ys)) / 2 * sc
    scr = [(p[0] * sc + ox, -p[1] * sc + oy, p[2]) for p in pts]
    img = bytearray(b'\x20\x20\x28\xff' * size * size)
    zbuf = [1e30] * (size * size)
    light = (0.4, -0.5, 0.75)
    for fi, f in enumerate(faces):
        a, b, c = (pts[i] for i in f)
        u = [b[k] - a[k] for k in range(3)]; w = [c[k] - a[k] for k in range(3)]
        n = (u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0])
        ln = math.sqrt(sum(x * x for x in n)) or 1
        shade = abs(sum(n[k] / ln * light[k] for k in range(3)))
        col = (int(60 + 180 * shade), int(30 + 60 * shade), int(30 + 50 * shade))
        A, B, C = (scr[i] for i in f)
        minx = max(int(min(A[0], B[0], C[0])), 0); maxx = min(int(max(A[0], B[0], C[0])) + 1, size - 1)
        miny = max(int(min(A[1], B[1], C[1])), 0); maxy = min(int(max(A[1], B[1], C[1])) + 1, size - 1)
        den = (B[1] - C[1]) * (A[0] - C[0]) + (C[0] - B[0]) * (A[1] - C[1])
        if abs(den) < 1e-9:
            continue
        for y in range(miny, maxy + 1):
            for x in range(minx, maxx + 1):
                l1 = ((B[1] - C[1]) * (x - C[0]) + (C[0] - B[0]) * (y - C[1])) / den
                l2 = ((C[1] - A[1]) * (x - C[0]) + (A[0] - C[0]) * (y - C[1])) / den
                l3 = 1 - l1 - l2
                if l1 < 0 or l2 < 0 or l3 < 0:
                    continue
                z = l1 * A[2] + l2 * B[2] + l3 * C[2]
                i = y * size + x
                if z < zbuf[i]:
                    zbuf[i] = z
                    if uv is not None and fi in uv and tex:
                        tw, th, px = tex
                        (u0, v0), (u1, v1), (u2, v2) = uv[fi]
                        u = l1 * u0 + l2 * u1 + l3 * u2
                        v = l1 * v0 + l2 * v1 + l3 * v2
                        tx = int(u * tw) % tw; ty = int(v * th) % th
                        k = (ty * tw + tx) * 4
                        lit = 0.45 + 0.55 * shade
                        img[i * 4:i * 4 + 3] = bytes(min(255, int(px[k + j] * lit)) for j in range(3))
                    else:
                        img[i * 4:i * 4 + 3] = bytes(col)
    return size, img


def png(path, w, h, rgba):
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    verts, faces = load(sys.argv[1])
    uv = load_uv(sys.argv[4]) if len(sys.argv) > 5 else None
    tex = load_png(sys.argv[5]) if len(sys.argv) > 5 else None
    size, img = render(verts, faces, yaw=float(sys.argv[3]) if len(sys.argv) > 3 else 30.0, uv=uv, tex=tex)
    png(sys.argv[2], size, size, img)
    print(len(verts), 'verts', len(faces), 'faces ->', sys.argv[2])
