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
            line, _, objs = line.rstrip("\n").partition(" #")
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
            objects = []
            for o in objs.split():
                v = o.rsplit(":", 2)
                if len(v) == 3:
                    objects.append((v[0], tuple(float(x) for x in v[1].split(",")), v[2]))
            rows.append((ms, frame, status, fighters, objects))
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


def objects(host, times, client):
    """The other replicated creatures (projectiles, pickups, hazards): per name, the moments each
    machine had one, how many it had on the client against the server at the same moment, and how
    far the nearest copy of the same name was."""
    seen = {}
    for ms, frame, status, fighters, objs in client:
        if status != 1000:
            continue
        i = bisect.bisect_left(times, ms)
        if i == 0 or i >= len(host):
            continue
        hobjs = host[i][4]
        names = {o[0] for o in objs} | {o[0] for o in hobjs}
        for n in names:
            c = [o for o in objs if o[0] == n]
            h = [o for o in hobjs if o[0] == n]
            e = seen.setdefault(n, {"server": 0, "client": 0, "both": 0, "unlinked": 0, "err": []})
            e["server"] += bool(h)
            e["client"] += bool(c)
            e["both"] += bool(h) and bool(c)
            e["unlinked"] += sum(1 for o in c if o[2] == "0")
            e["local"] = any(o[2] == "-" for o in c + h)
            for o in c:
                if h:
                    e["err"].append(min(math.dist(o[1], x[1]) for x in h))
    if seen:
        print("objects (frames present on the server / client / both, client copies unlinked, distance to the nearest server copy median / p95):")
    for n in sorted(seen):
        e = seen[n]
        v = sorted(e["err"])
        kind = " (not replicated)" if e.get("local") else ""
        print(f"  {n}{kind}: {e['server']} / {e['client']} / {e['both']}, unlinked {e['unlinked']}, distance {pct(v, .5):.1f} / {pct(v, .95):.1f}")


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
    for ms, frame, status, fighters, _ in client:
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
    objects(host, times, client)
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
