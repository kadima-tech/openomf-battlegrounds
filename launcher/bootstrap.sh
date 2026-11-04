#!/bin/bash
# All setup tools ship inside the app. First-run setup never downloads anything.
set -euo pipefail
resources=$(cd "$(dirname "$0")" && pwd)
game_home=$1
shift
mkdir -p "$game_home"
exec 3>&1
exec 2>>"$game_home/bootstrap.log"
trap 'printf "%s\n" "{\"stage\":\"error\",\"status\":\"error\",\"detail\":\"Could not start the bundled setup tools. Reinstall the app or check bootstrap.log in your game folder.\"}" >&3' ERR
printf '%s\n' '{"stage":"runtime","status":"waiting","detail":"Bundled setup tools ready · no downloads needed"}'
# Isolated Python ignores user environments. Explicit paths allow the app to
# move to another Mac or a folder with spaces without a virtualenv/shebang.
exec "$resources/python/bin/python3" -I -B -c \
    'import runpy, sys; root = sys.argv.pop(1); sys.path[:0] = [root, root + "/python-packages"]; sys.argv[0] = root + "/install.py"; runpy.run_path(sys.argv[0], run_name="__main__")' \
    "$resources" --home "$game_home" --resources "$resources" "$@"
