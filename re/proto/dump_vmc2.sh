#!/bin/sh
# Write every Class's VMC2 payload to re/vmc2/<file>/<class>.bin
cd "$(dirname "$0")/../.."
D=source/extracted/App_Executables/Data
for f in "$D"/*.DEObjects; do
	n=$(basename "$f" .DEObjects)
	engine/build/deobj-dump toc "$f" 2>/dev/null | tail -n +2 | awk -F'\t' '$2=="Class"{print $1}' | while read -r cls; do
		mkdir -p "re/vmc2/$n"
		engine/build/deobj-dump ex "$f" "$cls" VMC2 "re/vmc2/$n/$cls.bin" >/dev/null 2>&1 || rm -f "re/vmc2/$n/$cls.bin"
		[ -f "re/vmc2/$n/$cls.bin" ] && engine/build/deobj-dump block "$f" "$cls" references "re/vmc2/$n/$cls.refs"
	done
done
find re/vmc2 -name '*.bin' | wc -l
