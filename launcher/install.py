#!/usr/bin/env python3
"""Private first-run installer. stdout is newline-delimited JSON for the native UI.

Only this file's staging directory is patched. The CD/ISO and existing installs
are never modified. A complete, checksum-verified tree is published atomically.
Runs on macOS and on Windows 10/11 with the app's private Python.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time

# The app starts its private Python isolated (-I), which leaves the script's own folder off
# sys.path. Find the sibling modules and the bundled decoder package next to this file.
_here = Path(__file__).resolve().parent
for _path in (_here / "python-packages", _here):
    if str(_path) not in sys.path:
        sys.path.insert(0, str(_path))

from iso9660 import extract_directory  # noqa: E402
from patching import apply_patch, digest  # noqa: E402

WINDOWS = os.name == "nt"

PRODUCT_GUID = "b8ef8b5a-6d15-436c-ac07-5f2bd5e1aa1b"


def emit(stage: str, status: str, detail: str) -> None:
    print(json.dumps(dict(stage=stage, status=status, detail=detail)), flush=True)


def verify(root: Path, manifest: dict) -> None:
    if not manifest["files"]:
        raise ValueError("The game verification manifest is empty.")
    for item in manifest["files"]:
        path = root / item["path"]
        if not path.is_file() or path.stat().st_size != item["size"]:
            raise ValueError(f"Game file missing or incomplete: {item['path']}")
        if digest(path) != item["sha256"]:
            raise ValueError(f"Game file needs repair: {item['path']}")


def changed_ns(info: os.stat_result) -> int:
    """The second timestamp the native launchers compare: the inode change time on macOS,
    and on Windows the creation time (a file replaced by another gets a new one)."""
    return getattr(info, "st_birthtime_ns", info.st_ctime_ns) if WINDOWS else info.st_ctime_ns


def record_verified(root: Path, manifest: dict, destination: Path) -> None:
    """Seed the native readiness cache only after full checksum verification.

    ctime (Windows: creation time), mtime nanoseconds and inode (Windows: file ID)
    detect replacements and ordinary edits without rereading 730 MB on every start.
    Expected digests invalidate the cache if a future app targets a different
    official data version.
    """
    files = []
    for item in manifest["files"]:
        info = (root / item["path"]).stat()
        changed = changed_ns(info)
        files.append(dict(item, modified=f"{info.st_mtime_ns // 10**9}:{info.st_mtime_ns % 10**9}",
                          changed=f"{changed // 10**9}:{changed % 10**9}", inode=info.st_ino))
    temporary = destination.with_suffix(".tmp")
    temporary.write_text(json.dumps(dict(root=str(root.resolve()), files=files)), encoding="utf-8")
    temporary.replace(destination)


def child_named(root: Path, name: str) -> Path:
    if not root.is_dir():
        return root / name
    return next((p for p in root.iterdir() if p.name.casefold() == name.casefold()), root / name)


def match_manifest_case(root: Path, manifest: dict) -> None:
    """Rename extracted entries whose names differ from the manifest only by case.

    A no-op on macOS's and Windows' case-insensitive volumes; on Linux it keeps the verified
    tree identical to the recorded file list whatever case the cabinet used.
    """
    for item in manifest["files"]:
        current = root
        for part in PurePosixPath(item["path"]).parts:
            wanted = current / part
            if not wanted.exists() and current.is_dir():
                found = child_named(current, part)
                if found.exists() and not found.is_symlink():
                    found.rename(wanted)
            current = wanted


def disc_setup(root: Path) -> Path:
    setup = child_named(root, "Setup")
    ini = child_named(setup, "setup.ini")
    if PRODUCT_GUID not in ini.read_text(encoding="latin-1").lower():
        raise ValueError("This is not a One Must Fall: Battlegrounds game disc.")
    for name in ("data1.cab", "data1.hdr", "data2.cab"):
        if not child_named(setup, name).is_file():
            raise ValueError(f"The game disc is incomplete ({name} is missing).")
    return setup


def run(command: list[str | Path], log, **kwargs) -> None:
    log.write("\n> " + " ".join(map(str, command)) + "\n")
    log.flush()
    if WINDOWS:
        # Setup runs without a console; keep console tools from opening a window of their own.
        kwargs.setdefault("creationflags", subprocess.CREATE_NO_WINDOW)
    subprocess.run(list(map(str, command)), stdout=log, stderr=log, check=True, **kwargs)


def tool(resources: Path, name: str) -> Path:
    return resources / "bin" / (name + ".exe" if WINDOWS else name)


def unshield_path(path: Path) -> str:
    """A path as unshield 1.5.1 can take it apart. It finds a cabinet's other volumes
    (data1.hdr, data2.cab) by splitting at the last "/", so on Windows a path with
    backslashes sends it looking for the wrong files. Windows accepts forward slashes."""
    return path.as_posix() if WINDOWS else str(path)


def unshield_command(resources: Path, work: Path, unpacked: Path, cabinet: Path) -> tuple[list[str], Path | None]:
    """The extraction command and the folder to run it in.

    unshield also creates output folders one "/"-separated part at a time from the start
    of the path. On Windows the first part of an absolute path is a bare drive ("C:"),
    which the C runtime cannot stat, so every folder failed. There it runs in the work
    folder with a relative output folder instead. The cabinet stays absolute (it may be
    on the CD) with forward slashes."""
    if WINDOWS:
        return ([str(tool(resources, "unshield")), "-d", unpacked.relative_to(work).as_posix(), "x",
                 unshield_path(cabinet)], work)
    return [str(tool(resources, "unshield")), "-d", str(unpacked), "x", str(cabinet)], None


class SetupLock:
    """An exclusive, non-blocking lock that the OS releases if setup is killed."""

    def __init__(self, path: Path):
        # Append mode: never truncate a file another installer has locked (Windows refuses).
        self.stream = path.open("a+")

    def __enter__(self):
        try:
            if WINDOWS:
                import msvcrt
                self.stream.seek(0)
                msvcrt.locking(self.stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self.stream.close()
            raise ValueError("Setup is already running in another window.") from None
        return self

    def __exit__(self, *_):
        self.stream.close()


def retry(action, attempts: int = 1):
    """Windows briefly refuses renames and deletes of folders that an antivirus scanner or the
    search indexer has just opened. Retry those for a few seconds; macOS needs one attempt."""
    for attempt in range(attempts):
        try:
            return action()
        except PermissionError:
            if attempt == attempts - 1:
                raise
            time.sleep(0.5)


ATTEMPTS = 20 if WINDOWS else 1


def remove_tree(path: Path) -> None:
    def writable(function, name, _error):
        os.chmod(name, stat.S_IWRITE)  # Windows will not delete read-only files.
        function(name)
    handler = dict(onexc=writable) if sys.version_info >= (3, 12) else dict(onerror=writable)
    retry(lambda: shutil.rmtree(path, **handler), ATTEMPTS)


def publish(staged: Path, destination: Path) -> None:
    backup = destination.with_name("game.previous")
    # A previous interrupted publish is recoverable; never delete the only copy.
    if backup.exists() and not destination.exists():
        retry(lambda: backup.replace(destination), ATTEMPTS)
    if backup.exists():
        remove_tree(backup)
    if destination.exists():
        retry(lambda: destination.replace(backup), ATTEMPTS)
    try:
        retry(lambda: staged.replace(destination), ATTEMPTS)
    except OSError:
        if backup.exists():
            retry(lambda: backup.replace(destination), ATTEMPTS)
        raise
    if backup.exists():
        remove_tree(backup)


def extract_setup(source: Path, work: Path, log) -> Path:
    """Copy the disc image's Setup folder into the private work folder."""
    disc = work / "disc"
    disc.mkdir()
    # macOS has bsdtar at a fixed path; Linux distributions may not install it.
    bsdtar = Path("/usr/bin/bsdtar")
    if not WINDOWS and sys.platform == "darwin" and bsdtar.is_file():
        listing = subprocess.check_output([str(bsdtar), "-tf", str(source)], text=True)
        entry = next((x for x in listing.splitlines() if x.strip("./").lower() == "setup"), None)
        if not entry:
            raise ValueError("This image does not contain the Battlegrounds installer.")
        run([bsdtar, "-xf", source, "-C", disc, entry], log)
    else:
        log.write(f"\n> read ISO 9660 image {source}\n")
        extract_directory(source, "Setup", disc)
    return disc


