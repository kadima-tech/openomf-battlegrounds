"""Compare two builds of the SafeDisc-wrapped OMFBG.exe using only its readable parts (PATCHES.md).

usage: python3 -I launcher_diff.py <old OMFBG.exe> <new OMFBG.exe>

Reads the COFF symbol table (left intact by the wrapper) and the import names in .rdata; the
encrypted .text / .data are never touched, decrypted or executed.
"""
import re, struct, sys


def load(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec, ts, symptr, nsym = struct.unpack_from('<HIII', d, pe + 6)
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    secs = {}
    o = pe + 24 + optsz
    for _ in range(nsec):
        name = d[o:o + 8].rstrip(b'\0').decode('latin1')
        vs, va, rs, rp = struct.unpack_from('<IIII', d, o + 8)
        secs[name] = (va, vs, rp, rs)
        o += 40
    funcs, data = set(), set()
    strtab = symptr + nsym * 18
    i = 0
    while i < nsym:
        e = symptr + 18 * i
        raw = d[e:e + 8]
        if raw[:4] == b'\0\0\0\0':
            off = struct.unpack_from('<I', raw, 4)[0]
            name = d[strtab + off:d.index(b'\0', strtab + off)].decode('latin1')
        else:
            name = raw.rstrip(b'\0').decode('latin1')
        value, secnum, typ, cls, naux = struct.unpack_from('<IhHBB', d, e + 8)
        if secnum > 0 and cls == 2:
            (funcs if typ == 0x20 else data).add(name)
        i += 1 + naux
    imports = set()
    if '.rdata' in secs:
        va, vs, rp, rs = secs['.rdata']
        for m in re.finditer(rb'[\x20-\x7e]{4,}', d[rp:rp + rs]):
            s = m.group().decode()
            if s.startswith('?') or re.fullmatch(r'_?[A-Za-z][A-Za-z0-9_@]+', s):
                imports.add(s)
    return ts, len(d), funcs, data, imports


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    print(f'timestamp {a[0]:#x} -> {b[0]:#x}, size {a[1]} -> {b[1]}')
    for label, x, y in (('function', a[2], b[2]), ('data symbol', a[3], b[3]), ('.rdata name', a[4], b[4])):
        for n in sorted(x - y):
            print(f'{label} removed {n}')
        for n in sorted(y - x):
            print(f'{label} added {n}')


if __name__ == '__main__':
    main()
