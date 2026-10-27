#!/bin/sh
# Run a read-only Ghidra script against an analysed module: run.sh <Module> <Script.java> [args...]
RE="$(cd "$(dirname "$0")/.." && pwd)"
G="${GHIDRA_INSTALL_DIR:?set GHIDRA_INSTALL_DIR to your Ghidra install}"
J="${GHIDRA_JDK:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}"
m=$1; s=$2; shift 2
PATH=$J/bin:$PATH JAVA_HOME=$J "$G/support/analyzeHeadless" "$RE/ghidra" OMFBG -process "$m.ModuleDLL" -noanalysis -readOnly \
	-scriptPath "$RE/scripts" -postScript "$s" "$@" 2>&1 | sed -n 's/^INFO  [A-Za-z]*\.java> //p' | sed 's/ (GhidraScript)  $//'
