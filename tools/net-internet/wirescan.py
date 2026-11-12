#!/usr/bin/env python3
"""Reads OPENBG_NET_WIRE files (every datagram a game socket received: a 4-byte little-endian
length, then the bytes) and checks that no game datagram holds any of the given plaintext markers.
Lobby datagrams ("OBGL") and LAN queries ("OBGQ") are public and only counted.

    wirescan.py <wire file>... <marker>...
"""
import os
import sys


def datagrams(path):
    with open(path, "rb") as f:
        d = f.read()
    i = 0
    while i + 4 <= len(d):
        n = int.from_bytes(d[i:i + 4], "little")
        yield d[i + 4:i + 4 + n]
        i += 4 + n


def main():
    files = [a for a in sys.argv[1:] if os.path.exists(a)]
    markers = [a.encode() for a in sys.argv[1:] if a not in files]
    bad = 0
    for path in files:
        game = lobby = size = 0
        hits = {}
        for g in datagrams(path):
            if g[:4] in (b"OBGL", b"OBGQ"):
                lobby += 1
                continue
            game += 1
            size += len(g)
            for m in markers:
                if m in g:
                    hits[m] = hits.get(m, 0) + 1
        bad += sum(hits.values())
        found = ", ".join(f"{m.decode()} x{n}" for m, n in hits.items()) or "none"
        print(f"{os.path.basename(path)}: {game} game datagrams ({size} bytes), {lobby} lobby; plaintext markers found: {found}")
    if not files:
        print("wirescan: no wire files")
        return 1
    print("wirescan: " + ("PLAINTEXT ON THE WIRE" if bad else "no plaintext game data on the wire"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
