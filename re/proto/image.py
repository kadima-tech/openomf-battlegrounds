"""Decode Image\\Standard objects to PNG (prototype). Usage: image.py <DEObjects> <object> <out.png>

Contents: DE_CImage chunk (Core 0x7018d850) then DE_CStandardImage chunk (0x70194420).
Pixels: EX entry keyed by mip level (u16), DE_CStandardImageCompressor::Load (0x7018b4b0):
u8 version; v>1: Contents (u32 len) passed through DE_CContents::Decompress; then
repeat [i32 channel, ResX*ResY bytes] until channel < 0. Channels are planar 8-bit.
"""
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import vmc2  # R reader


def deobjects_ex(path, obj):
    out = subprocess.run([__import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)), '../../engine/build/deobj-dump'), 'toc+', path, ],
                         capture_output=True, text=True).stdout
    lines = out.split('\n')
    for i, l in enumerate(lines):
        if l.split('\t')[0] == obj:
            ex = []
            for e in lines[i + 1:]:
                if not e.startswith('    '):
                    break
                if e.startswith('    ex '):
                    key = e.split('[')[1].split(']')[0]
                    val = int(e.split('value=')[1].split()[0])
                    ex.append((key, val))
            return int(l.split('off=')[1].split('\t')[0], 16), ex
    raise SystemExit('no object ' + obj)


def lzrw3a_decompress(src, n):
    sys.path.insert(0, __file__.rsplit('/', 1)[0])
    import lzrw3a
    return lzrw3a.decompress(src, n)


def contents_decompress(b):
    r = vmc2.R(b)
    m = r.sq()
    if m == 0:
        return r.block()
    if m == 7:
        r.p += 16
    n = r.sq()
    return lzrw3a_decompress(b[r.p:], n)


def png(path, w, h, rgba):
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    path, obj, out = sys.argv[1:4]
    d = open(path, 'rb').read()
    off, ex = deobjects_ex(path, obj)
    r = vmc2.R(d, off)
    contents = r.block()
    c = vmc2.R(contents)
    v = c.u8(); end = c.size()
    name = c.text(); typ = c.i32(); w = c.i32(); h = c.i32()
    print('image v', v, name, 'type', typ, 'size', w, 'x', h)
    key, val = next(e for e in ex if e[0] == '0000')
    p = vmc2.R(d, val)
    flag = p.u8(); payload = p.block()
    q = vmc2.R(payload)
    ver = q.u8()
    data = contents_decompress(q.block()) if ver > 1 else payload[1:]
    q = vmc2.R(data)
    chans = {}
    while True:
        ch = q.i32()
        if ch < 0:
            break
        chans[ch] = data[q.p:q.p + w * h]; q.p += w * h
    print('channels', sorted(chans))
    rgba = bytearray(w * h * 4)
    for i in range(w * h):
        for k in range(3):
            rgba[i * 4 + k] = chans.get(k, chans.get(0, b'\0' * (w * h)))[i]
        rgba[i * 4 + 3] = chans[3][i] if 3 in chans else 255
    png(out, w, h, bytes(rgba))
    print('wrote', out)
