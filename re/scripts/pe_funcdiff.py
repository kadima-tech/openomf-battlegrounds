"""Function-level diff of two builds of a DE module DLL (PATCHES.md), by COFF symbol name.

usage: python3 -I pe_funcdiff.py <old.ModuleDLL> <new.ModuleDLL> [detail dir]

Each function (COFF type 0x20) spans to the next function symbol. Its instructions are compared
after normalising relocated operands and branch targets to symbol+offset, so code that only moved
compares equal. Prints added / removed / changed functions with old and new addresses; with a
detail dir, writes each changed function's normalised old/new listing there (keep out of git).
Needs capstone.
"""
import bisect, os, struct, sys
import capstone


def load(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec, _, symptr, nsym = struct.unpack_from('<HIII', d, pe + 6)
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    reloc_rva, reloc_size = struct.unpack_from('<II', d, pe + 24 + 96 + 5 * 8)
    secs = []
    o = pe + 24 + optsz
    for _ in range(nsec):
        name = d[o:o + 8].rstrip(b'\0').decode('latin1')
        vs, va, rs, rp = struct.unpack_from('<IIII', d, o + 8)
        secs.append((name, va, vs, rp, rs))
        o += 40

    def rva2off(rva):
        for _, va, vs, rp, rs in secs:
            if va <= rva < va + max(vs, rs):
                return rp + rva - va
        return None

    syms, funcs = [], []
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
        if 1 <= secnum <= nsec:
            addr = base + value
            syms.append((addr, name))
            if typ == 0x20:
                funcs.append((addr, name))
        i += 1 + naux
    syms.sort()
    funcs.sort()
    relocs = set()
    p = rva2off(reloc_rva) if reloc_rva else None
    end = p + reloc_size if p is not None else 0
    while p is not None and p < end:
        page, size = struct.unpack_from('<II', d, p)
        if size < 8:
            break
        for k in range((size - 8) // 2):
            w = struct.unpack_from('<H', d, p + 8 + 2 * k)[0]
            if w >> 12 == 3:
                relocs.add(base + page + (w & 0xfff))
        p += size
    return dict(data=d, base=base, secs=secs, rva2off=rva2off, syms=syms, sym_addrs=[a for a, _ in syms],
                funcs=funcs, relocs=relocs)


def symname(m, v):
    i = bisect.bisect_right(m['sym_addrs'], v) - 1
    if i < 0:
        return f'?{v:#x}'
    a, n = m['syms'][i]
    if v - a > 0x400:
        for name, va, vs, _, _ in m['secs']:
            if va <= v - m['base'] < va + vs:
                return 'anon' + name
    if v == a and (n.startswith('$SG') or n.startswith('??_C@')):
        off = m['rva2off'](a - m['base'])
        if off is not None:
            raw = m['data'][off:off + 256].split(b'\0')[0]
            n = 'str' + repr(raw.decode('latin1'))
    elif n.startswith('_$') or n.startswith('$'):
        n = n[:3]
    return n if v == a else f'{n}+{v - a:#x}'


def listing(m, start, end):
    off = m['rva2off'](start - m['base'])
    code = m['data'][off:off + (end - start)]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    out = []
    for ins in md.disasm(code, start):
        text = ins.op_str
        for r in range(ins.address, ins.address + ins.size - 3):
            if r in m['relocs']:
                v = struct.unpack_from('<I', code, r - start)[0]
                text = text.replace(hex(v), '<' + symname(m, v) + '>')
        if ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL):
            if ins.operands and ins.operands[0].type == capstone.x86.X86_OP_IMM:
                t = ins.operands[0].imm
                text = f'.+{t - start:#x}' if start <= t < end else '<' + symname(m, t) + '>'
        out.append(f'{ins.mnemonic} {text}'.strip())
    return out


def functions(m):
    f = m['funcs']
    out = {}
    anon = 0
    for i, (a, n) in enumerate(f):
        if n.startswith('_$E'):
            n = f'_$E#{anon}'
            anon += 1
        nxt = f[i + 1][0] if i + 1 < len(f) else a + 16
        nxt = min(nxt, a + 0x20000)
        k = n
        while k in out:
            k += '~'
        out[k] = (a, nxt)
    return out


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    detail = sys.argv[3] if len(sys.argv) > 3 else None
    if detail:
        os.makedirs(detail, exist_ok=True)
    fa, fb = functions(a), functions(b)
    print(f'functions {len(fa)} -> {len(fb)}')
    ia = {tuple(listing(a, *fa[n])) for n in fa if n.startswith('_$E')}
    ib = {tuple(listing(b, *fb[n])) for n in fb if n.startswith('_$E')}
    for t in sorted(ia - ib):
        print('static initializer removed: ' + '; '.join(t[:3]))
    for t in sorted(ib - ia):
        print('static initializer added: ' + '; '.join(t[:3]))
    fa = {n: v for n, v in fa.items() if not n.startswith('_$E')}
    fb = {n: v for n, v in fb.items() if not n.startswith('_$E')}
    for n in sorted(set(fa) - set(fb)):
        print(f'removed {fa[n][0]:#010x} {n}')
    for n in sorted(set(fb) - set(fa)):
        print(f'added   {fb[n][0]:#010x} {n}')
    changed = 0
    for n in sorted(set(fa) & set(fb)):
        la, lb = listing(a, *fa[n]), listing(b, *fb[n])
        while la and la[-1] in ('int3', 'nop'):
            la.pop()
        while lb and lb[-1] in ('int3', 'nop'):
            lb.pop()
        if la != lb:
            changed += 1
            print(f'changed {fa[n][0]:#010x} -> {fb[n][0]:#010x} {n} ({len(la)} -> {len(lb)} insns)')
            if detail:
                safe = ''.join(c if c.isalnum() or c in '_@?$' else '_' for c in n)[:150]
                open(os.path.join(detail, safe + '.old.s'), 'w').write('\n'.join(la) + '\n')
                open(os.path.join(detail, safe + '.new.s'), 'w').write('\n'.join(lb) + '\n')
    print(f'{changed} changed')


if __name__ == '__main__':
    main()
