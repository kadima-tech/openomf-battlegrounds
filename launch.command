#!/bin/bash
# Double-click in Finder, or run ./launch.command from a terminal.
set -euo pipefail
repo=$(cd "$(dirname "$0")" && pwd)
app="$repo/dist/Battlegrounds.app"
if [ ! -x "$app/Contents/MacOS/Battlegrounds" ]; then
    "$repo/launcher/build-app.sh"
fi
export OPENBG_PROJECT_ROOT="$repo"
exec "$app/Contents/MacOS/Battlegrounds" "$@"
