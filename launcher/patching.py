"""Apply the bundled, hash-pinned English 2.090 -> 2.123 cumulative patch.

Only the installer's private staging tree may be passed to apply_patch().
No Windows updater, network service or intermediate game version is used.
"""
from __future__ import annotations

from contextlib import contextmanager
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import zipfile

PATCH_NAME = "english-2090-2123.obgpatch"


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def matches(data: bytes, item: dict, prefix: str = "") -> bool:
    return len(data) == item[prefix + "size"] and hashlib.sha256(data).hexdigest() == item[prefix + "sha256"]


@contextmanager
def bundled_patch(resources: Path):
    """Validate the complete archive before accepting any disc files or deltas."""
    expected = json.loads((resources / "cumulative-patch.json").read_text())
    path = resources / "patches" / PATCH_NAME
    if not path.is_file() or path.stat().st_size != expected["size"]:
        raise ValueError("The bundled game update is missing or incomplete. Reinstall the app.")
    payload = path.read_bytes()
    if not matches(payload, expected):
        raise ValueError("The bundled game update failed its integrity check. Reinstall the app.")
    target = json.loads((resources / "game-files.json").read_text())
    with zipfile.ZipFile(io.BytesIO(payload)) as archive:
        if archive.getinfo("manifest.json").file_size > 512 * 1024:
            raise ValueError("The bundled game update manifest is too large.")
        manifest = json.loads(archive.read("manifest.json"))
        if (manifest.get("format") != 1 or manifest.get("source_version") != "2.090"
                or manifest.get("target_version") != target["version"]):
            raise ValueError("The bundled game update has an unsupported version.")
        items = manifest["files"]
        targets = {item["path"]: item for item in target["files"]}
        if not targets or len(items) != len(targets) or {item["path"] for item in items} != set(targets):
            raise ValueError("The bundled game update has an incomplete file list.")
        members = {"manifest.json"}
        for item in items:
            name = item["path"]
            parts = PurePosixPath(name)
            if parts.is_absolute() or ".." in parts.parts or "\\" in name or parts.parts[0] not in ("Data", "Engine"):
                raise ValueError("The bundled game update contains an invalid path.")
            if any(item[key] != targets[name][key] for key in ("size", "sha256")):
                raise ValueError(f"The bundled game update has the wrong target: {name}")
            if "delta" in item:
                member = item["delta"]
                if member in members or not member.startswith("deltas/"):
                    raise ValueError("The bundled game update contains duplicate or invalid deltas.")
                members.add(member)
                # A binary delta for these files must fit comfortably below a
                # full uncompressed replacement, with space for small headers.
                size = archive.getinfo(member).file_size
                if size != item["delta_size"] or size > item["size"] + 1024 * 1024:
                    raise ValueError(f"The bundled game update has an invalid delta size: {name}")
                delta = archive.read(member)
                if not matches(delta, item, "delta_"):
                    raise ValueError(f"The bundled game update has a corrupt delta: {name}")
                if (len(delta) < 32 or delta[:8] != b"BSDIFF40" or delta[31] & 0x80
                        or int.from_bytes(delta[24:32], "little") != item["size"]):
                    raise ValueError(f"The bundled game update has an invalid delta header: {name}")
            elif item["source_size"] != item["size"] or item["source_sha256"] != item["sha256"]:
                raise ValueError(f"The bundled game update is missing a delta: {name}")
        if len(archive.namelist()) != len(members) or set(archive.namelist()) != members:
            raise ValueError("The bundled game update contains unexpected archive members.")
        yield archive, items


def validate_bundle(resources: Path) -> None:
    with bundled_patch(resources):
        pass


def apply_patch(root: Path, resources: Path, progress=lambda _done, _total: None) -> None:
    import bsdiff4

    with bundled_patch(resources) as (archive, items):
        # Preflight every input before changing any file. Mixing disc editions
        # or patch levels must never produce a partially accepted installation.
        for item in items:
            path = root / item["path"]
            if (not path.resolve().is_relative_to(root.resolve()) or path.is_symlink()
                    or not path.is_file() or path.stat().st_size != item["source_size"]
                    or digest(path) != item["source_sha256"]):
                raise ValueError(f"Unsupported disc file: {item['path']}. Use the original English 2.090 CD.")
        for index, item in enumerate(items, 1):
            if "delta" in item:
                path = root / item["path"]
                result = bsdiff4.patch(path.read_bytes(), archive.read(item["delta"]))
                if not matches(result, item):
                    raise ValueError(f"The bundled game update could not reconstruct: {item['path']}")
                temporary = path.with_name(path.name + ".obg-new")
                try:
                    temporary.write_bytes(result)
                    temporary.replace(path)
                finally:
                    temporary.unlink(missing_ok=True)
            progress(index, len(items))


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Check a bundled cumulative patch and its target manifest")
    parser.add_argument("resources", type=Path)
    validate_bundle(parser.parse_args().resources)
    print("Bundled cumulative patch verified")
