#!/bin/bash
# Runs only after choosing a disc. uv, Python, wheels and all caches stay local.
set -euo pipefail
resources=$(cd "$(dirname "$0")" && pwd)
game_home=$1
shift
mkdir -p "$game_home/runtime"
export UV_PYTHON_INSTALL_DIR="$game_home/runtime/python"
export UV_CACHE_DIR="$game_home/runtime/cache"
export UV_PYTHON_BIN_DIR="$game_home/runtime/bin"
export UV_TOOL_DIR="$game_home/runtime/tools"
export UV_TOOL_BIN_DIR="$game_home/runtime/bin"
export UV_NO_CONFIG=1
export UV_PYTHON_PREFERENCE=only-managed
export UV_LINK_MODE=copy
uv="$resources/bin/uv"
venv="$game_home/runtime/venv"
exec 3>&1
printf '%s\n' '{"stage":"runtime","status":"active","detail":"Preparing the local setup tools…"}'
# Bootstrap diagnostics must never mix with the JSON event stream.
exec 2>>"$game_home/bootstrap.log"
trap 'printf "%s\n" "{\"stage\":\"error\",\"status\":\"error\",\"detail\":\"Could not prepare the local setup tools. Check your internet connection and free space, then retry. See bootstrap.log in your game folder for details.\"}" >&3' ERR
if [ ! -x "$venv/bin/python3" ] || ! cmp -s "$resources/requirements.txt" "$game_home/runtime/installed-requirements.txt" || ! "$venv/bin/python3" -I -c 'import pefile, unicorn' >&2; then
    "$uv" --no-config venv --clear --python 3.12 "$venv" >&2
    "$uv" --no-config pip install --python "$venv/bin/python3" --require-hashes -r "$resources/requirements.txt" >&2
    cp "$resources/requirements.txt" "$game_home/runtime/installed-requirements.txt"
fi
printf '%s\n' '{"stage":"runtime","status":"waiting","detail":"Setup tools ready · installed locally"}'
exec "$venv/bin/python3" -I "$resources/install.py" --home "$game_home" --resources "$resources" "$@"
