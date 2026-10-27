// Ghidra headless post-script: dump a module's functions (decompiled C), exports,
// imports, RTTI classes and strings into <outdir>/<module>/ for rewrite reference.
// @category OMFBG
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

import java.io.*;
import java.nio.file.*;
import java.util.*;

public class ExportModule extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		Path out = Paths.get(args[0], currentProgram.getName().replace(".ModuleDLL", "").replace(".exe", ""));
		Files.createDirectories(out.resolve("functions"));

		SymbolTable st = currentProgram.getSymbolTable();
		try (PrintWriter w = new PrintWriter(out.resolve("exports.tsv").toFile())) {
			AddressIterator eps = st.getExternalEntryPointIterator();
			while (eps.hasNext()) {
				Address a = eps.next();
				Symbol s = st.getPrimarySymbol(a);
				w.println(a + "\t" + (s != null ? s.getName(true) : "?"));
			}
		}
		try (PrintWriter w = new PrintWriter(out.resolve("imports.tsv").toFile())) {
			for (Symbol s : st.getExternalSymbols()) {
				ExternalLocation loc = currentProgram.getExternalManager().getExternalLocation(s);
				w.println((loc != null ? loc.getLibraryName() : "?") + "\t" + s.getName());
			}
		}
		try (PrintWriter w = new PrintWriter(out.resolve("classes.txt").toFile())) {
			SortedSet<String> names = new TreeSet<>();
			Iterator<GhidraClass> cls = st.getClassNamespaces();
			while (cls.hasNext()) {
				names.add(cls.next().getName(true));
			}
			names.forEach(w::println);
		}
		try (PrintWriter w = new PrintWriter(out.resolve("strings.tsv").toFile())) {
			for (Data d : currentProgram.getListing().getDefinedData(true)) {
				if (!StringDataInstance.isString(d)) {
					continue;
				}
				StringDataInstance sdi = StringDataInstance.getStringDataInstance(d);
				String v = sdi.getStringValue();
				if (v != null) {
					w.println(d.getAddress() + "\t" + v.replace("\n", "\\n").replace("\t", "\\t"));
				}
			}
		}

		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
		try (PrintWriter idx = new PrintWriter(out.resolve("functions.tsv").toFile())) {
			while (it.hasNext() && !monitor.isCancelled()) {
				Function f = it.next();
				if (f.isThunk() || f.isExternal()) {
					continue;
				}
				String name = f.getName(true);
				idx.println(f.getEntryPoint() + "\t" + f.getBody().getNumAddresses() + "\t" + name);
				DecompileResults r = di.decompileFunction(f, 60, monitor);
				String c = r.decompileCompleted() ? r.getDecompiledFunction().getC() : "/* decompile failed: " + r.getErrorMessage() + " */\n";
				Files.writeString(out.resolve("functions").resolve(f.getEntryPoint() + ".c"), "// " + name + "\n" + c);
			}
		}
		di.dispose();
		println("Exported " + currentProgram.getName() + " -> " + out);
	}
}
