#!/bin/bash
# Linux: run ./launch.sh from a terminal (or your file manager) to build and open
# the launcher. macOS uses launch.command.
set -euo pipefail
repo=$(cd "$(dirname "$0")" && pwd)
app="$repo/dist/Battlegrounds"
if [ ! -x "$app/Battlegrounds" ] || \
   ! python3 "$repo/launcher/build_state.py" --check "$repo/dist/.launcher-build-state"; then
    echo "Building Battlegrounds for the current sources…"
    "$repo/launcher/build-linux.sh"
fi
export OPENBG_PROJECT_ROOT="$repo"
exec "$app/Battlegrounds" "$@"
