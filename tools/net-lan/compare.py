#!/usr/bin/env python3
"""Sets two machines' OPENBG_NET_TRACE files side by side: each fighter's place on the client
against the server's at the same scene time (interpolated), and their health.

    compare.py host.trace client.trace
"""
import bisect
import math
import sys


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            parts = line.split(" | ")
            head = parts[0].split()
            if len(head) < 3:
                continue
            frame, ms, status = int(head[0]), int(head[1]), int(head[2])
            fighters = {}
            for p in parts[1:]:
                v = p.split()
                if len(v) < 7:
                    continue
                fighters[int(v[0])] = (v[1], float(v[2]), float(v[3]), float(v[4]), float(v[5]), v[6])
            rows.append((ms, frame, status, fighters))
    rows.sort(key=lambda r: r[0])
    return rows


def at(rows, times, ms, slot):
    i = bisect.bisect_left(times, ms)
    if i == 0 or i >= len(rows):
        return None
    a, b = rows[i - 1], rows[i]
    if slot not in a[3] or slot not in b[3] or b[0] == a[0]:
        return None
    t = (ms - a[0]) / (b[0] - a[0])
    fa, fb = a[3][slot], b[3][slot]
    pos = [fa[k] + (fb[k] - fa[k]) * t for k in (1, 2, 3)]
    return fa[0], pos, fa[4] if t < 0.5 else fb[4]


def pct(v, f):
    return v[int(f * (len(v) - 1))] if v else 0.0


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    host, client = load(sys.argv[1]), load(sys.argv[2])
    times = [r[0] for r in host]
    errors, health, first = {}, {}, {}
    for ms, frame, status, fighters in client:
        if status != 1000:
            continue
        for slot, f in fighters.items():
            h = at(host, times, ms, slot)
            if not h or h[0] != f[0]:
                continue
            first.setdefault(slot, ms)
            if ms - first[slot] < 2000:
                continue
            e = math.dist(h[1], f[1:4])
            errors.setdefault(slot, []).append(e)
            if h[2] >= 0 and f[4] >= 0:
                health.setdefault(slot, []).append(abs(h[2] - f[4]))
    ok = True
    for slot in sorted(errors):
        v = sorted(errors[slot])
        hv = sorted(health.get(slot, []))
        name = client[-1][3].get(slot, ("?",))[0]
        mean = sum(v) / len(v)
        print(f"slot {slot} {name}: position error (client copy vs server) mean {mean:.2f} median {pct(v, .5):.2f} "
              f"p95 {pct(v, .95):.2f} max {v[-1]:.2f} over {len(v)} frames; health difference p95 {pct(hv, .95):.3f} max {hv[-1] if hv else 0:.3f}")
    if not errors:
        print("no frames where both machines had the fighters")
        ok = False
    last_host = host[-1][3] if host else {}
    last_client = client[-1][3] if client else {}
    for slot in sorted(set(last_host) | set(last_client)):
        lh, lc = last_host.get(slot), last_client.get(slot)
        print(f"slot {slot} at the end: server {lh[0] + ' %.0f%% ' % (lh[4] * 100) + lh[5] if lh else '-'}; client {lc[0] + ' %.0f%% ' % (lc[4] * 100) + lc[5] if lc else '-'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
