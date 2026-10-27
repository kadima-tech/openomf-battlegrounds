// List every call in the function containing each address: "<from> -> <target> <all symbol names>".
// @category OMFBG
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class CallsIn extends GhidraScript {
	@Override
	protected void run() throws Exception {
		for (String a : getScriptArgs()) {
			Function f = getFunctionContaining(toAddr(a));
			InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
			while (it.hasNext()) {
				Instruction in = it.next();
				if (!in.getFlowType().isCall()) {
					continue;
				}
				for (Reference r : in.getReferencesFrom()) {
					StringBuilder names = new StringBuilder();
					for (Symbol s : currentProgram.getSymbolTable().getSymbols(r.getToAddress())) {
						names.append(' ').append(s.getName());
					}
					println("CALL " + in.getAddress() + " -> " + r.getToAddress() + names);
				}
			}
		}
	}
}
