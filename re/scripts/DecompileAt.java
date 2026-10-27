// Disassemble + create function containing each address (if missing), print decompiled C.
// @category OMFBG
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileAt extends GhidraScript {
	@Override
	protected void run() throws Exception {
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		for (String a : getScriptArgs()) {
			Address addr = toAddr(a);
			Function f = getFunctionContaining(addr);
			if (f == null) {
				// Walk back to a likely prologue (push ebp / int3 / ret padding) and start there.
				Address start = addr;
				for (int i = 0; i < 0x400; i++) {
					Address p = addr.subtract(i);
					byte b = getByte(p);
					byte prev = getByte(p.subtract(1));
					if ((prev == (byte) 0xCC || prev == (byte) 0x90 || prev == (byte) 0xC3) && (b == (byte) 0x55 || b == (byte) 0x6A || b == (byte) 0x83 || b == (byte) 0x64 || b == (byte) 0x56 || b == (byte) 0x51 || b == (byte) 0x81)) {
						start = p;
						break;
					}
				}
				new DisassembleCommand(start, null, true).applyTo(currentProgram, monitor);
				f = createFunction(start, null);
				println("CREATED " + start + " for " + a + " -> " + f);
			}
			if (f == null) {
				continue;
			}
			DecompileResults r = di.decompileFunction(f, 120, monitor);
			println("=====BEGIN " + a + " in " + f.getEntryPoint() + " " + f.getName(true));
			println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "FAILED " + r.getErrorMessage());
			println("=====END");
		}
	}
}
