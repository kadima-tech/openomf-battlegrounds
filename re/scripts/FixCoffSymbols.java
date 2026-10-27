// Pre-script: Ghidra places debug COFF symbols at sectionVA + value, but in the DE images
// the value is already an RVA, so every label lands 0x1000 (.text) or more past its target.
// Moves each label (and entry point) from the wrong address to the true one using
// re/coff/<module>.tsv produced by re/proto/coffsyms.py.
// @category OMFBG
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.*;

import java.nio.file.*;
import java.util.List;

public class FixCoffSymbols extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String module = currentProgram.getName().replace(".ModuleDLL", "");
		Path tsv = Paths.get(getScriptArgs()[0], module + ".tsv");
		SymbolTable st = currentProgram.getSymbolTable();
		int moved = 0;
		List<String> rows = Files.readAllLines(tsv);
		// Pass 1: drop every misplaced label and entry point first, so a true address that
		// collides with another symbol's wrong address is not removed in pass 2.
		for (String row : rows) {
			String[] f = row.split("\t", 4);
			Address wrong = toAddr(Long.parseLong(f[0], 16));
			for (Symbol s : st.getSymbols(wrong)) {
				if (s.getName().equals(f[3]) && s.getSource() == SourceType.IMPORTED) {
					s.delete();
				}
			}
			if (f[2].equals("1") && st.isExternalEntryPoint(wrong)) {
				st.removeExternalEntryPoint(wrong);
			}
		}
		for (String row : rows) {
			String[] f = row.split("\t", 4);
			Address right = toAddr(Long.parseLong(f[1], 16));
			if (currentProgram.getMemory().contains(right)) {
				try {
					st.createLabel(right, f[3], SourceType.IMPORTED);
				} catch (ghidra.util.exception.InvalidInputException e) {
					continue;  // e.g. "\x7fCore_NULL_THUNK_DATA" import-descriptor names
				}
				if (f[2].equals("1")) {
					st.addExternalEntryPoint(right);
				}
				moved++;
			}
		}
		println("FixCoffSymbols: relocated " + moved + " of " + rows.size() + " COFF symbols in " + module);
	}
}
