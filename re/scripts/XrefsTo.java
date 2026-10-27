// Print references (and containing functions) to each address passed as script args.
// @category OMFBG
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class XrefsTo extends GhidraScript {
	@Override
	protected void run() throws Exception {
		for (String a : getScriptArgs()) {
			Address target = toAddr(a);
			for (Reference r : getReferencesTo(target)) {
				Function f = getFunctionContaining(r.getFromAddress());
				println("XREF " + a + " <- " + r.getFromAddress() + " " + r.getReferenceType() + " in " + (f != null ? f.getEntryPoint() + " " + f.getName(true) : "?"));
			}
		}
	}
}
