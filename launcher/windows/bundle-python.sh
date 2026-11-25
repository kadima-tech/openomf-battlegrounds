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

# uv 0.13.9 for the build host, as in ../bundle-setup.sh. It installs the decoder for Windows
# whatever the host is (--python-platform). It does not fetch this interpreter.
case "$(uname -s)-$(uname -m)" in
    MINGW*-x86_64|MSYS*-x86_64|CYGWIN*-x86_64) triple=x86_64-pc-windows-msvc; checksum=edd61f7f5728a56cc62455386661860bdfc7bab65872f39b0c25bdcc1d8e1beb; kind=zip ;;
    Linux-x86_64) triple=x86_64-unknown-linux-gnu; checksum=a7fbaff2b9a271d85c1bdb540941090fd1ce011797b4abeb48e4dcd39c2471d5; kind=tar.gz ;;
    Linux-aarch64) triple=aarch64-unknown-linux-gnu; checksum=0797542018e541a9288855df555c8077eed06f7a47d206362030480e8ecc9f3d; kind=tar.gz ;;
    Darwin-arm64) triple=aarch64-apple-darwin; checksum=5aeca67f2283f75235de40d0bd9c0a5bbc5fce23b6d48b3bf9fa1b2db08382ca; kind=tar.gz ;;
    Darwin-x86_64) triple=x86_64-apple-darwin; checksum=617a3211261b0fc739562f0071aa34e9739797c510208ede8e2977c30903d041; kind=tar.gz ;;
    *) echo "Unsupported build host: $(uname -s) $(uname -m)" >&2; exit 1 ;;
esac
uv="$tools/uv-0.13.9-$triple/uv"
if [ ! -x "$uv" ] && [ ! -x "$uv.exe" ]; then
    archive="$tools/uv-$triple.$kind"
    fetch "https://github.com/astral-sh/uv/releases/download/0.13.9/uv-$triple.$kind" "$archive" "$checksum"
    mkdir -p "$tools/uv-0.13.9-$triple"
    if [ "$kind" = zip ]; then
        "${PYTHON:-python3}" -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
            "$archive" "$tools/uv-0.13.9-$triple"
    else
        tar -xzf "$archive" -C "$tools/uv-0.13.9-$triple" --strip-components 1 "uv-$triple/uv"
    fi
fi
[ -x "$uv.exe" ] && uv="$uv.exe"

# CPython 3.15.2 for Windows x64, python-build-standalone 20261124: the same release
# ../bundle-setup.sh unpacks for macOS and Linux. uv 0.13.9 predates that rebuild,
# and it cannot install a Windows interpreter from another host.
python_archive="$tools/cpython-3.15.2+20261124-x86_64-pc-windows-msvc-install_only_stripped.tar.gz"
fetch "https://github.com/astral-sh/python-build-standalone/releases/download/20261124/cpython-3.15.2%2B20261124-x86_64-pc-windows-msvc-install_only_stripped.tar.gz" \
    "$python_archive" 547cdbb844edc425b235513fdf0267f64e8e59c8f484527bb7ef92d3aad91dd9
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
