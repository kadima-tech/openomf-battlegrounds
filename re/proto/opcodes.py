"""Extract opcode -> pc advance from DE_CPCExecuter::ContinueExecutionInternal decompile."""
import re
import sys

import os
src = open(os.path.join(os.path.dirname(__file__), '../dump/Core/functions/7014eaa0.c')).read().split('\n')
# Only the top-level switch: from 'switch(iVar41)' to its label section.
start = next(i for i, l in enumerate(src) if 'switch(iVar41)' in l)
cases = {}
cur = []
body = []
blocks = []
TABLE = {}
for line in src[start + 1:]:
    m = re.match(r'^        case (0x[0-9a-f]+|\d+):', line)
    d = re.match(r'^        default:', line)
    if m or d:
        if cur and body:
            blocks.append((cur, body))
            cur, body = [], []
        if m:
            cur.append(int(m.group(1), 0))
        else:
            cur.append('default')
        continue
    if cur:
        body.append(line)
if cur:
    blocks.append((cur, body))

adv_re = re.compile(r'\*\(\w+ \*\*\)\(this \+ 0x2c\) = (piVar28|puVar18|\w+) \+ (\w+);')
for ops, body in blocks:
    text = '\n'.join(body)
    inline = []
    other = []
    for base, k in adv_re.findall(text):
        if re.match(r'^(0x)?[0-9a-f]+$', k) and base in ('piVar28', 'puVar18'):
            inline.append(int(k, 0) - 1 if base == 'piVar28' else int(k, 0))
        else:
            other.append(f'{base}+{k}')
    SIZES = {'BYTE': 1, 'CHAR': 1, 'WORD': 2, 'SHORT': 2, 'DWORD': 4, 'INT': 4, 'FLOAT': 4}
    kinds = re.findall(r'Extract(BYTE|CHAR|WORD|SHORT|DWORD|INT|FLOAT)\(this\)', text)
    # Operand layout in bytes: inline int reads first (pc = piVar28 + k), then Extract* calls in order.
    layout = ['i32'] * (max(inline) if inline else 0) + [k.lower() for k in kinds]
    adv = sum(4 for _ in range(max(inline) if inline else 0)) + sum(SIZES[k] for k in kinds)
    # Jumps set pc = VMCode base + operand.
    jump = bool(other)
    calls = sorted(set(re.findall(r'([A-Z][A-Za-z_]+::[A-Za-z_]+)\(', text)))[:4]
    if __name__ == '__main__':
        print(','.join(map(str, ops)), '\tbytes=', adv, '\t', ' '.join(layout), '\tjump' if jump else '', '\t', ' '.join(calls))
    for o in ops:
        TABLE[o] = (adv, layout, jump)

# Hand-decoded cases whose pc arithmetic the pattern match can't see (see FINDINGS).
# 'blob' = operand 2 is a byte length of inline data that follows the operands.
TABLE[2] = (8, ['i32', 'i32', 'blob'], False)   # copy inline literal of len bytes to stack[dst]
TABLE[3] = (8, ['i32', 'i32', 'blob'], False)   # stack[dst] = pointer to inline data (strings)
TABLE[20] = (2, ['word'], False)                # allocate N local reference slots
TABLE[21] = (6, ['word', 'i32'], False)         # stack[off] = local reference[idx]
TABLE[22] = (6, ['word', 'i32'], False)         # local reference[idx] = stack[off]
TABLE[310] = (0, [], False)                     # no case label: falls through the switch (no-op)
TABLE[313] = (7, ['word', 'int', 'byte', 'word?10'], False)  # declare variable parameter; class index only when type == 10
