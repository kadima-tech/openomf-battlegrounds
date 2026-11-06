#!/usr/bin/env python3
"""Sets two machines' OPENBG_NET_TRACE files side by side: each fighter's place on the client
against the server's at the same moment (interpolated), and their health. The moment is the wall
clock (both processes on one machine), or with --scene each one's scene time.

    compare.py [--scene] host.trace client.trace
"""
import bisect
import math
import sys


def load(path, scene):
    rows = []
    with open(path) as f:
        for line in f:
            parts = line.split(" | ")
            head = parts[0].split()
            if len(head) < 3:
                continue
            frame, ms, status = int(head[0]), int(head[1]), int(head[2])
            if not scene and len(head) > 3:
                ms = int(head[3])
            fighters = {}
            for p in parts[1:]:
                v = p.split()
                if len(v) < 7:
                    continue
                fighters[int(v[0])] = (v[1], float(v[2]), float(v[3]), float(v[4]), float(v[5]), v[6], int(v[7]) if len(v) > 7 else 0)
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
    return fa[0], pos, fa[4] if t < 0.5 else fb[4], fa[6] + (fb[6] - fa[6]) * t


def pct(v, f):
    return v[int(f * (len(v) - 1))] if v else 0.0


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    scene = "--scene" in sys.argv
    if len(args) != 2:
        print(__doc__)
        return 2
    host, client = load(args[0], scene), load(args[1], scene)
    times = [r[0] for r in host]
    errors, health, since, clocks = {}, {}, {}, {}
    spawning = ("Entry()", "FighterJustCreated()", "AirFallFromSpawn()")
    for ms, frame, status, fighters in client:
        if status != 1000:
            since.clear()
            continue
        for slot in list(since):
            if slot not in fighters:
                del since[slot]
        for slot, f in fighters.items():
            h = at(host, times, ms, slot)
            # Measured from 2 s after both copies are out of their spawn (each round's).
            if not h or h[0] != f[0] or f[5] in spawning:
                since.pop(slot, None)
                continue
            since.setdefault(slot, ms)
            if ms - since[slot] < 2000:
                continue
            e = math.dist(h[1], f[1:4])
            errors.setdefault(slot, []).append(e)
            clocks.setdefault(slot, []).append(f[6] - h[3])
            if h[2] >= 0 and f[4] >= 0:
                health.setdefault(slot, []).append(abs(h[2] - f[4]))
    ok = True
    for slot in sorted(errors):
        v = sorted(errors[slot])
        hv = sorted(health.get(slot, []))
        name = client[-1][3].get(slot, ("?",))[0]
        mean = sum(v) / len(v)
        print(f"slot {slot} {name}: position error (client copy vs server) mean {mean:.2f} median {pct(v, .5):.2f} "
              f"p95 {pct(v, .95):.2f} max {v[-1]:.2f} over {len(v)} frames; health difference p95 {pct(hv, .95):.3f} max {hv[-1] if hv else 0:.3f}; "
              f"its clock on the client {pct(sorted(clocks[slot]), .5):+.0f} ms against the server's (median)")
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
