// For every *Factory::PathAdd / ObjName, print the string it returns (mov eax, imm32 ; ret).
// @category OMFBG
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;

public class FactoryPaths extends GhidraScript {
	@Override
	protected void run() throws Exception {
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			String n = f.getName();
			String ns = f.getParentNamespace().getName();
			if (!ns.endsWith("Factory") || !(n.equals("PathAdd") || n.equals("ObjName") || n.equals("ObjParent"))) {
				continue;
			}
			Instruction in = getInstructionAt(f.getEntryPoint());
			String val = "?";
			for (int i = 0; i < 4 && in != null; i++, in = in.getNext()) {
				if (n.equals("ObjParent") && in.getMnemonicString().equals("MOV") && in.getNumOperands() == 2) {
					// Cross-module parent: mov eax, [__imp_g_FooFactory] (IAT slot).
					ghidra.program.model.symbol.Reference[] refs = in.getOperandReferences(1);
					if (refs.length > 0 && !(in.getOpObjects(1).length == 1 && in.getOpObjects(1)[0] instanceof Scalar)) {
						ghidra.program.model.symbol.Symbol sym = getSymbolAt(refs[0].getToAddress());
						if (sym != null) {
							val = sym.getName(true);
							break;
						}
					}
				}
				if (in.getMnemonicString().equals("MOV") && in.getNumOperands() == 2) {
					Object[] ops = in.getOpObjects(1);
					if (ops.length == 1 && ops[0] instanceof Scalar sc
							&& currentProgram.getMemory().contains(toAddr(sc.getUnsignedValue()))) {
						Data d = getDataAt(toAddr(sc.getUnsignedValue()));
						if (n.equals("ObjParent")) {
							ghidra.program.model.symbol.Symbol sym = getSymbolAt(toAddr(sc.getUnsignedValue()));
							val = sym != null ? sym.getName(true) : String.format("%08x", sc.getUnsignedValue());
						} else if (d != null && d.getValue() instanceof String s) {
							val = s;
						} else {
							byte[] b = getBytes(toAddr(sc.getUnsignedValue()), 64);
							int k = 0;
							while (k < b.length && b[k] != 0) k++;
							val = new String(b, 0, k, "latin1");
						}
						break;
					}
				}
			}
			println("FP\t" + ns + "\t" + n + "\t" + val);
		}
	}
}
