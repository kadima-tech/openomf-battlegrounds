#!/usr/bin/env python3
"""Explicitly sign native code embedded outside standard macOS bundle paths."""
from pathlib import Path
import subprocess


def sign_native_code(resources: Path) -> None:
    magic = {b'\xfe\xed\xfa\xce', b'\xce\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'}
    for folder in ('bin', 'python', 'python-packages'):
        for path in (resources / folder).rglob('*'):
            if path.is_file() and not path.is_symlink():
                with path.open('rb') as stream:
                    native = stream.read(4) in magic
                if native:
                    subprocess.run(['codesign', '--force', '--sign', '-', str(path)], check=True)


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("resources", type=Path)
    sign_native_code(parser.parse_args().resources)
