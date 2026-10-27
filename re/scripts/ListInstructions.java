// Print all instructions of the function containing each address.
// @category OMFBG
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;

public class ListInstructions extends GhidraScript {
	@Override
	protected void run() throws Exception {
		for (String a : getScriptArgs()) {
			Function f = getFunctionContaining(toAddr(a));
			InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
			while (it.hasNext()) {
				Instruction in = it.next();
				println("INS " + in.getAddress() + " " + in);
			}
		}
	}
}
