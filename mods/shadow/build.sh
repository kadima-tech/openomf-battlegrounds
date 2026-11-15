#!/bin/bash
# build.sh [--model] [out dir]: builds Shadow.DEObjects by importing source/shadow.glb onto Katana with the content editor,
# his 2097 moves added by script/shadow.bgs. --model first prepares source/ from the user's model blender/shadow-model.fbx
# with blender/prepare_shadow.py (needs Blender; TEXTURE=file.png uses that texture instead of the generated one).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
if [ "$1" = --model ]; then
  shift
  blender=${BLENDER:-/Applications/Blender.app/Contents/MacOS/Blender}
  "$blender" -b --factory-startup --python "$here/blender/prepare_shadow.py" -- --fbx "$here/blender/shadow-model.fbx" \
    --out "$here/source/shadow.glb" --blend "$here/source/shadow.blend" --paint-out "$here/source" \
    --picture "$here/source/choose_creature.png" --preview "$here/renders/shadow" ${TEXTURE:+--texture "$TEXTURE"}
fi
out=${1:-$here/build}
data=${OPENBG_DATA:-$root/source/patched/v2123/Data}
editor=${OPENBG_EDITOR:-$root/engine/build/openbg-editor}
home=$(mktemp -d)
trap 'rm -rf "$home"' EXIT
text="The most secretive project of the last twenty years. Somehow the Shadow throws quasi-real projections of itself, though any damage they take is felt by the host. Hard to fight against, harder to fight with; once mastered, a weapon to be reckoned with."
ghosts=()
for g in GhostPunchLunge GhostPunchUppercut GhostKick GhostGrabLeap GhostGrabHold; do ghosts+=(--import-ghost "$g=$g@0.5"); done
mkdir -p "$out"
rm -rf "$out/Shadow.DEObjects" "$out/Shadow.assets"
HOME=$home "$editor" --data "$data" "$out/Shadow.DEObjects" \
  --import "$here/source/shadow.glb" --import-kind robot --import-base Katana --import-name Shadow "${ghosts[@]}" \
  --import-paint "Stealth=$here/source/ShadowDark.png" --har-title Shadow --har-text "$text" --har-pilot Dawn \
  --har-picture "$here/source/choose_creature.png" --har-script "$here/script/shadow.bgs" --har-help "$here/help.txt" --compile --save --batch --frames 60
echo "built $out/Shadow.DEObjects"
