"""AnimationSequenceGroup v4 (Core 0x701bbf70) prototype reader."""
import struct
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import vmc2


def load(path):
    d = open(path, 'rb').read()
    r = vmc2.R(d)
    v = r.u8(); end = r.size()
    assert v == 4, v
    name = r.text()
    seqs = []
    for _ in range(r.i32()):
        sname = r.text()
        streams = []
        for _ in range(r.i32()):
            ch = r.u16()
            sv = r.u8(); send = r.size()
            pos = [struct.unpack_from('<4f', d, r.p + 16 * i) for i in range(r.i32())] if sv == 1 else []
            r.p += 16 * len(pos)
            n = r.i32()
            rot = [struct.unpack_from('<5f', d, r.p + 20 * i) for i in range(n)]
            r.p += 20 * n
            r.p = send
            streams.append((ch, pos, rot))
        seqs.append((sname, streams))
    chans = []
    for _ in range(r.i32()):
        cname = r.text(); b1 = r.boolean(); b2 = r.boolean()
        parent = struct.unpack_from('<h', d, r.p)[0]; r.p += 2
        rest = struct.unpack_from('<3f', d, r.p); r.p += 12
        q = struct.unpack_from('<4f', d, r.p); r.p += 16
        chans.append((cname, b1, b2, parent, rest, q))
    assert r.p == end, (r.p, end)
    return name, seqs, chans


if __name__ == '__main__':
    name, seqs, chans = load(sys.argv[1])
    print(name, len(seqs), 'sequences', len(chans), 'channels')
    for i, c in enumerate(chans):
        print(' ', i, c[0], c[1], c[2], 'parent', c[3], 'rest', [round(x, 2) for x in c[4]], 'q', [round(x, 3) for x in c[5]])
    for s, streams in seqs[:12]:
        nk = sum(len(p) + len(o) for _, p, o in streams)
        tmax = max([k[0] for _, p, o in streams for k in p + o] or [0])
        print(' ', s, len(streams), 'streams', nk, 'keys', 'tmax', round(tmax, 3))
    print('...', [s for s, _ in seqs[12:]])
