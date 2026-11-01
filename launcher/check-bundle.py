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
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("resources", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="OpenBG offline setup ") as temporary:
        root = Path(temporary)
        resources = root / "Moved app with spaces/Resources"
        shutil.copytree(args.resources, resources, symlinks=True)
        magic = {b'\xfe\xed\xfa\xce', b'\xce\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'}
        for path in resources.rglob('*'):
            if path.is_file() and not path.is_symlink():
                with path.open('rb') as stream:
                    native = stream.read(4) in magic
                if native:
                    subprocess.run(['codesign', '--verify', '--strict', str(path)], check=True)
                    subprocess.run(['lipo', str(path), '-verify_arch', os.uname().machine], check=True)
        home = root / "Fresh game folder"
        home.mkdir()
        empty = root / "No game data"
        empty.mkdir()
        env = dict(os.environ, HOME=str(home), PATH="/usr/bin:/bin", PYTHONHOME="/invalid/python",
                   PYTHONPATH="/invalid/packages", HTTP_PROXY="http://127.0.0.1:1", HTTPS_PROXY="http://127.0.0.1:1")
        sandbox = ["/usr/bin/sandbox-exec", "-p", "(version 1) (allow default) (deny network*)"]
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
        print("Relocated packaged runtime, decoder, patch and installer passed with network access denied")


if __name__ == "__main__":
    main()
