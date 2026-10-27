// Pre-script: Ghidra's discovered-non-returning heuristic wrongly flags DE_CDataBlock ctor,
// operator delete, _gAllocateMemoryDebug etc. in Core, truncating thousands of functions.
// @category OMFBG
import ghidra.app.script.GhidraScript;

public class NoReturnOff extends GhidraScript {
	@Override
	protected void run() throws Exception {
		setAnalysisOption(currentProgram, "Non-Returning Functions - Discovered", "false");
	}
}
