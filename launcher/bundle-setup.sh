#!/bin/bash
# Build-time downloads only. The finished app never uses uv or the network.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
build="$here/build"
resources=${1:?app Resources directory}
arch=$(uname -m)
os=$(uname -s)
mkdir -p "$build/tools" "$resources/licenses"

check_sha256() {  # file expected
    if command -v sha256sum >/dev/null; then echo "$2  $1" | sha256sum -c - >/dev/null
    else echo "$2  $1" | shasum -a 256 -c - >/dev/null; fi
}

# uv 0.13.7 installs the hash-locked decoder. It does not select the interpreter:
# its managed CPython is still python-build-standalone 20261026. The archive below
# is release 20261110, the newer rebuild of CPython 3.15.2 (zlib 1.4, Expat 2.10.0).
uv="$build/tools/uv-0.13.7-$os-$arch"
if [ ! -x "$uv" ]; then
    case "$os/$arch" in
        Darwin/arm64) triple=aarch64-apple-darwin; checksum=81b811b57e4624df7ed3aac148e0359100d9962f3d9dce00aec533dc2e086731 ;;
        Darwin/x86_64) triple=x86_64-apple-darwin; checksum=20c47e4c1f630b8abaff8c291fa9b9aa300fef92c4fbff95adf198b55287cda7 ;;
        Linux/x86_64) triple=x86_64-unknown-linux-gnu; checksum=c5221ff41b7892bfc223543883600125f6a47fe845093e0f5bdff4f359196d83 ;;
        Linux/aarch64) triple=aarch64-unknown-linux-gnu; checksum=71279f8d3c67241e807b9d8db872d1bf541579fb9f08b7d1eb1682196d1e57dc ;;
        *) echo "Unsupported platform: $os $arch" >&2; exit 1 ;;
    esac
    archive="$build/tools/uv-$os-$arch.tar.gz"
    curl --proto '=https' --tlsv1.2 -fL --retry 2 "https://github.com/astral-sh/uv/releases/download/0.13.7/uv-$triple.tar.gz" -o "$archive"
    check_sha256 "$archive" "$checksum"
    tar -xzf "$archive" -C "$build/tools" "uv-$triple/uv"
    cp "$build/tools/uv-$triple/uv" "$uv"
fi

case "$os/$arch" in
    Darwin/arm64) py_triple=aarch64-apple-darwin; py_checksum=7e58607647b5b4832092a339ff1c55eeeccee128221232509b8fc8d27957065e ;;
    Darwin/x86_64) py_triple=x86_64-apple-darwin; py_checksum=11143c2a2556b14619e8241d3f74bb43ad97181c0eb4909bab87d7aad1154e49 ;;
    Linux/x86_64) py_triple=x86_64-unknown-linux-gnu; py_checksum=ad23d1b73fac26440b0306ae06911031344c49a90ec19716c8362e7fa40cae43 ;;
    Linux/aarch64) py_triple=aarch64-unknown-linux-gnu; py_checksum=d1367a6c3cd86a9e35712072841f1376859f3b47a2a54e224421a0f9fd169047 ;;
    *) echo "Unsupported platform: $os $arch" >&2; exit 1 ;;
esac
py_name="cpython-3.15.2+20261110-${py_triple}-install_only_stripped.tar.gz"
py_archive="$build/tools/$py_name"
if [ ! -f "$py_archive" ] || ! check_sha256 "$py_archive" "$py_checksum"; then
    curl --proto '=https' --tlsv1.2 -fL --retry 2 \
        "https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.15.2%2B20261110-${py_triple}-install_only_stripped.tar.gz" \
        -o "$py_archive.part"
    check_sha256 "$py_archive.part" "$py_checksum" || { echo "Checksum mismatch for $py_name" >&2; rm -f "$py_archive.part"; exit 1; }
    mv "$py_archive.part" "$py_archive"
fi

# These are generated bundle directories. Remove older architectures/packages
# so rebuilding an existing app cannot accidentally retain obsolete tools.
export UV_CACHE_DIR="$build/uv-cache"
export UV_NO_CONFIG=1
rm -rf "$resources/python" "$resources/python-packages"
tar -xzf "$py_archive" -C "$resources"
test -x "$resources/python/bin/python3"
"$uv" --no-config pip install --python "$resources/python/bin/python3" \
    --target "$resources/python-packages" --only-binary :all: --require-hashes \
    -r "$here/requirements.txt"
cp "$resources/python/lib/python3.15/LICENSE.txt" "$resources/licenses/Python.txt"
# Preserve all upstream runtime/package license files in the bundle, including
# the decoder's dist-info license. Nothing is installed on the build host.
"$resources/python/bin/python3" -I -B -c \
    'import sys; sys.path.insert(0, sys.argv[1]); import bsdiff4; assert bsdiff4.__version__ == "1.4.2"' \
    "$resources/python-packages"
