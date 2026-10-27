"""Compose full factory paths from factory-paths.tsv and match them to class-census.txt."""
import re
import sys

pathadd, parent, module = {}, {}, {}
for line in open('factory-paths.tsv'):
    mod, cls, fn, val = line.rstrip('\n').split('\t')
    module.setdefault(cls, mod)
    if fn == 'PathAdd':
        pathadd[cls] = val
    elif fn == 'ObjParent':
        # g_FooFactory / __imp_g_FooFactory / DLL::g_FooFactory -> DE_CFooFactory
        m = re.search(r'g_(\w+Factory)', val)
        parent[cls] = 'DE_C' + m.group(1) if m else None


def full(cls, depth=0):
    if cls is None or cls == 'DE_CObjectFactory' or depth > 20:
        return ''
    head = full(parent.get(cls), depth + 1)
    leaf = pathadd.get(cls, '?')
    return f'{head}\\{leaf}' if head else leaf


paths = {}
for cls in pathadd:
    paths.setdefault(full(cls), []).append(cls)

census = [l.split(None, 1) for l in open('class-census.txt')]
hit = miss = 0
with open('class-map.tsv', 'w') as out:
    for n, path in census:
        path = path.strip()
        classes = paths.get(path)
        out.write(f'{n}\t{path}\t{",".join(c[:-7] for c in classes) if classes else "?"}\t'
                  f'{module.get(classes[0], "?") if classes else "?"}\n')
        if classes:
            hit += int(n)
        else:
            miss += int(n)
print(f'objects with a known class: {hit}, unknown: {miss}')
