#!/usr/bin/env python3
"""Exercise packaged setup after relocation, with a fresh home and no network.

CI needs no game disc. The installer must start its bundled interpreter/decoder,
validate its real update archive, and return the expected missing-disc-file error.
Real-disc end-to-end installation is a separate local check.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

MACH_O = {b'\xfe\xed\xfa\xce', b'\xce\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xcf\xfa\xed\xfe',
          b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'}
ELF_MACHINES = {"x86_64": 62, "aarch64": 183}


def check_native(path: Path) -> None:
    """Signed for this architecture on macOS; built for this architecture on Linux."""
    with path.open('rb') as stream:
        header = stream.read(20)
    if sys.platform == "darwin":
        if header[:4] in MACH_O:
            subprocess.run(['codesign', '--verify', '--strict', str(path)], check=True)
            subprocess.run(['lipo', str(path), '-verify_arch', os.uname().machine], check=True)
    elif header[:4] == b'\x7fELF':
        machine = int.from_bytes(header[18:20], "little" if header[5] == 1 else "big")
        if machine != ELF_MACHINES.get(os.uname().machine):
            raise RuntimeError(f"{path} is not built for {os.uname().machine}")


def network_denied() -> list[str]:
    if sys.platform == "darwin":
        return ["/usr/bin/sandbox-exec", "-p", "(version 1) (allow default) (deny network*)"]
    # A new user and network namespace has only an unconfigured loopback device.
    command = ["unshare", "--user", "--map-root-user", "--net", "--"]
    if subprocess.run(command + ["true"], capture_output=True).returncode != 0:
        raise RuntimeError("unshare could not create a network-less namespace. Enable unprivileged user "
                           "namespaces (Ubuntu: sysctl kernel.apparmor_restrict_unprivileged_userns=0).")
    return command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("resources", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="OpenBG offline setup ") as temporary:
        root = Path(temporary)
        resources = root / "Moved app with spaces/Resources"
        shutil.copytree(args.resources, resources, symlinks=True)
        for path in resources.rglob('*'):
            if path.is_file() and not path.is_symlink():
                check_native(path)
        home = root / "Fresh game folder"
        home.mkdir()
        empty = root / "No game data"
        empty.mkdir()
        env = dict(os.environ, HOME=str(home), PATH="/usr/bin:/bin", PYTHONHOME="/invalid/python",
                   PYTHONPATH="/invalid/packages", HTTP_PROXY="http://127.0.0.1:1", HTTPS_PROXY="http://127.0.0.1:1")
        sandbox = network_denied()
        python = resources / "python/bin/python3"
        # Run a real decoder round trip, not just an import of an empty module.
        subprocess.run(sandbox + [str(python), "-I", "-B", "-c",
            "import sys; sys.path.insert(0, sys.argv[1]); import bsdiff4; "
            "assert bsdiff4.patch(b'old', bsdiff4.diff(b'old', b'new')) == b'new'",
            str(resources / "python-packages")], env=env, check=True)
        subprocess.run(sandbox + [str(python), "-I", "-B", str(resources / "patching.py"), str(resources)],
                       env=env, check=True)
        result = subprocess.run(sandbox + ["/bin/bash", str(resources / "bootstrap.sh"), str(home),
                                "--verify", str(empty)], env=env, capture_output=True, text=True)
        events = [json.loads(line) for line in result.stdout.splitlines()]
        if result.returncode != 1 or not events or not events[-1].get("detail", "").startswith("Game file missing or incomplete:"):
            raise RuntimeError(f"Packaged setup did not start correctly: {result.stdout} {result.stderr}")
        if (home / "runtime").exists() or (home / "patches").exists():
            raise RuntimeError("Setup unexpectedly created a download/tool cache")
        # Linux ISO setup reads images with the bundled Python, never bsdtar.
        if sys.platform != "darwin":
            iso = root / "Disc image with spaces.iso"
            build_iso(iso)
            result = subprocess.run(sandbox + [str(resources / "bin/worker"), str(resources / "bootstrap.sh"),
                                    str(root / "New game folder"), "--source", str(iso)], env=env,
                                    capture_output=True, text=True)
            events = [json.loads(line) for line in result.stdout.splitlines()]
            details = [event.get("detail", "") for event in events]
            if (result.returncode != 1 or "Unpacking the original game files…" not in details
                    or not details[-1].startswith("The game disc could not be unpacked")):
                raise RuntimeError(f"Packaged ISO setup did not reach extraction: {result.stdout} {result.stderr}")
        print("Relocated packaged runtime, decoder, patch and installer passed with network access denied")


def build_iso(path: Path) -> None:
    """A minimal ISO 9660 image holding a Battlegrounds-shaped Setup folder (no game data)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from test_iso9660 import DISC, Builder
    Builder(DISC).write(path)


if __name__ == "__main__":
    main()
