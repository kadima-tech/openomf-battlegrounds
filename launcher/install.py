#!/usr/bin/env python3
"""Private first-run installer. stdout is newline-delimited JSON for the native UI.

Only this file's staging directory is patched. The CD/ISO and existing installs
are never modified. A complete, checksum-verified tree is published atomically.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import urllib.request

PRODUCT_GUID = "b8ef8b5a-6d15-436c-ac07-5f2bd5e1aa1b"


def emit(stage: str, status: str, detail: str) -> None:
    print(json.dumps(dict(stage=stage, status=status, detail=detail)), flush=True)


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def verify(root: Path, manifest: dict) -> None:
    if not manifest["files"]:
        raise ValueError("The game verification manifest is empty.")
    for item in manifest["files"]:
        path = root / item["path"]
        if not path.is_file() or path.stat().st_size != item["size"]:
            raise ValueError(f"Game file missing or incomplete: {item['path']}")
        if digest(path) != item["sha256"]:
            raise ValueError(f"Game file needs repair: {item['path']}")


def record_verified(root: Path, manifest: dict, destination: Path) -> None:
    """Seed the native readiness cache only after full checksum verification.

    macOS ctime/mtime nanoseconds and inode detect replacements and ordinary
    edits without rereading 730 MB on every start. Expected digests invalidate
    the cache if a future app targets a different official data version.
    """
    files = []
    for item in manifest["files"]:
        info = (root / item["path"]).stat()
        files.append(dict(item, modified=f"{info.st_mtime_ns // 10**9}:{info.st_mtime_ns % 10**9}",
                          changed=f"{info.st_ctime_ns // 10**9}:{info.st_ctime_ns % 10**9}", inode=info.st_ino))
    temporary = destination.with_suffix(".tmp")
    temporary.write_text(json.dumps(dict(root=str(root.resolve()), files=files)))
    temporary.replace(destination)


def child_named(root: Path, name: str) -> Path:
    return next((p for p in root.iterdir() if p.name.casefold() == name.casefold()), root / name)


def disc_setup(root: Path) -> Path:
    setup = child_named(root, "Setup")
    ini = child_named(setup, "setup.ini")
    if PRODUCT_GUID not in ini.read_text(errors="replace").lower():
        raise ValueError("This is not a One Must Fall: Battlegrounds game disc.")
    for name in ("data1.cab", "data1.hdr", "data2.cab"):
        if not child_named(setup, name).is_file():
            raise ValueError(f"The game disc is incomplete ({name} is missing).")
    return setup


def download(url: str, destination: Path, expected: dict) -> None:
    """Never accept an HTML error page, truncated download or changed patch."""
    temporary = destination.with_suffix(destination.suffix + ".part")
    request = urllib.request.Request(url, headers={"User-Agent": "OpenBG-Launcher/1.0"})
    try:
        with urllib.request.urlopen(request, timeout=45) as response, temporary.open("wb") as out:
            total = 0
            while block := response.read(256 * 1024):
                total += len(block)
                if total > expected["size"]:
                    raise ValueError("The patch download has an unexpected size.")
                out.write(block)
        if total != expected["size"] or digest(temporary) != expected["sha256"]:
            raise ValueError("The patch download failed its integrity check. Please retry.")
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def run(command: list[str | Path], log, **kwargs) -> None:
    log.write("\n> " + " ".join(map(str, command)) + "\n")
    log.flush()
    subprocess.run(list(map(str, command)), stdout=log, stderr=log, check=True, **kwargs)


def publish(staged: Path, destination: Path) -> None:
    backup = destination.with_name("game.previous")
    # A previous interrupted publish is recoverable; never delete the only copy.
    if backup.exists() and not destination.exists():
        backup.replace(destination)
    if backup.exists():
        shutil.rmtree(backup)
    if destination.exists():
        destination.replace(backup)
    try:
        staged.replace(destination)
    except OSError:
        if backup.exists():
            backup.replace(destination)
        raise
    if backup.exists():
        shutil.rmtree(backup)


def install(source: Path, home: Path, resources: Path, project: Path | None = None) -> None:
    home.mkdir(parents=True, exist_ok=True)
    with (home / "install.lock").open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError("Setup is already running in another window.") from None
        if shutil.disk_usage(home).free < 3 * 1024**3:
            raise ValueError("Setup needs at least 3 GB of free space. Free some space and retry.")
        with (home / "install.log").open("w", buffering=1) as log:
            # Remove only abandoned private staging directories, while holding the lock.
            for stale in home.glob(".install-*"):
                if stale.is_dir() and not stale.is_symlink():
                    shutil.rmtree(stale)
            with tempfile.TemporaryDirectory(prefix=".install-", dir=home) as tmp:
                work = Path(tmp)
                emit("files", "active", "Reading your game disc…")
                if source.is_dir():
                    setup = disc_setup(source)
                else:
                    if not source.is_file() or source.suffix.lower() != ".iso":
                        raise ValueError("Choose a Battlegrounds ISO image or insert the original game CD.")
                    listing = subprocess.check_output(["/usr/bin/bsdtar", "-tf", str(source)], text=True)
                    entry = next((x for x in listing.splitlines() if x.strip("./").lower() == "setup"), None)
                    if not entry:
                        raise ValueError("This image does not contain the Battlegrounds installer.")
                    disc = work / "disc"
                    disc.mkdir()
                    run(["/usr/bin/bsdtar", "-xf", source, "-C", disc, entry], log)
                    setup = disc_setup(disc)
                emit("files", "active", "Unpacking the original game files…")
                unpacked = work / "unpacked"
                run([resources / "bin/unshield", "-d", unpacked, "x", child_named(setup, "data1.cab")], log)
                game = unpacked / "App_Executables"
                if not (game / "AutoPatch/patchw32.dll").is_file() or not (game / "Data/OMFBGGUI.DEObjects").is_file():
                    raise ValueError("This disc edition is not supported. Use the original English 2.090 CD.")
                emit("files", "done", "Original game files extracted")
                patches = json.loads((resources / "patches.json").read_text())
                cache = home / "patches"
                cache.mkdir(exist_ok=True)
                for index, patch in enumerate(patches, 1):
                    name = patch["name"]
                    target = cache / name
                    if not (target.is_file() and target.stat().st_size == patch["size"] and digest(target) == patch["sha256"]):
                        local = project / "source/patches" / name if project else None
                        if local and local.is_file() and digest(local) == patch["sha256"]:
                            shutil.copyfile(local, target)
                        else:
                            emit("patches", "active", f"Downloading official patch {index} of {len(patches)}…")
                            download(patch["url"], target, patch)
                    emit("patches", "active", f"Applying official patch {index} of {len(patches)}…")
                    patch_log = work / "patch.log"
                    with patch_log.open("w") as output:
                        command = [sys.executable, "-I", str(resources / "rtpatch_emu.py"),
                                   str(game / "AutoPatch/patchw32.dll"), str(target), str(game),
                                   "-NOBACKUP", "-NOUNDO", "-NOCONFIRM"]
                        result = subprocess.run(command, stdout=output, stderr=output)
                    report = patch_log.read_text(errors="replace")
                    log.write(report)
                    if result.returncode or re.search(r"Old Files Invalid\.+ \(\s*[1-9]|error ept", report, re.I):
                        raise ValueError(f"Official patch {index} could not be applied. Your disc may be a different edition.")
                emit("patches", "done", "All official patches applied · 2.123")
                emit("runtime", "active", "Verifying every game file…")
                manifest = json.loads((resources / "game-files.json").read_text())
                verify(game, manifest)
                publish(game, home / "game")
                try:
                    record_verified(home / "game", manifest, home / "verified-files.json")
                except OSError as error:
                    log.write(f"Readiness cache could not be saved: {error}\n")
                emit("runtime", "done", "Verified and ready to play")
                emit("complete", "done", str(home / "game/Data"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--home", type=Path, required=True)
    parser.add_argument("--resources", type=Path, default=Path(__file__).parent)
    parser.add_argument("--project", type=Path)
    parser.add_argument("--verify", type=Path, help="Verify an existing install instead of installing")
    args = parser.parse_args()
    # The native app starts a private process group. Cancellation stops children,
    # raises through TemporaryDirectory and releases the install lock.
    def cancelled(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, cancelled)
    try:
        if args.verify:
            verify(args.verify, json.loads((args.resources / "game-files.json").read_text()))
        elif args.source:
            install(args.source.resolve(), args.home.resolve(), args.resources.resolve(), args.project)
        else:
            parser.error("--source or --verify is required")
        return 0
    except KeyboardInterrupt:
        emit("error", "error", "Setup cancelled. You can safely try again.")
    except Exception as error:
        emit("error", "error", str(error))
    return 1


if __name__ == "__main__":
    sys.exit(main())
