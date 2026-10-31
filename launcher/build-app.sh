#!/bin/bash
# Developer packaging step. The resulting .app needs no compiler or Homebrew.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
build="$here/build"
app="$repo/dist/Battlegrounds.app"
arch=$(uname -m)
sdk=$(xcrun --show-sdk-path)
mkdir -p "$build" "$app/Contents/MacOS" "$app/Contents/Resources/bin" "$app/Contents/Resources/assets"
resources="$app/Contents/Resources"

if [ "${1:-}" != "--ui-only" ]; then
    cmake -S "$here/cmake" -B "$build/runtime" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
        -DZLIB_LIBRARY="$sdk/usr/lib/libz.tbd" -DZLIB_INCLUDE_DIR="$sdk/usr/include"
    cmake --build "$build/runtime" --target openbg unshield -j "$(sysctl -n hw.ncpu)"
    cp "$build/runtime/engine/openbg" "$resources/bin/openbg"
    cp "$build/runtime/_deps/unshield-build/src/unshield" "$resources/bin/unshield"
    # Refuse to produce an app that secretly relies on a developer's package manager.
    for binary in "$resources/bin/openbg" "$resources/bin/unshield"; do
        if otool -L "$binary" | tail -n +2 | awk '{print $1}' | grep -Ev '^(/usr/lib/|/System/Library/)' ; then
            echo "Non-system dynamic dependency in $binary; app is not portable." >&2
            exit 1
        fi
    done
    mkdir -p "$resources/licenses"
    cp "$build/runtime/_deps/sdl2-src/LICENSE.txt" "$resources/licenses/SDL2.txt"
    cp "$build/runtime/_deps/unshield-src/LICENSE" "$resources/licenses/unshield.txt"
    cp "$repo/engine/third_party/dmusic/VENDORED.md" "$resources/licenses/dmusic.md"
    cp "$repo/engine/third_party/dmusic/LICENSE.md" "$resources/licenses/dmusic.txt"
    cp "$repo/engine/third_party/dmusic/LICENSE-DDKSynth.md" "$resources/licenses/DDKSynth.txt"
    indeo="$build/runtime/_deps/openindeo-src"
    if [ ! -d "$indeo" ]; then indeo="$repo/../openindeo"; fi
    cp "$indeo/LICENSE" "$resources/licenses/openindeo.txt"
fi

# Download the pinned standalone uv executable only into the app bundle.
if [ ! -x "$resources/bin/uv" ]; then
    case "$arch" in
        arm64) triple=aarch64-apple-darwin; checksum=35572b9619fc14d67fc1cd72582c3cfc5c9c66d97f310192e04f26fb3fe96005 ;;
        x86_64) triple=x86_64-apple-darwin; checksum=41946d87e1576c297d6d3cca88b089b6942b8777a5a25e70de1ef8c57b94b9cf ;;
        *) echo "Unsupported architecture: $arch" >&2; exit 1 ;;
    esac
    archive="$build/uv.tar.gz"
    curl --proto '=https' --tlsv1.2 -fL --retry 2 "https://github.com/astral-sh/uv/releases/download/0.9.7/uv-$triple.tar.gz" -o "$archive"
    echo "$checksum  $archive" | shasum -a 256 -c -
    tar -xzf "$archive" -C "$build" "uv-$triple/uv"
    cp "$build/uv-$triple/uv" "$resources/bin/uv"
fi

mkdir -p "$resources/licenses"
for license in LICENSE-MIT LICENSE-APACHE; do
    if [ ! -f "$resources/licenses/uv-$license" ]; then
        curl --proto '=https' --tlsv1.2 -fsSL --retry 2 "https://raw.githubusercontent.com/astral-sh/uv/0.9.7/$license" -o "$resources/licenses/uv-$license"
    fi
done

xcrun swiftc -O -parse-as-library -target "$arch-apple-macosx13.0" \
    "$here/Launcher.swift" "$here/Readiness.swift" -o "$app/Contents/MacOS/Battlegrounds" -framework SwiftUI -framework AppKit
xcrun clang -Os -mmacosx-version-min=13.0 "$here/worker.c" -o "$resources/bin/worker"
cp "$here/bootstrap.sh" "$here/install.py" "$here/requirements.txt" "$here/patches.json" "$here/game-files.json" "$resources/"
cp "$repo/re/scripts/rtpatch_emu.py" "$resources/"
cp "$here/assets/robot-blueprint.png" "$resources/assets/"
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>Battlegrounds</string>
  <key>CFBundleIdentifier</key><string>org.openomf.battlegrounds</string>
  <key>CFBundleName</key><string>Battlegrounds</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleShortVersionString</key><string>0.1.0</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
# Ad-hoc signing is useful for local runs. Public releases still need Developer ID
# signing and notarization; this script does not pretend to provide either.
codesign --force --deep --sign - "$app"
printf '\nBuilt %s\n' "$app"
