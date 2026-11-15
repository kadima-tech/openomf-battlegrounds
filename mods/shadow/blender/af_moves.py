"""Print a One Must Fall 2097 FIGHTR*.AF file's header and move table (layout and categories as OpenOMF's
src/formats/af.c, move.c, animation.c and game/objects/har.h): per move its input string, animation string,
category, damage, stun, successor and sprite frames.

python3 af_moves.py FIGHTR1.AF [--json out.json]
"""
import json
import struct
import sys

CATEGORIES = {0: "misc", 2: "close", 4: "low", 5: "medium", 6: "high", 7: "jumping", 8: "projectile", 9: "basic",
              10: "hazard", 11: "victory", 12: "scrap", 13: "destruction"}


class R:
    def __init__(s, b):
        s.b, s.p = b, 0

    def take(s, fmt):
        v = struct.unpack_from("<" + fmt, s.b, s.p)
        s.p += struct.calcsize("<" + fmt)
        return v if len(v) > 1 else v[0]

    def buf(s, n):
        v = s.b[s.p:s.p + n]
        s.p += n
        return v

    def text(s):
        n = s.take("H")
        v = s.buf(n).decode("latin1")
        s.take("B")
        return v


def animation(r):
    sx, sy, _, cc, sc = r.take("hhIHB")
    coords = []
    for _ in range(cc):
        t = r.take("I")
        a, b = t & 0xFFFF, t >> 16
        coords.append(((((a & 0x3FF) ^ 0x200) - 0x200), (((b & 0x3FF) ^ 0x200) - 0x200), b >> 10))
    anim = r.text()
    extras = [r.text() for _ in range(r.take("B"))]
    sprites = []
    for _ in range(sc):
        ln, x, y, w, h, idx, missing = r.take("HhhHHBB")
        if not missing:
            r.buf(ln)
        sprites.append(dict(x=x, y=y, w=w, h=h, index=idx, missing=missing))
    return dict(start=(sx, sy), anim=anim, extras=extras, sprites=sprites, coords=coords)


def main():
    path = sys.argv[1]
    b = open(path, "rb").read()
    r = R(b)
    fid, window, endurance, jump_limit, health, fwd, rev, jump, fall, ver, thr = r.take("HHIBHiiiiBB")
    head = dict(fighter_id=fid, endurance=endurance / 256.0, health=health, forward=fwd / 256.0, reverse=rev / 256.0,
                jump=jump / 256.0, fall=fall / 256.0)
    moves = []
    while r.p < len(b):
        mid = r.take("B")
        if mid >= 70:
            break
        a = animation(r)
        f = r.take("HH8B9B")
        ms = r.buf(21).split(b"\0")[0].decode("latin1")
        n = r.take("H")
        foot = r.buf(n).decode("latin1") if n else ""
        moves.append(dict(id=mid, ai_flags=f[0], pos=f[1], play_if_hit=f[10], category=f[11], block_damage=f[12],
                          block_stun=f[13], successor=f[14], damage=f[15], throw_duration=f[16], ess=f[17], points=f[18],
                          input=ms, footer=foot, **a))
    print("header", head)
    for m in moves:
        print("move %2d %-11s in=%-8r dmg=%-3d blk=%d stun=%d next=%d hit->%d pts=%d ess=%d thr=%d ai=%04x pos=%04x sprites=%d" % (
            m["id"], CATEGORIES.get(m["category"], m["category"]), m["input"], m["damage"], m["block_damage"], m["block_stun"],
            m["successor"], m["play_if_hit"], m["points"], m["ess"], m["throw_duration"], m["ai_flags"], m["pos"], len(m["sprites"])))
        print("        anim  ", m["anim"])
        for k, e in enumerate(m["extras"]):
            print("        extra%d" % k, e)
        if m["footer"]:
            print("        footer", m["footer"])
    if "--json" in sys.argv:
        with open(sys.argv[sys.argv.index("--json") + 1], "w") as fp:
            json.dump(dict(header=head, moves=moves), fp, indent=1)


main()
