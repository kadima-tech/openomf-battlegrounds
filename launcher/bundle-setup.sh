#!/bin/bash
# Build-time downloads only. The finished app never uses uv or the network.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
build="$here/build"
resources=${1:?app Resources directory}
arch=$(uname -m)
mkdir -p "$build/tools" "$resources/licenses"
uv="$build/tools/uv-0.9.7-$arch"
if [ ! -x "$uv" ]; then
    case "$arch" in
        arm64) triple=aarch64-apple-darwin; checksum=35572b9619fc14d67fc1cd72582c3cfc5c9c66d97f310192e04f26fb3fe96005 ;;
        x86_64) triple=x86_64-apple-darwin; checksum=41946d87e1576c297d6d3cca88b089b6942b8777a5a25e70de1ef8c57b94b9cf ;;
        *) echo "Unsupported architecture: $arch" >&2; exit 1 ;;
    esac
    archive="$build/tools/uv-$arch.tar.gz"
    curl --proto '=https' --tlsv1.2 -fL --retry 2 "https://github.com/astral-sh/uv/releases/download/0.9.7/uv-$triple.tar.gz" -o "$archive"
    echo "$checksum  $archive" | shasum -a 256 -c -
    tar -xzf "$archive" -C "$build/tools" "uv-$triple/uv"
    cp "$build/tools/uv-$triple/uv" "$uv"
fi

# uv embeds the distribution URLs/checksums. Its version and the exact Python
# version are pinned; keep this cache separate from other uv installations.
export UV_PYTHON_INSTALL_DIR="$build/python-uv-0.9.7"
export UV_CACHE_DIR="$build/uv-cache"
export UV_NO_CONFIG=1
"$uv" --no-config python install --no-bin 3.12.12
interpreter=$("$uv" --no-config python find --managed-python --no-python-downloads 3.12.12)
prefix=$("$interpreter" -I -c 'import sys; print(sys.base_prefix)')
# These are generated bundle directories. Remove older architectures/packages
# so rebuilding an existing app cannot accidentally retain obsolete tools.
rm -rf "$resources/python" "$resources/python-packages"
ditto "$prefix" "$resources/python"
"$uv" --no-config pip install --python "$resources/python/bin/python3" \
    --target "$resources/python-packages" --only-binary :all: --require-hashes \
    -r "$here/requirements.txt"
cp "$resources/python/lib/python3.12/LICENSE.txt" "$resources/licenses/Python.txt"
# Preserve all upstream runtime/package license files in the bundle, including
# the decoder's dist-info license. Nothing is installed on the build host.
"$resources/python/bin/python3" -I -B -c \
    'import sys; sys.path.insert(0, sys.argv[1]); import bsdiff4; assert bsdiff4.__version__ == "1.2.6"' \
    "$resources/python-packages"
