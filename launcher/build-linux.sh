#!/bin/bash
# Developer packaging step for Linux. dist/Battlegrounds needs no compiler, SDL2,
# Python or distro packages on the player's machine; the desktop supplies GTK 3,
# OpenGL and the display/audio servers.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
build="$here/build"
app="$repo/dist/Battlegrounds"
resources="$app/resources"
version=${OPENBG_VERSION:-0.1.0}
build_number=${OPENBG_BUILD_NUMBER:-1}
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || [[ ! "$build_number" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]]; then
    echo "OPENBG_VERSION must be major.minor.patch; OPENBG_BUILD_NUMBER must have one to three numeric components." >&2
    exit 1
fi
for tool in cmake cc c++ pkg-config python3 curl readelf; do
    command -v "$tool" >/dev/null || { echo "Missing build tool: $tool (see launcher/README.md#linux)" >&2; exit 1; }
done
if ! pkg-config --exists gtk+-3.0 gio-unix-2.0; then
    echo "GTK 3 development files are missing (Debian/Ubuntu: libgtk-3-dev; Fedora: gtk3-devel)." >&2
    exit 1
fi
# The engine links libstdc++ statically so players need no particular GCC.
if [ "${1:-}" != "--ui-only" ] && [ "$(c++ -print-file-name=libstdc++.a)" = "libstdc++.a" ]; then
    echo "The static C++ runtime is missing (Fedora: libstdc++-static; Debian/Ubuntu ship it with g++)." >&2
    exit 1
fi
mkdir -p "$build" "$resources/bin" "$resources/assets" "$resources/licenses"
# Failed and UI-only builds must not mark a partially updated app as current.
state="$repo/dist/.launcher-build-state"
build_state=$(python3 "$here/build_state.py")
rm -f "$state"
cp "$repo/LICENSE" "$resources/licenses/openomf-battlegrounds.txt"
cp "$repo/THIRD_PARTY_NOTICES.md" "$resources/licenses/THIRD_PARTY_NOTICES.md"

python3 "$here/patching.py" "$here"

if [ "${1:-}" != "--ui-only" ]; then
    runtime="$build/runtime-linux"
    cmake -S "$here/cmake" -B "$runtime" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    # SDL2 only builds the video/audio back ends whose headers exist. An engine
    # without X11 or a sound back end would build and then fail on players' desktops.
    sdl_config=$(find "$runtime/_deps/sdl2-build" -name SDL_config.h -path '*include*' | head -n 1)
    for feature in SDL_VIDEO_DRIVER_X11 SDL_VIDEO_DRIVER_WAYLAND SDL_VIDEO_OPENGL_GLX SDL_VIDEO_OPENGL_EGL \
                   SDL_AUDIO_DRIVER_PULSEAUDIO SDL_AUDIO_DRIVER_ALSA; do
        if ! grep -Eq "^#define $feature +1" "$sdl_config"; then
            echo "SDL2 was configured without $feature. Install the development packages listed in launcher/README.md#linux." >&2
            exit 1
        fi
    done
    cmake --build "$runtime" --target openbg unshield -j "$(nproc)"
    install -m 755 "$runtime/engine/openbg" "$resources/bin/openbg"
    install -m 755 "$runtime/_deps/unshield-build/src/unshield" "$resources/bin/unshield"
    strip "$resources/bin/openbg" "$resources/bin/unshield"
    # Refuse to produce an app that secretly relies on build-machine libraries.
    allowed='^(libc|libm|libdl|libpthread|librt|libz)\.so\.[0-9]+$|^ld-linux[-a-z0-9_]*\.so\.[0-9]+$|^lib(GL|OpenGL|GLX|EGL)\.so\.[0-9]+$'
    for binary in "$resources/bin/openbg" "$resources/bin/unshield"; do
        if readelf -d "$binary" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | grep -Ev "$allowed"; then
            echo "Non-system dynamic dependency in $binary; app is not portable." >&2
            exit 1
        fi
    done
    cp "$runtime/_deps/sdl2-src/LICENSE.txt" "$resources/licenses/SDL2.txt"
    cp "$runtime/_deps/unshield-src/LICENSE" "$resources/licenses/unshield.txt"
    cp "$repo/engine/third_party/dmusic/VENDORED.md" "$resources/licenses/dmusic.md"
    cp "$repo/engine/third_party/dmusic/LICENSE.md" "$resources/licenses/dmusic.txt"
    cp "$repo/engine/third_party/dmusic/LICENSE-DDKSynth.md" "$resources/licenses/DDKSynth.txt"
    indeo="$runtime/_deps/openindeo-src"
    if [ ! -d "$indeo" ]; then indeo="$repo/../openindeo"; fi
    cp "$indeo/LICENSE" "$resources/licenses/openindeo.txt"
fi

# Bundle a relocatable Python and decoder. Player setup is offline.
bash "$here/bundle-setup.sh" "$resources"
# Tk is not used on Linux; its notice ships with the runtime that contains it.
if [ -f "$resources/python/lib/tk8.6/demos/license.terms" ]; then
    cp "$resources/python/lib/tk8.6/demos/license.terms" "$resources/licenses/Tcl-Tk.txt"
fi

gtk_flags=$(pkg-config --cflags gtk+-3.0 gio-unix-2.0)
gtk_libs=$(pkg-config --libs gtk+-3.0 gio-unix-2.0)
# shellcheck disable=SC2086
cc -std=gnu11 -O2 -Wall -Wextra $gtk_flags "$here/linux-launcher.c" -o "$app/Battlegrounds" $gtk_libs
cc -std=gnu11 -Os -Wall "$here/worker.c" -o "$resources/bin/worker"
strip "$app/Battlegrounds" "$resources/bin/worker"
cp "$here/bootstrap.sh" "$here/install.py" "$here/patching.py" "$here/iso9660.py" "$here/readiness.py" \
   "$here/requirements.txt" "$here/cumulative-patch.json" "$here/game-files.json" "$resources/"
mkdir -p "$resources/patches"
cp "$here/patches/english-2090-2123.obgpatch" "$resources/patches/"
"$resources/python/bin/python3" -I -B "$resources/patching.py" "$resources"
cp "$here/assets/robot-blueprint.png" "$resources/assets/"
printf '%s (%s)\n' "$version" "$build_number" > "$resources/VERSION"
if [ "${1:-}" != "--ui-only" ]; then
    printf '%s\n' "$build_state" > "$state"
fi
printf '\nBuilt %s\n' "$app"
