#!/usr/bin/env python3
"""Fingerprint app inputs, including additions/deletions, without build outputs."""
import hashlib
import os
from pathlib import Path
import sys


def fingerprint(repo: Path) -> str:
    inputs = [repo / "launch.command", repo / "re/scripts/rtpatch_emu.py",
              repo / "re/scripts/directx_tables.py"]
    for directory in ("engine", "launcher"):
        base = repo / directory
        for current, directories, filenames in os.walk(base):
            # Prune generated trees before descending into fetched dependencies.
            directories[:] = [name for name in directories if name != "__pycache__" and not (
                Path(current) == base and (name == "build" or (directory == "engine" and name.startswith("build"))))]
            inputs.extend(Path(current) / name for name in filenames if name != ".DS_Store")
    digest = hashlib.sha256()
    for path in sorted(inputs):
        digest.update(path.relative_to(repo).as_posix().encode() + b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


if __name__ == "__main__":
    current = fingerprint(Path(__file__).resolve().parent.parent)
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        state = Path(sys.argv[2])
        sys.exit(0 if state.is_file() and state.read_text().strip() == current else 1)
    print(current)
