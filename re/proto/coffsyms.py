"""Parse the COFF symbol table left in a linked PE image. Usage: coffsyms.py <pe> [--tsv]"""
import collections
import struct
import sys


def parse(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    symptr, nsym = struct.unpack_from('<II', d, pe + 12)
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    o = pe + 24 + optsz
    secs = []
    for i in range(nsec):
        name = d[o:o + 8].rstrip(b'\0').decode()
        vs, va = struct.unpack_from('<II', d, o + 8)
        secs.append((name, va, vs))
        o += 40
    strtab = symptr + nsym * 18
    syms = []
    i = 0
    while i < nsym:
        e = symptr + i * 18
        raw = d[e:e + 8]
        if raw[:4] == b'\0\0\0\0':
            off = struct.unpack_from('<I', raw, 4)[0]
            name = d[strtab + off:d.index(b'\0', strtab + off)].decode('latin1')
        else:
            name = raw.rstrip(b'\0').decode('latin1')
        value, secnum, typ, cls, naux = struct.unpack_from('<IhHBB', d, e + 8)
        syms.append((name, value, secnum, typ, cls, naux))
        i += 1 + naux
    return base, secs, syms


if __name__ == '__main__':
    base, secs, syms = parse(sys.argv[1])
    if '--tsv' in sys.argv:
        for name, value, secnum, typ, cls, naux in syms:
            if 1 <= secnum <= len(secs) and typ == 0x20:  # function
                print(f'{base + secs[secnum - 1][1] + value:08x}\t{cls}\t{name}')
        sys.exit()
    print('sections:', [(n, hex(va)) for n, va, _ in secs])
    print('symbols:', len(syms))
    print('by section:', collections.Counter(s[2] for s in syms).most_common(10))
    print('by type:', collections.Counter(s[3] for s in syms).most_common(5))
    print('by class:', collections.Counter(s[4] for s in syms).most_common(8))
    for s in syms[:5] + [s for s in syms if 'ObjProcessContents@DE_CTransferAgent@' in s[0]]:
        print(s)


def fixups(path):
    """(name, ghidra_va, true_va) for every section-bound symbol. Ghidra adds the
    section VA to the COFF value, but in these DE images the value is already an RVA."""
    base, secs, syms = parse(path)
    out = []
    for name, value, secnum, typ, cls, naux in syms:
        if 1 <= secnum <= len(secs) and not name.startswith('.') and name != '@comp.id':
            out.append((name, base + secs[secnum - 1][1] + value, base + value, typ == 0x20))
    return out
