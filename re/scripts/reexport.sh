#!/bin/sh
# Re-import modules with non-returning discovery disabled, then re-export into re/dump.
RE="$(cd "$(dirname "$0")/.." && pwd)"
G="${GHIDRA_INSTALL_DIR:?set GHIDRA_INSTALL_DIR to your Ghidra install}"
J="${GHIDRA_JDK:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}"
E="$RE/../source/extracted/App_Executables/Engine"
for m in "$@"; do
	rm -rf "$RE/dump/$m"
	rm -rf "$RE/ghidra/OMFBG.rep/"*/"$m"* 2>/dev/null
	PATH=$J/bin:$PATH JAVA_HOME=$J "$G/support/analyzeHeadless" "$RE/ghidra" OMFBG -import "$E/$m.ModuleDLL" -overwrite -max-cpu 10 -analysisTimeoutPerFile 7200 \
		-scriptPath "$RE/scripts" -preScript NoReturnOff.java -preScript FixCoffSymbols.java "$RE/coff" -postScript ExportModule.java "$RE/dump" > "$RE/ghidra/$m.log" 2>&1
	(cd "$RE/dump/$m/functions" && ls | sort | xargs cat > ../all.c)
	echo "$m: $(wc -l < "$RE/dump/$m/functions.tsv") functions, $(grep -c 'Subroutine does not return' "$RE/dump/$m/all.c") noreturn warnings, $(grep -o "relocated [0-9]* of [0-9]*" "$RE/ghidra/$m.log")"
done
