// Prints the decompiled C of the functions containing the given addresses, from the project that
// tools/image_to_elf.py's ELF was imported into (see docs/internals/linux_port.md):
//   analyzeHeadless private/ghidra CrashOTT -process image.elf -noanalysis -readOnly \
//       -scriptPath tools/ghidra -postScript DecompileAt.java 0x822765C0 [...]
// @category CrashRecomp
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileAt extends GhidraScript {
    @Override
    protected void run() throws Exception {
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        for (String arg : getScriptArgs()) {
            Address address = toAddr(arg);
            Function function = getFunctionContaining(address);
            if (function == null) {
                println("no function at " + arg);
                continue;
            }
            DecompileResults results = decompiler.decompileFunction(function, 60, monitor);
            if (!results.decompileCompleted()) {
                println(function.getName() + ": " + results.getErrorMessage());
                continue;
            }
            println("// " + function.getName() + " @ " + function.getEntryPoint());
            println(results.getDecompiledFunction().getC());
        }
        decompiler.dispose();
    }
}
