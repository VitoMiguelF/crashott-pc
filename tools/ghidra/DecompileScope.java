// Decompiles every function of one or more classes/namespaces into <out>/<Scope>.c, in address
// order, each with its address; the decompiler prints the plate comment ApplySymbols.java left
// (source, confidence, note). Run on the project tools/image_to_elf.py's ELF was imported into,
// after ApplySymbols.java:
//   analyzeHeadless private/ghidra CrashOTT -process image.elf -noanalysis \
//       -scriptPath tools/ghidra -postScript DecompileScope.java private/decompiled CFrontendManager [...]
// A scope matches the function's full name up to "::" (CFrontendManager matches
// CFrontendManager::Update and CFrontendManager::CSaveGameHandler::LoadOptions). The output is
// derived from the game: keep it in private/.
// @category CrashRecomp
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class DecompileScope extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("usage: DecompileScope.java <out dir> <scope> [scope...]");
            return;
        }
        File outDir = new File(args[0]);
        outDir.mkdirs();

        DecompInterface decompiler = new DecompInterface();
        decompiler.setOptions(new DecompileOptions());
        decompiler.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            String scope = args[i];
            List<Function> functions = new ArrayList<>();
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.getName(true).startsWith(scope + "::")) {
                    functions.add(f);
                }
            }
            File out = new File(outDir, scope.replace("::", "_").replaceAll("[<>,* ]", "_") + ".c");
            int failed = 0;
            try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
                w.println("// " + scope + ": " + functions.size() + " functions, decompiled by Ghidra from the game image");
                w.println("// (derived from the game: do not publish). Names: private/symbols/functions.csv.");
                w.println();
                for (Function f : functions) {
                    monitor.checkCancelled();
                    w.println("// ---------------------------------------------------------------------------------");
                    w.println("// " + f.getName(true) + " @ " + f.getEntryPoint());
                    DecompileResults r = decompiler.decompileFunction(f, 120, monitor);
                    if (r.decompileCompleted()) {
                        w.println(r.getDecompiledFunction().getC());
                    } else {
                        w.println("// decompile failed: " + r.getErrorMessage());
                        w.println();
                        failed++;
                    }
                }
            }
            println(scope + ": " + functions.size() + " functions (" + failed + " failed) -> " + out);
        }
        decompiler.dispose();
    }
}
