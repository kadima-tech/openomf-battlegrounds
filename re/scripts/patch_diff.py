"""Summarise what changed between two reconstructed install trees (PATCHES.md).

usage: python3 -I patch_diff.py <old tree> <new tree> <deobj-diff binary> [detail dir]

Lists added / removed / modified files; for .DEObjects runs deobj-diff; for PE files compares
timestamps, section sizes and exported / COFF symbol names. Prints names and sizes only.
"""
import hashlib, os, subprocess, sys


def files(root):
    out = {}
    for d, _, fs in os.walk(root):
        for f in fs:
            p = os.path.join(d, f)
            out[os.path.relpath(p, root)] = p
    return out


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def pe_summary(p):
    import struct
    d = open(p, 'rb').read()
    if d[:2] != b'MZ':
        return None
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec, ts, symptr, nsym = struct.unpack_from('<HIII', d, pe + 6)
    opt = struct.unpack_from('<H', d, pe + 20)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + opt + 40 * i
        name = d[o:o + 8].rstrip(b'\0').decode('latin1')
        vsize = struct.unpack_from('<I', d, o + 8)[0]
        secs.append((name, vsize))
    syms = set()
    if symptr and nsym:
        strtab = symptr + 18 * nsym
        i = 0
        while i < nsym:
            o = symptr + 18 * i
            if d[o:o + 4] == b'\0\0\0\0':
                off = struct.unpack_from('<I', d, o + 4)[0]
                e = d.index(b'\0', strtab + off)
                name = d[strtab + off:e].decode('latin1')
            else:
                name = d[o:o + 8].rstrip(b'\0').decode('latin1')
            sect = struct.unpack_from('<h', d, o + 12)[0]
            cls = d[o + 16]
            if sect > 0 and cls == 2:
                syms.add(name)
            i += 1 + d[o + 17]
    return ts, secs, syms


def main():
    old, new, tool = sys.argv[1:4]
    detail = sys.argv[4] if len(sys.argv) > 4 else None
    fo, fn = files(old), files(new)
    for k in sorted(set(fo) - set(fn)):
        print(f'REMOVED {k}')
    for k in sorted(set(fn) - set(fo)):
        print(f'ADDED   {k} ({os.path.getsize(fn[k])} bytes)')
    for k in sorted(set(fo) & set(fn)):
        if sha(fo[k]) == sha(fn[k]):
            continue
        print(f'CHANGED {k} ({os.path.getsize(fo[k])} -> {os.path.getsize(fn[k])} bytes)')
        if k.endswith('.DEObjects'):
            args = [tool, fo[k], fn[k]]
            if detail:
                args.append(os.path.join(detail, os.path.basename(k).replace(' ', '_')))
            r = subprocess.run(args, capture_output=True, text=True)
            for line in r.stdout.splitlines():
                print('    ' + line)
            if r.returncode:
                print('    deobj-diff failed: ' + r.stderr.strip()[:200])
        else:
            a, b = pe_summary(fo[k]), pe_summary(fn[k])
            if a and b:
                print(f'    timestamp {a[0]:#x} -> {b[0]:#x}')
                sa, sb = dict(a[1]), dict(b[1])
                for s in sorted(set(sa) | set(sb)):
                    if sa.get(s) != sb.get(s):
                        print(f'    section {s}: {sa.get(s)} -> {sb.get(s)}')
                for s in sorted(a[2] - b[2]):
                    print(f'    symbol removed {s}')
                for s in sorted(b[2] - a[2]):
                    print(f'    symbol added {s}')


if __name__ == '__main__':
    main()
