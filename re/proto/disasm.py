"""Disassemble/validate VMC2 method bytecode across re/vmc2. Usage: disasm.py [--show <bin>]"""
import collections
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import opcodes
import vmc2

OPS = opcodes.TABLE


def count_behaviors(refs):
    r = vmc2.R(refs)
    r.u8(); r.p = r.size()          # DE_CObject::ObjProcessReferences chunk
    v = r.u8(); end = r.size()      # DE_CClass chunk
    n = 0
    while r.p < end and r.boolean():
        tag = r.u8()
        if tag in (1, 3): r.u16()
        elif tag == 2: r.u8()
        elif tag == 4: r.u32()
        n += 1
    return n


FMT = {'i32': '<i', 'dword': '<I', 'int': '<i', 'float': '<f', 'byte': '<B', 'char': '<b', 'word': '<H', 'short': '<h'}


def disasm(code):
    i, out = 0, []
    while i + 4 <= len(code):
        op = struct.unpack_from('<i', code, i)[0]
        if op not in OPS:
            return out, f'unknown opcode {op} at {i}'
        size, layout, _ = OPS[op]
        args, p = [], i + 4
        for t in layout:
            if t == 'word?10':
                if args[-1] != 10:
                    continue
                t = 'word'
            if t == 'blob':
                n = args[-1]
                args.append(code[p:p + n])
                p += n
                continue
            if p + struct.calcsize(FMT[t]) > len(code):
                return out, f'operand past end at {i}'
            args.append(struct.unpack_from(FMT[t], code, p)[0])
            p += struct.calcsize(FMT[t])
        out.append((i, op, args))
        i = p
    if i != len(code):
        return out, f'stopped at {i} of {len(code)} bytes'
    return out, None


if __name__ == '__main__':
    if '--show' in sys.argv:
        f = sys.argv[sys.argv.index('--show') + 1]
        c = vmc2.vmc2(open(f, 'rb').read(), count_behaviors(open(f[:-4] + '.refs', 'rb').read()))
        for b in c['behaviors']:
            for m in b['methods']:
                if not m:
                    continue
                print('==', m['signature'])
                ins, err = disasm(m['contents'])
                for at, op, args in ins:
                    print(f'  {at:4} op{op:<4} {" ".join(map(str, args))}')
                if err:
                    print('  !!', err)
        sys.exit()
    ok = bad = parse_fail = 0
    errs = collections.Counter()
    used = collections.Counter()
    for f in sorted(glob.glob(os.path.join(os.path.dirname(__file__), '../vmc2/*/*.bin'))):
        try:
            c = vmc2.vmc2(open(f, 'rb').read(), count_behaviors(open(f[:-4] + '.refs', 'rb').read()))
        except Exception as e:
            parse_fail += 1
            errs['parse: ' + str(e)[:60]] += 1
            continue
        for b in c['behaviors']:
            for m in b['methods']:
                if not m:
                    continue
                ins, err = disasm(m['contents'])
                for _, op, _ in ins:
                    used[op] += 1
                if err:
                    bad += 1; errs[err.split(' at ')[0] if 'unknown' in err else 'length mismatch'] += 1
                    if bad <= 5: print(os.path.basename(f), m['signature'], err)
                else:
                    ok += 1
    print(f'methods ok {ok}, bad {bad}, vmc2 parse failures {parse_fail}')
    print(errs.most_common(10))
    print('distinct opcodes used:', len(used))
