#!/bin/sh
# Rebuild source/patched/v<version>/ from the disc install and the official patches (PATCHES.md).
# usage: rtpatch_chain.sh <work dir>   (needs ~6 GB there; python with unicorn + pefile as $PYTHON)
# Trees are named by the build number compiled into the modules: the patches labelled
# v2097->v2100, v2101->v2111 and v2112->v2121 produce 2.101, 2.112 and 2.123, so the
# 2111->2112 and 2121->2123 patches find nothing to do; no 2.100 / 2.111 / 2.121 tree exists.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../source"
work=${1:?work dir}
py=${PYTHON:-python3}
dll="$src/extracted/App_Executables/AutoPatch/patchw32.dll"
mkdir -p "$work" "$src/patched"
prev="$src/extracted/App_Executables"
step() {
	to=$1; exe=$2
	rm -rf "$work/v$to"
	mkdir -p "$work/v$to"
	cp -R "$prev" "$work/v$to/game"
	"$py" -I "$here/rtpatch_emu.py" "$dll" "$src/patches/$exe" "$work/v$to/game" -NOBACKUP -NOUNDO -NOCONFIRM \
		> "$work/v$to/patch.log" 2>&1
	if grep -qE 'Old Files Invalid\.+ \( *[1-9]|error ept' "$work/v$to/patch.log"; then
		echo "v$to: see $work/v$to/patch.log"; exit 1
	fi
	prev="$work/v$to/game"
}
publish() {
	rm -rf "$src/patched/v$1"
	cp -R "$work/v$1/game" "$src/patched/v$1"
	echo "v$1"
}
step 2095 omf-battlegrounds-patch-v2090-to-v2095.exe; publish 2095
step 2096 omf-battlegrounds-v2095-v2096.exe; publish 2096
step 2097 omf-battlegrounds-v2096-v2097.exe; publish 2097
step 2101 one-must-fall-battlegrounds-v2097-to-v2100-patch.exe; publish 2101
step 2112 one-must-fall-v2101-to-v2111-patch.exe
step 2112b one-must-fall-v2111-to-v2112-patch.exe
diff -rq "$work/v2112/game" "$work/v2112b/game"
publish 2112
step 2123 one-must-fall-battlegrounds-v2112-to-v2121-patch.exe
step 2123b one-must-fall-battlegrounds-v2121-to-v2123-patch.exe
diff -rq "$work/v2123/game" "$work/v2123b/game"
publish 2123
