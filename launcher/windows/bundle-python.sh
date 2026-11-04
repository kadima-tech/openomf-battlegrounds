#!/bin/bash
# Build-time downloads only: a relocatable CPython for Windows x64 and the hash-locked decoder.
# The finished app never uses uv or the network. Works on Windows (MSYS2), Linux and macOS hosts.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
launcher=$(cd "$here/.." && pwd)
resources=${1:?resources directory}
tools=${2:?build tools directory}
mkdir -p "$tools" "$resources/licenses"

verify() {  # file sha256
    if command -v sha256sum >/dev/null; then echo "$2  $1" | sha256sum -c - >/dev/null
    else echo "$2  $1" | shasum -a 256 -c - >/dev/null; fi
}
fetch() {  # url file sha256
    if [ ! -f "$2" ] || ! verify "$2" "$3"; then
        curl --proto '=https' --tlsv1.2 -fL --retry 2 "$1" -o "$2.part"
        verify "$2.part" "$3" || { echo "Checksum mismatch for $1" >&2; rm -f "$2.part"; exit 1; }
        mv "$2.part" "$2"
    fi
}

# uv 0.9.7 for the build host, as in ../bundle-setup.sh. It installs the decoder for Windows
# whatever the host is (--python-platform).
case "$(uname -s)-$(uname -m)" in
    MINGW*-x86_64|MSYS*-x86_64|CYGWIN*-x86_64) triple=x86_64-pc-windows-msvc; checksum=5d250c32d3604e28dbe18dc65c668ff628c53e00dde2c642576e831e4a60da64; kind=zip ;;
    Linux-x86_64) triple=x86_64-unknown-linux-gnu; checksum=b26fcc8dfa1c39b5a5613445af3be3eefda45d9a39359bee271eafe34913583e; kind=tar.gz ;;
    Linux-aarch64) triple=aarch64-unknown-linux-gnu; checksum=8b3d31a154673c6d357727d2083a33525b515589d153fa5b5455e1db9e9e6363; kind=tar.gz ;;
    Darwin-arm64) triple=aarch64-apple-darwin; checksum=35572b9619fc14d67fc1cd72582c3cfc5c9c66d97f310192e04f26fb3fe96005; kind=tar.gz ;;
    Darwin-x86_64) triple=x86_64-apple-darwin; checksum=41946d87e1576c297d6d3cca88b089b6942b8777a5a25e70de1ef8c57b94b9cf; kind=tar.gz ;;
    *) echo "Unsupported build host: $(uname -s) $(uname -m)" >&2; exit 1 ;;
esac
uv="$tools/uv-0.9.7-$triple/uv"
if [ ! -x "$uv" ] && [ ! -x "$uv.exe" ]; then
    archive="$tools/uv-$triple.$kind"
    fetch "https://github.com/astral-sh/uv/releases/download/0.9.7/uv-$triple.$kind" "$archive" "$checksum"
    mkdir -p "$tools/uv-0.9.7-$triple"
    if [ "$kind" = zip ]; then
        "${PYTHON:-python3}" -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
            "$archive" "$tools/uv-0.9.7-$triple"
    else
        tar -xzf "$archive" -C "$tools/uv-0.9.7-$triple" --strip-components 1 "uv-$triple/uv"
    fi
fi
[ -x "$uv.exe" ] && uv="$uv.exe"

# CPython 3.12.12 for Windows x64: the same python-build-standalone release uv 0.9.7 installs.
# uv cannot install a Windows interpreter from another host, so the pinned archive is unpacked.
python_archive="$tools/cpython-3.12.12+20251028-x86_64-pc-windows-msvc-install_only_stripped.tar.gz"
fetch "https://github.com/astral-sh/python-build-standalone/releases/download/20251028/cpython-3.12.12%2B20251028-x86_64-pc-windows-msvc-install_only_stripped.tar.gz" \
    "$python_archive" b01ce5aed9bae58023ccfba76c9cf0e789aa2ca36057ee6e6af054da2a2d4eb1
# Generated bundle directories: never keep files from an older build.
rm -rf "$resources/python" "$resources/python-packages"
tar -xzf "$python_archive" -C "$resources"
test -f "$resources/python/python.exe"

export UV_CACHE_DIR="$tools/uv-cache"
export UV_NO_CONFIG=1
"$uv" --no-config pip install --python-platform x86_64-pc-windows-msvc --python-version 3.12 \
    --target "$resources/python-packages" --only-binary :all: --require-hashes --no-python-downloads \
    -r "$launcher/requirements.txt"
test -f "$resources/python-packages/bsdiff4/core.cp312-win_amd64.pyd"
cp "$resources/python/LICENSE.txt" "$resources/licenses/Python.txt"
