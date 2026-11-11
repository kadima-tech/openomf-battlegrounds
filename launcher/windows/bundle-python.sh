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

# uv 0.13.7 for the build host, as in ../bundle-setup.sh. It installs the decoder for Windows
# whatever the host is (--python-platform).
case "$(uname -s)-$(uname -m)" in
    MINGW*-x86_64|MSYS*-x86_64|CYGWIN*-x86_64) triple=x86_64-pc-windows-msvc; checksum=0cdc5b38fe5a8168ed6a25814f5ba43c21d313dd3187314fccb37b7cec46e021; kind=zip ;;
    Linux-x86_64) triple=x86_64-unknown-linux-gnu; checksum=c5221ff41b7892bfc223543883600125f6a47fe845093e0f5bdff4f359196d83; kind=tar.gz ;;
    Linux-aarch64) triple=aarch64-unknown-linux-gnu; checksum=71279f8d3c67241e807b9d8db872d1bf541579fb9f08b7d1eb1682196d1e57dc; kind=tar.gz ;;
    Darwin-arm64) triple=aarch64-apple-darwin; checksum=81b811b57e4624df7ed3aac148e0359100d9962f3d9dce00aec533dc2e086731; kind=tar.gz ;;
    Darwin-x86_64) triple=x86_64-apple-darwin; checksum=20c47e4c1f630b8abaff8c291fa9b9aa300fef92c4fbff95adf198b55287cda7; kind=tar.gz ;;
    *) echo "Unsupported build host: $(uname -s) $(uname -m)" >&2; exit 1 ;;
esac
uv="$tools/uv-0.13.7-$triple/uv"
if [ ! -x "$uv" ] && [ ! -x "$uv.exe" ]; then
    archive="$tools/uv-$triple.$kind"
    fetch "https://github.com/astral-sh/uv/releases/download/0.13.7/uv-$triple.$kind" "$archive" "$checksum"
    mkdir -p "$tools/uv-0.13.7-$triple"
    if [ "$kind" = zip ]; then
        "${PYTHON:-python3}" -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
            "$archive" "$tools/uv-0.13.7-$triple"
    else
        tar -xzf "$archive" -C "$tools/uv-0.13.7-$triple" --strip-components 1 "uv-$triple/uv"
    fi
fi
[ -x "$uv.exe" ] && uv="$uv.exe"

# CPython 3.15.2 for Windows x64: the python-build-standalone release uv 0.13.7 installs
# (its linux build stamp is 20261026). uv cannot install a Windows interpreter from
# another host, so the pinned archive is unpacked.
python_archive="$tools/cpython-3.15.2+20261026-x86_64-pc-windows-msvc-install_only_stripped.tar.gz"
fetch "https://github.com/astral-sh/python-build-standalone/releases/download/20261026/cpython-3.15.2%2B20261026-x86_64-pc-windows-msvc-install_only_stripped.tar.gz" \
    "$python_archive" 02e532b60e720aa3d77e757c349ae9cc945441c05851fa646336fc9ff0403041
# Generated bundle directories: never keep files from an older build.
rm -rf "$resources/python" "$resources/python-packages"
tar -xzf "$python_archive" -C "$resources"
test -f "$resources/python/python.exe"

export UV_CACHE_DIR="$tools/uv-cache"
export UV_NO_CONFIG=1
"$uv" --no-config pip install --python-platform x86_64-pc-windows-msvc --python-version 3.15 \
    --target "$resources/python-packages" --only-binary :all: --require-hashes --no-python-downloads \
    -r "$launcher/requirements.txt"
test -f "$resources/python-packages/bsdiff4/core.cp315-win_amd64.pyd"
cp "$resources/python/LICENSE.txt" "$resources/licenses/Python.txt"
