#!/bin/bash
# Developer packaging step. The resulting .app needs no compiler or Homebrew.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
build="$here/build"
app="$repo/dist/Battlegrounds.app"
arch=$(uname -m)
sdk=$(xcrun --show-sdk-path)
version=${OPENBG_VERSION:-0.1.0}
build_number=${OPENBG_BUILD_NUMBER:-1}
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || [[ ! "$build_number" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]]; then
    echo "OPENBG_VERSION must be major.minor.patch; OPENBG_BUILD_NUMBER must have one to three numeric components." >&2
    exit 1
fi
mkdir -p "$build" "$app/Contents/MacOS" "$app/Contents/Resources/bin" "$app/Contents/Resources/assets"
resources="$app/Contents/Resources"
# Failed and UI-only builds must not mark a partially updated app as current.
state="$repo/dist/.launcher-build-state"
build_state=$(python3 "$here/build_state.py")
rm -f "$state"
mkdir -p "$resources/licenses"
cp "$repo/LICENSE" "$resources/licenses/openomf-battlegrounds.txt"
cp "$repo/THIRD_PARTY_NOTICES.md" "$resources/licenses/THIRD_PARTY_NOTICES.md"

python3 "$here/patching.py" "$here"

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
    cp "$build/runtime/_deps/sdl2-src/LICENSE.txt" "$resources/licenses/SDL2.txt"
    cp "$build/runtime/_deps/unshield-src/LICENSE" "$resources/licenses/unshield.txt"
    cp "$repo/engine/third_party/dmusic/VENDORED.md" "$resources/licenses/dmusic.md"
    cp "$repo/engine/third_party/dmusic/LICENSE.md" "$resources/licenses/dmusic.txt"
    cp "$repo/engine/third_party/dmusic/LICENSE-DDKSynth.md" "$resources/licenses/DDKSynth.txt"
    indeo="$build/runtime/_deps/openindeo-src"
    if [ ! -d "$indeo" ]; then indeo="$repo/../openindeo"; fi
    cp "$indeo/LICENSE" "$resources/licenses/openindeo.txt"
fi

# Bundle a relocatable Python and decoder before signing. Player setup is offline.
bash "$here/bundle-setup.sh" "$resources"
# Clean up resources left by the former download/emulation-based installer.
rm -f "$resources/bin/uv" "$resources/rtpatch_emu.py" "$resources/patches.json"

xcrun swiftc -O -parse-as-library -target "$arch-apple-macosx13.0" \
    "$here/Launcher.swift" "$here/Readiness.swift" -o "$app/Contents/MacOS/Battlegrounds" -framework SwiftUI -framework AppKit
xcrun clang -Os -mmacosx-version-min=13.0 "$here/worker.c" -o "$resources/bin/worker"
cp "$here/bootstrap.sh" "$here/install.py" "$here/patching.py" "$here/requirements.txt" "$here/cumulative-patch.json" "$here/game-files.json" "$resources/"
mkdir -p "$resources/patches"
cp "$here/patches/english-2090-2123.obgpatch" "$resources/patches/"
"$resources/python/bin/python3" -I -B "$resources/patching.py" "$resources"
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
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $build_number" "$app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $version" "$app/Contents/Info.plist"
# Helpers and Python extensions live in resource subdirectories. Sign each
# Mach-O explicitly; --deep does not discover all of this embedded code.
python3 "$here/signing.py" "$resources"
# Ad-hoc signing is useful for local runs. Public releases still need Developer ID
# signing and notarization; this script does not pretend to provide either.
codesign --force --deep --sign - "$app"
if [ "${1:-}" != "--ui-only" ]; then
    printf '%s\n' "$build_state" > "$state"
fi
printf '\nBuilt %s\n' "$app"
