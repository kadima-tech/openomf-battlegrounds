#!/bin/bash
# Windows 10/11 packaging step: builds dist/Battlegrounds/ (Battlegrounds.exe and resources/).
# The result needs no compiler, MSYS2, Python or DLLs beyond those every Windows 10/11 PC has.
#
# Run it in an MSYS2 UCRT64 shell on Windows, or cross-compile from Linux/macOS with a
# MinGW-w64 toolchain (x86_64-w64-mingw32-gcc). Needs CMake 3.20+, Ninja, Python 3 and network
# access for the pinned dependencies.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
launcher=$(cd "$here/.." && pwd)
repo=$(cd "$launcher/.." && pwd)
build="$launcher/build/windows"
app="$repo/dist/Battlegrounds"
resources="$app/resources"
version=${OPENBG_VERSION:-0.1.0}
build_number=${OPENBG_BUILD_NUMBER:-1}
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || [[ ! "$build_number" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]]; then
    echo "OPENBG_VERSION must be major.minor.patch; OPENBG_BUILD_NUMBER must have one to three numeric components." >&2
    exit 1
fi
source "$here/require-tools.sh"
require_windows_build_tools || exit 1
python=$PYTHON
jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) native=1 ;;
    *) native=0 ;;
esac

# Failed builds must not mark a partially updated app as current.
state="$repo/dist/.launcher-build-state-windows"
build_state=$("$python" "$launcher/build_state.py")
mkdir -p "$repo/dist" "$build/tools"
rm -f "$state"
"$python" "$launcher/patching.py" "$launcher"

cmake_args=(-S "$launcher/cmake" -B "$build/runtime" -G Ninja -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DOPENBG_VERSION="$version" -DOPENBG_BUILD_NUMBER="$build_number")
if [ "$native" = 0 ]; then
    cmake_args+=(-DCMAKE_TOOLCHAIN_FILE="$here/mingw-w64-x86_64.cmake")
fi
cmake "${cmake_args[@]}" ${OPENBG_CMAKE_ARGS:-}
# openbg-editor is compiled to check it builds; it is not part of the app.
cmake --build "$build/runtime" --target openbg unshield openbg-editor Battlegrounds test-readiness test-launcher -j "$jobs"

rm -rf "$app"
mkdir -p "$resources/bin" "$resources/assets" "$resources/licenses" "$resources/patches"
cp "$build/runtime/Battlegrounds.exe" "$app/Battlegrounds.exe"
cp "$build/runtime/engine/openbg.exe" "$resources/bin/openbg.exe"
cp "$build/runtime/_deps/unshield-build/src/unshield.exe" "$resources/bin/unshield.exe"
# Refuse to produce a download that secretly relies on a compiler runtime or library DLL.
"$python" "$here/check-imports.py" "$app/Battlegrounds.exe" "$resources/bin/openbg.exe" "$resources/bin/unshield.exe"

# Fetched sources, or local checkouts given with -DFETCHCONTENT_SOURCE_DIR_<NAME>=... in OPENBG_CMAKE_ARGS.
source_dir() {
    local override
    override=$(sed -n "s/^FETCHCONTENT_SOURCE_DIR_$1:[A-Z]*=//p" "$build/runtime/CMakeCache.txt")
    echo "${override:-$build/runtime/_deps/$2-src}"
}
cp "$repo/LICENSE" "$resources/licenses/openomf-battlegrounds.txt"
cp "$repo/THIRD_PARTY_NOTICES.md" "$resources/licenses/THIRD_PARTY_NOTICES.md"
cp "$(source_dir SDL2 sdl2)/LICENSE.txt" "$resources/licenses/SDL2.txt"
cp "$(source_dir UNSHIELD unshield)/LICENSE" "$resources/licenses/unshield.txt"
cp "$(source_dir ZLIB zlib)/LICENSE" "$resources/licenses/zlib.txt"
cp "$repo/engine/third_party/dmusic/VENDORED.md" "$resources/licenses/dmusic.md"
cp "$repo/engine/third_party/dmusic/LICENSE.md" "$resources/licenses/dmusic.txt"
cp "$repo/engine/third_party/dmusic/LICENSE-DDKSynth.md" "$resources/licenses/DDKSynth.txt"
cp "$(source_dir OPENINDEO openindeo)/LICENSE" "$resources/licenses/openindeo.txt"
cp "$(source_dir ENET enet)/LICENSE" "$resources/licenses/enet.txt"

# A relocatable CPython and the decoder, both for Windows x64, whatever the build host is.
bash "$here/bundle-python.sh" "$resources" "$build/tools"

cp "$launcher/install.py" "$launcher/patching.py" "$launcher/iso9660.py" "$launcher/requirements.txt" \
    "$launcher/cumulative-patch.json" "$launcher/game-files.json" "$resources/"
cp "$launcher/patches/english-2090-2123.obgpatch" "$resources/patches/"
"$python" -I -B "$resources/patching.py" "$resources"
cp "$launcher/assets/robot-blueprint.png" "$resources/assets/"
printf '%s\n' "$build_state" > "$state"
printf '\nBuilt %s\n' "$app"
