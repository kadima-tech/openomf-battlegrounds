#!/bin/bash
# launch.cmd's MSYS2 half: rebuild dist/Battlegrounds when launcher, engine or packaging inputs
# changed (including added or deleted files), exactly like launch.command on macOS.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
python=${PYTHON:-$(command -v python3 || command -v python)}
if [ ! -f "$repo/dist/Battlegrounds/Battlegrounds.exe" ] || \
   ! "$python" "$repo/launcher/build_state.py" --check "$repo/dist/.launcher-build-state-windows"; then
    echo "Building Battlegrounds for the current sources…"
    "$repo/launcher/windows/build-windows.sh"
fi
