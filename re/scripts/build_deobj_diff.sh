#!/bin/sh
# build_deobj_diff.sh <out dir>: builds deobj-diff from the engine's reader sources.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../engine/src"
out=${1:?out dir}
mkdir -p "$out"
c++ -std=c++20 -O2 -I"$src" -o "$out/deobj-diff" "$here/deobj_diff.cpp" \
	"$src/de/contents.cpp" "$src/de/lzrw3a.cpp" "$src/de/md5.cpp" "$src/de/objects_file.cpp" "$src/de/pcode.cpp" \
	"$src/vm/vmcode.cpp" "$src/vm/disasm.cpp"
echo "$out/deobj-diff"
