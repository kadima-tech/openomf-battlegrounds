#!/bin/bash
# build.sh [out dir]: builds Kreissack.DEObjects (the pilot and the WAR Finale tournament) with the content editor.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-$here/build}
data=${OPENBG_DATA:-$root/source/patched/v2123/Data}
editor=${OPENBG_EDITOR:-$root/engine/build/openbg-editor}
home=$(mktemp -d)
trap 'rm -rf "$home"' EXIT
args() { while IFS= read -r l || [ -n "$l" ]; do [ -n "$l" ] && a+=("$l"); done < "$1"; }
mkdir -p "$out"
rm -f "$out/Kreissack.DEObjects"
a=(--new-pilot "General Brander:Major Kreissack")
args "$here/scripts/pilot_args.txt"
HOME=$home "$editor" --data "$data" "$out/Kreissack.DEObjects" "${a[@]}" --photo "$here/source/kreissack_cutout.png" --save --batch --frames 60
a=()
args "$here/scripts/tournament_args.txt"
HOME=$home "$editor" --data "$data" "$out/Kreissack.DEObjects" "${a[@]}" --compile --save --batch --frames 60
echo "built $out/Kreissack.DEObjects"
