#!/usr/bin/env python3
"""Rebuild the cumulative delta archive from local English disc/final trees.

Needs bsdiff4 from launcher/requirements.txt. No full game files are bundled.
Every final file and every delta round trip is checked before publication.
"""
from concurrent.futures import ProcessPoolExecutor
import argparse
import hashlib
import json
from pathlib import Path
import sys
import zipfile

LAUNCHER = Path(__file__).resolve().parents[2] / "launcher"
sys.path.insert(0, str(LAUNCHER))
from patching import PATCH_NAME, matches, validate_bundle


def build_entry(args):
    import bsdiff4
    index, item, disc, patched = args
    original = (disc / item["path"]).read_bytes()
    final = (patched / item["path"]).read_bytes()
    if not matches(final, item):
        raise ValueError(f"Final tree does not match the official manifest: {item['path']}")
    entry = dict(item, source_size=len(original), source_sha256=hashlib.sha256(original).hexdigest())
    delta = None
    if original != final:
        delta = bsdiff4.diff(original, final)
        if bsdiff4.patch(original, delta) != final:
            raise ValueError(f"Delta round trip failed: {item['path']}")
        entry.update(delta=f"deltas/{index:03d}.bsdiff", delta_size=len(delta),
                     delta_sha256=hashlib.sha256(delta).hexdigest())
    return entry, delta


def write_member(archive, name, data):
    # Fix timestamps, permissions, order and JSON formatting for reproducibility.
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.create_system = 3
    info.external_attr = 0o100644 << 16
    archive.writestr(info, data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disc", type=Path, help="Unmodified English 2.090 App_Executables tree")
    parser.add_argument("patched", type=Path, help="Verified 2.123 tree from the official patch chain")
    parser.add_argument("--resources", type=Path, default=LAUNCHER)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    target = json.loads((args.resources / "game-files.json").read_text())
    if not target["files"] or target["version"] != "2.123":
        parser.error("A complete English 2.123 target manifest is required")
    output = args.resources / "patches" / PATCH_NAME
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(".part")
    try:
        entries = []
        work = [(index, item, args.disc, args.patched) for index, item in enumerate(target["files"])]
        with ProcessPoolExecutor(max_workers=args.jobs) as pool, zipfile.ZipFile(temporary, "w") as archive:
            for entry, delta in pool.map(build_entry, work):
                if delta is not None:
                    write_member(archive, entry["delta"], delta)
                entries.append(entry)
                print(entry["path"], flush=True)
            manifest = dict(format=1, source_version="2.090", target_version="2.123", files=entries)
            write_member(archive, "manifest.json", json.dumps(manifest, sort_keys=True, indent=2).encode() + b"\n")
        temporary.replace(output)
        descriptor = dict(size=output.stat().st_size, sha256=hashlib.sha256(output.read_bytes()).hexdigest())
        (args.resources / "cumulative-patch.json").write_text(json.dumps(descriptor, indent=2) + "\n")
        validate_bundle(args.resources)
        print(f"Verified {output}: {descriptor}")
    finally:
        temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