def install(source: Path, home: Path, resources: Path, project: Path | None = None) -> None:
    home.mkdir(parents=True, exist_ok=True)
    with SetupLock(home / "install.lock"):
        if shutil.disk_usage(home).free < 3 * 1024**3:
            raise ValueError("Setup needs at least 3 GB of free space. Free some space and retry.")
        # Explicit UTF-8: the default is the locale's encoding (cp1252 on most Windows systems),
        # which cannot write the log's arrows and middle dots.
        with (home / "install.log").open("w", buffering=1, encoding="utf-8") as log:
            # Remove only abandoned private staging directories, while holding the lock.
            for stale in home.glob(".install-*"):
                if stale.is_dir() and not stale.is_symlink():
                    remove_tree(stale)
            # A folder Windows still holds open is removed with the stale ones next time.
            cleanup = dict(ignore_cleanup_errors=True) if WINDOWS else {}
            with tempfile.TemporaryDirectory(prefix=".install-", dir=home, **cleanup) as tmp:
                work = Path(tmp)
                emit("files", "active", "Reading your game disc…")
                if source.is_dir():
                    setup = disc_setup(source)
                else:
                    if not source.is_file() or source.suffix.lower() != ".iso":
                        raise ValueError("Choose a Battlegrounds ISO image or insert the original game CD.")
                    setup = disc_setup(extract_setup(source, work, log))
                emit("files", "active", "Unpacking the original game files…")
                unpacked = work / "unpacked"
                command, folder = unshield_command(resources, work, unpacked, child_named(setup, "data1.cab"))
                try:
                    run(command, log, **(dict(cwd=folder) if folder else {}))
                except subprocess.CalledProcessError:
                    raise ValueError("The game disc could not be unpacked. Check the disc or ISO image and retry; "
                                     "details are in the setup log.") from None
                game = child_named(unpacked, "App_Executables")
                manifest = json.loads((resources / "game-files.json").read_text(encoding="utf-8"))
                match_manifest_case(game, manifest)
                if not (game / "Data/OMFBGGUI.DEObjects").is_file():
                    raise ValueError("This disc edition is not supported. Use the original English 2.090 CD.")
                emit("files", "done", "Original game files extracted")
                emit("patches", "active", "Checking your disc and the bundled update…")
                apply_patch(game, resources, lambda done, total: emit(
                    "patches", "active", f"Applying bundled update · {done}/{total} files"))
                log.write("Bundled cumulative update applied · 2.090 → 2.123\n")
                emit("patches", "done", "Bundled update applied · 2.123")
                emit("runtime", "active", "Verifying every game file…")
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
            verify(args.verify, json.loads((args.resources / "game-files.json").read_text(encoding="utf-8")))
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
