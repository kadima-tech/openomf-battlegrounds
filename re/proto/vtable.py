"""Resolve vtable slots: vtable.py <module> <Class> <offset>... using re/coff/<module>.tsv."""
import struct
import sys

mod, cls = sys.argv[1], sys.argv[2]
pe_path = f'source/extracted/App_Executables/Engine/{mod}.ModuleDLL'
d = open(pe_path, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
o = pe + 24 + struct.unpack_from('<H', d, pe + 20)[0]
secs = [struct.unpack_from('<IIII', d, o + 8 + 40 * i) for i in range(nsec)]


def read32(va):
    rva = va - base
    for vs, sva, rs, ro in secs:
        if sva <= rva < sva + max(vs, rs):
            return struct.unpack_from('<I', d, rva - sva + ro)[0]


names = {}
vt = None
for line in open(f're/coff/{mod}.tsv'):
    wrong, right, isfn, name = line.rstrip('\n').split('\t', 3)
    names.setdefault(int(right, 16), name)
    if name == f'??_7{cls}@@6B@':
        vt = int(right, 16)
print(f'{cls} vtable at {vt:08x}')
for off in sys.argv[3:]:
    t = read32(vt + int(off, 0))
    print(f'  +{int(off, 0):#x} -> {t:08x} {names.get(t, "?")}')
