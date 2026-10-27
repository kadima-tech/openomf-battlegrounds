// Print every symbol at each address with its source and primary flag.
// @category OMFBG
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.Symbol;

public class SymbolsAt extends GhidraScript {
	@Override
	protected void run() throws Exception {
		for (String a : getScriptArgs()) {
			for (Symbol s : currentProgram.getSymbolTable().getSymbols(toAddr(a))) {
				println("SYM " + a + " " + s.getName(true) + " src=" + s.getSource() + " primary=" + s.isPrimary());
			}
		}
	}
}
