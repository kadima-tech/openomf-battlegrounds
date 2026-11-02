"""Startup readiness check for the Linux launcher; a port of Readiness.swift.

Shares verified-files.json with install.record_verified() and the macOS app.
No remembered "first run" flag: the actual files and their expected digests
decide readiness. Unchanged files (same inode, size, mtime and ctime) recorded
after a full checksum pass are not reread; anything else is hashed again.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import stat
import sys

# The launcher runs this file with an isolated (-I) interpreter, which leaves
# the script's own folder off sys.path.
if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from patching import digest  # noqa: E402

STAMP_KEYS = ("path", "sha256", "size", "modified", "changed", "inode")


def stamp(item: dict, info: os.stat_result) -> dict:
    return dict(path=item["path"], sha256=item["sha256"], size=item["size"],
                modified=f"{info.st_mtime_ns // 10**9}:{info.st_mtime_ns % 10**9}",
                changed=f"{info.st_ctime_ns // 10**9}:{info.st_ctime_ns % 10**9}", inode=info.st_ino)


def load_cache(path: Path, root: str) -> dict:
    try:
        cache = json.loads(path.read_text())
        if cache.get("root") != root:
            return {}
        return {entry["path"]: {key: entry[key] for key in STAMP_KEYS} for entry in cache["files"]}
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return {}


def verify(root: Path, manifest: dict, cache_path: Path) -> bool:
    try:
        canonical = os.path.realpath(root, strict=True)
    except OSError:
        return False
    files = manifest.get("files") or []
    if not files:
        return False
    previous = load_cache(cache_path, canonical)
    stamps = []
    for item in files:
        path = Path(canonical) / item["path"]
        try:
            info = path.stat()
        except OSError:
            return False
        if not stat.S_ISREG(info.st_mode) or info.st_size != item["size"]:
            return False
        current = stamp(item, info)
        stamps.append(current)
        if previous.get(item["path"]) == current:
            continue
        try:
            if digest(path) != item["sha256"]:
                return False
        except OSError:
            return False
    # An optimization for ordinary local changes, not an authenticity boundary.
    # Never cache a failed verification.
    try:
        cache_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = cache_path.with_name(cache_path.name + ".tmp")
        temporary.write_text(json.dumps(dict(root=canonical, files=stamps)))
        temporary.replace(cache_path)
    except OSError:
        pass
    return True


def ready_game(resources: Path, home: Path, project: Path | None) -> Path | None:
    """The first verified installation: the private one, then a checkout's legacy tree."""
    try:
        manifest = json.loads((resources / "game-files.json").read_text())
    except (OSError, ValueError):
        return None
    candidates = [home / "game"] + ([project / "source/patched/v2123"] if project else [])
    return next((c for c in candidates if verify(c, manifest, home / "verified-files.json")), None)


if __name__ == "__main__":
    # The Linux launcher asks at startup: prints the ready game folder, or exits 1.
    if len(sys.argv) not in (3, 4):
        sys.exit("usage: readiness.py RESOURCES HOME [PROJECT]")
    found = ready_game(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]) if len(sys.argv) == 4 else None)
    if found is None:
        sys.exit(1)
    print(found)
