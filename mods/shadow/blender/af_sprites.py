import struct, sys, os
import numpy as np
import zlib


def save_png(path, rgba):
    h, w = rgba.shape[:2]
    raw = b''.join(b'\0' + rgba[y].tobytes() for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


class R:
    def __init__(s, b): s.b, s.p = b, 0
    def u8(s): v = s.b[s.p]; s.p += 1; return v
    def u16(s): v = struct.unpack_from('<H', s.b, s.p)[0]; s.p += 2; return v
    def i16(s): v = struct.unpack_from('<h', s.b, s.p)[0]; s.p += 2; return v
    def u32(s): v = struct.unpack_from('<I', s.b, s.p)[0]; s.p += 4; return v
    def buf(s, n): v = s.b[s.p:s.p + n]; s.p += n; return v


def sprite(r):
    ln = r.u16(); x = r.i16(); y = r.i16(); w = r.u16(); h = r.u16(); idx = r.u8(); missing = r.u8()
    data = None if missing else r.buf(ln)
    return dict(x=x, y=y, w=w, h=h, idx=idx, data=data)


def anim(r):
    sx = r.i16(); sy = r.i16(); r.u32(); cc = r.u16(); sc = r.u8()
    r.buf(cc * 4)
    n = r.u16(); s = r.buf(n).decode('latin1'); r.u8()
    ex = r.u8()
    for _ in range(ex):
        n = r.u16(); r.buf(n); r.u8()
    return s, [sprite(r) for _ in range(sc)]


def decode(sp):
    img = np.full((sp['h'], sp['w']), -1, np.int16)
    d, i, x, y = sp['data'], 0, 0, 0
    while i + 1 < len(d):
        c = d[i] | d[i + 1] << 8; i += 2
        op, n = c & 3, c >> 2
        if op == 0: x = n
        elif op == 2: y = n
        elif op == 1:
            for k in range(n):
                if 0 <= y < sp['h'] and 0 <= x < sp['w']: img[y, x] = d[i]
                i += 1; x += 1
        else: break
    return img


def main(path, out):
    b = open(path, 'rb').read()
    r = R(b); r.p = 29
    os.makedirs(out, exist_ok=True)
    hist = np.zeros(256, int)
    while r.p < len(b):
        mid = r.u8()
        if mid >= 70: break
        s, sps = anim(r)
        r.buf(42)
        n = r.u16()
        r.buf(n)
        print(mid, len(sps), s[:60])
        for k, sp in enumerate(sps):
            if sp['data'] is None or sp['w'] == 0: continue
            img = decode(sp)
            v = img.astype(int)
            m = img >= 0
            np.add.at(hist, v[m], 1)
            shade = ((v % 16) * 17).clip(0, 255)
            band = v // 16
            rgb = np.zeros(img.shape + (4,), np.uint8)
            rgb[..., 0] = np.where(band == 0, shade, np.where(band == 1, shade // 3, shade // 2))
            rgb[..., 1] = np.where(band == 0, shade, np.where(band == 1, shade, shade // 2))
            rgb[..., 2] = np.where(band == 0, shade, np.where(band == 1, 255, shade))
            rgb[..., 3] = m * 255
            save_png(f"{out}/m{mid:02d}_{k:02d}.png", np.ascontiguousarray(rgb.repeat(3, 0).repeat(3, 1)))
    print("index histogram:", {i: int(c) for i, c in enumerate(hist) if c})


main(sys.argv[1], sys.argv[2])
