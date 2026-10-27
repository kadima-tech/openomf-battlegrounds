"""Build re/native-api.tsv: every intrinsic registered with DE_CPCNodeClass::NewMethod.

For each module, scans the decompiled registration code for
    node = DE_CPCNodeClass::NewMethod(<ClassNode>, <name sym>, <Caller>)
    p = DE_CPCNodeMethod::NewParameter(node, <name sym>); p+0x3c = <type>
and resolves _SG string symbols through the corrected COFF table + strings.tsv.
"""
import os
import re
import struct

RE = os.path.join(os.path.dirname(__file__), '..')
MODULES = ['Core', 'OMF', 'Creature', 'Session', 'Direct3D', 'Input2', 'AudioPlayer', 'Particles', 'GameSpy']


def load_strings(mod):
    """Symbol name -> string value, using the true COFF addresses."""
    pe = open(os.path.join(RE, '../source/extracted/App_Executables/Engine', mod + '.ModuleDLL'), 'rb').read()
    h = struct.unpack_from('<I', pe, 0x3c)[0]
    base = struct.unpack_from('<I', pe, h + 24 + 28)[0]
    n = struct.unpack_from('<H', pe, h + 6)[0]
    o = h + 24 + struct.unpack_from('<H', pe, h + 20)[0]
    secs = [struct.unpack_from('<IIII', pe, o + 8 + 40 * i) for i in range(n)]

    def cstr(va):
        rva = va - base
        for vs, sva, rs, ro in secs:
            if sva <= rva < sva + rs:
                p = rva - sva + ro
                return pe[p:pe.index(b'\0', p)].decode('latin1')
        return None

    out = {}
    for line in open(os.path.join(RE, 'coff', mod + '.tsv')):
        _, right, _, name = line.rstrip('\n').split('\t', 3)
        if name.startswith('$SG') or name.startswith('??_C@'):
            s = cstr(int(right, 16))
            if s is not None:
                out[name.replace('$SG', '_SG')] = s  # Ghidra spells $SGnnn as _SGnnn
    return out


# Parameter type codes seen at NewParameter(...)+0x3c (decoded from usage; see FINDINGS).
TYPES = {}

new_method = re.compile(r'(\w+) = DE_CPCNodeClass::NewMethod\s*\(\s*([\w:]+)\s*,\s*([\w&]+)\s*,\s*(\w+)\s*\)')
new_param = re.compile(r'(\w+) = DE_CPCNodeMethod::NewParameter\s*\(\s*(\w+)\s*,\s*([\w&]+)\s*\)')
set_type = re.compile(r'\*\(undefined4 \*\)\((\w+) \+ 0x3c\) = (\w+)\s*$')

rows = []
for mod in MODULES:
    path = os.path.join(RE, 'dump', mod, 'all.c')
    if not os.path.exists(path):
        continue
    strings = load_strings(mod)
    text = open(path, encoding='latin1').read()
    # Join wrapped calls onto one line.
    text = re.sub(r'\n\s+', ' ', text)
    method_var = {}
    cur = None
    for stmt in text.split(';'):
        m = new_method.search(stmt)
        if m:
            var, node, name, caller = m.groups()
            cls = re.sub(r'::m_(\w+)ClassNode', r'', node).split('::')[0]
            cls = node.split('::')[0] if '::' in node else node
            cur = {'module': mod, 'class': cls, 'name': strings.get(name.lstrip('&'), name), 'caller': caller,
                   'params': []}
            rows.append(cur)
            method_var[var] = cur
            continue
        m = new_param.search(stmt)
        if m and m.group(2) in method_var:
            pvar, mvar, pname = m.groups()
            method_var[mvar]['params'].append([strings.get(pname.lstrip('&'), pname), None])
            method_var['__last_param'] = pvar
            continue
        m = set_type.search(stmt)
        if m and cur and cur['params'] and m.group(1) == method_var.get('__last_param'):
            cur['params'][-1][1] = m.group(2)

with open(os.path.join(RE, 'native-api.tsv'), 'w') as out:
    for r in rows:
        params = ', '.join(f'{t}:{n}' for n, t in r['params'])
        clean = lambda x: str(x).replace('\n', '\\n').replace('\t', '\\t')
        out.write(f"{r['module']}\t{r['class']}\t{clean(r['name'])}\t{r['caller']}\t{clean(params)}\n")
print(len(rows), 'registered intrinsics')
