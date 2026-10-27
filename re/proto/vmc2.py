"""Prototype VMC2 (compiled VMCode) reader.

Follows DE_CClass::LoadVMCode (Core 0x70134530), DE_CClassBehaviorReference::LoadVMCode
(0x701370d0) and DE_CClassCompiledMethod::LoadVMCode (0x7013de50).
Usage: vmc2.py <vmc2.bin> <num_behaviors>
"""
import struct
import sys


class R:
    def __init__(self, b, p=0):
        self.b, self.p = b, p

    def u8(self):
        v = self.b[self.p]; self.p += 1; return v

    def u16(self):
        v = struct.unpack_from('<H', self.b, self.p)[0]; self.p += 2; return v

    def u32(self):
        v = struct.unpack_from('<I', self.b, self.p)[0]; self.p += 4; return v

    def i32(self):
        v = struct.unpack_from('<i', self.b, self.p)[0]; self.p += 4; return v

    def sq(self):
        b = self.u8()
        return b if b < 0xFE else (self.u16() if b == 0xFE else self.u32())

    def block(self):
        n = self.u32(); v = self.b[self.p:self.p + n]; self.p += n; return v

    def text(self):
        return self.block().rstrip(b'\0').decode('latin1')

    def boolean(self):
        return self.u8() != 0

    def size(self):
        b = self.u8()
        n = self.u32() if b == 0xFF else b
        return self.p + n


def class_instance(r):
    inst = {'cls': r.sq(), 'u32': r.u32(), 'b': r.boolean(), 'k': r.sq()}
    if inst['k'] and r.boolean():
        inst['cls2'] = r.sq()
    return inst


def compiled_method(r):
    end = r.size()
    m = {}
    if not r.boolean():
        r.p = end
        return None
    m['code'] = r.block()          # +0x98 DataBlock
    m['contents'] = r.block()      # +0x64 Contents
    m['flags'] = [r.boolean() for _ in range(4)]  # +0x84 +0x88 +0x8c +0x90
    if m['flags'][2]:
        m['v34'] = r.sq()
    if m['flags'][3]:
        m['base_methods'] = [r.sq() for _ in range(r.sq())]
    m['instances'] = [class_instance(r) for _ in range(r.sq())]
    m['interfaces'] = [(r.u32(), r.i32(), r.boolean()) for _ in range(r.u32())]
    m['counts'] = [r.sq() for _ in range(5)]  # +0x3c +0x94 +0x40 +0x44 +0x48
    m['signature'] = r.text()
    m['v78'] = r.i32()
    m['replicated'] = [(r.sq(), r.sq(), r.sq()) for _ in range(r.sq())]
    if r.p != end:
        m['trailing'] = end - r.p
    r.p = end
    return m


def behavior(r):
    end = r.size()
    b = {'instances': [class_instance(r) for _ in range(r.sq())]}
    n = r.sq()
    b['methods'] = [compiled_method(r) for _ in range(n)]
    b['links'] = []
    for _ in range(n):
        b['links'].append(r.sq() if r.boolean() else None)
    b['replicated'] = [(r.sq(), r.sq(), r.sq()) for _ in range(r.sq())]
    if r.p != end:
        b['trailing'] = end - r.p
    r.p = end
    return b


def vmc2(data, nbehaviors):
    r = R(data)
    version = r.u8()
    end = r.size()
    assert version == 13, version
    c = {'change_info': r.block()}
    c['classes'] = [(r.text(), r.sq()) for _ in range(r.i32())]
    c['intrinsics'] = [(r.sq(), r.text()) for _ in range(r.i32())]
    c['ref_members'] = [r.sq() for _ in range(r.i32())]
    c['behaviors'] = [behavior(r) for _ in range(nbehaviors)]
    c['global_refs'] = [(r.u32(), r.text()) for _ in range(r.i32())]
    c['member_interfaces'] = [(r.u32(), r.i32()) for _ in range(r.i32())]
    c['final_instances'] = r.i32()  # entries need native per-class readers (vtable +0xE0)
    c['remaining'] = end - r.p
    return c


if __name__ == '__main__':
    data = open(sys.argv[1], 'rb').read()
    c = vmc2(data, int(sys.argv[2]))
    print('classes', len(c['classes']), [n for n, _ in c['classes']])
    print('intrinsics', len(c['intrinsics']))
    for cls, sig in c['intrinsics'][:8]:
        print('   ', c['classes'][cls][0] if cls < len(c['classes']) else cls, sig)
    print('ref_members', c['ref_members'])
    for b in c['behaviors']:
        print('behavior: instances', len(b['instances']), 'methods', len(b['methods']), 'trailing', b.get('trailing'))
        for m in b['methods'][:6]:
            if m:
                print('   ', m['signature'], 'code', len(m['code']), 'contents', len(m['contents']),
                      'flags', m['flags'], 'counts', m['counts'], 'trailing', m.get('trailing'))
    print('global_refs', len(c['global_refs']), 'member_interfaces', len(c['member_interfaces']),
          'final_instances', c['final_instances'], 'remaining', c['remaining'])
