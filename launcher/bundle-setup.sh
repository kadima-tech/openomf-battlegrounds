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

# uv 0.13.9 installs the hash-locked decoder. It does not select the interpreter:
# this release predates python-build-standalone 20261124, so its managed CPython
# is an older rebuild. The archive below is 20261124 (still CPython 3.15.2), which
# updates the bundled Expat, zstd and libffi for CVE-2026-134703, CVE-2026-124851
# and the libffi 3.8.1 advisories.
uv="$build/tools/uv-0.13.9-$os-$arch"
if [ ! -x "$uv" ]; then
    case "$os/$arch" in
        Darwin/arm64) triple=aarch64-apple-darwin; checksum=5aeca67f2283f75235de40d0bd9c0a5bbc5fce23b6d48b3bf9fa1b2db08382ca ;;
        Darwin/x86_64) triple=x86_64-apple-darwin; checksum=617a3211261b0fc739562f0071aa34e9739797c510208ede8e2977c30903d041 ;;
        Linux/x86_64) triple=x86_64-unknown-linux-gnu; checksum=a7fbaff2b9a271d85c1bdb540941090fd1ce011797b4abeb48e4dcd39c2471d5 ;;
        Linux/aarch64) triple=aarch64-unknown-linux-gnu; checksum=0797542018e541a9288855df555c8077eed06f7a47d206362030480e8ecc9f3d ;;
        *) echo "Unsupported platform: $os $arch" >&2; exit 1 ;;
    esac
    archive="$build/tools/uv-$os-$arch.tar.gz"
    curl --proto '=https' --tlsv1.2 -fL --retry 2 "https://github.com/astral-sh/uv/releases/download/0.13.9/uv-$triple.tar.gz" -o "$archive"
    check_sha256 "$archive" "$checksum"
    tar -xzf "$archive" -C "$build/tools" "uv-$triple/uv"
    cp "$build/tools/uv-$triple/uv" "$uv"
fi

case "$os/$arch" in
    Darwin/arm64) py_triple=aarch64-apple-darwin; py_checksum=2cc8874f034f7a0deff879530020108ebe6d318f5e8c6689673a866ab5fb7fdc ;;
    Darwin/x86_64) py_triple=x86_64-apple-darwin; py_checksum=f076837c5fb804e6f200841c3be3b2050633f7e19ea97934f81fd9727ada6b71 ;;
    Linux/x86_64) py_triple=x86_64-unknown-linux-gnu; py_checksum=d94bcba1ca1c63f591fa63109c3eeaacc76ee6796844ac7bdf2ae10715646f05 ;;
    Linux/aarch64) py_triple=aarch64-unknown-linux-gnu; py_checksum=dac9832fe240524c092015b99ed76fcef1a32a096d744fcb39eafa980d1a0ffa ;;
    *) echo "Unsupported platform: $os $arch" >&2; exit 1 ;;
esac
py_name="cpython-3.15.2+20261124-${py_triple}-install_only_stripped.tar.gz"
py_archive="$build/tools/$py_name"
if [ ! -f "$py_archive" ] || ! check_sha256 "$py_archive" "$py_checksum"; then
    curl --proto '=https' --tlsv1.2 -fL --retry 2 \
        "https://github.com/astral-sh/python-build-standalone/releases/download/20261124/cpython-3.15.2%2B20261124-${py_triple}-install_only_stripped.tar.gz" \
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
    'import sys; sys.path.insert(0, sys.argv[1]); import bsdiff4; assert bsdiff4.__version__ == "1.4.3"' \
    "$resources/python-packages"
