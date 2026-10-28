// Java version of tools/ghidra_apply_symbols.py, for Ghidra 12+ headless (no Jython there):
// names every function of private/symbols/functions.csv, puts it in its class or namespace
// (scopes that are RTTI classes in rtti.json next to the CSV become classes) and adds a plate
// comment "Name [source, confidence] note". With rtti.json it also labels vtables, COLs and
// TypeDescriptors. Names set by hand (USER_DEFINED) are kept. The register save/restore helpers
// (config/Crash.toml) get one void function per entry (__savegprlr_27 ...), so the decompiler
// does not take r3 after the prologue call for their return value.
//   analyzeHeadless private/ghidra CrashOTT -process image.elf -noanalysis \
//       -scriptPath tools/ghidra -postScript ApplySymbols.java private/symbols/functions.csv
// @category CrashRecomp
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.NamespaceUtils;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.VoidDataType;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

public class ApplySymbols extends GhidraScript {
    private final Map<String, Namespace> namespaces = new HashMap<>();
    private Set<String> classNames = new HashSet<>();

    /** 'a::b<c::d>::e' -> [a, b<c::d>, e] (ignores :: inside template arguments and {for_x::y}). */
    static List<String> splitScope(String name) {
        List<String> parts = new ArrayList<>();
        int depth = 0, start = 0;
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (c == '<' || c == '(' || c == '{') depth++;
            else if (c == '>' || c == ')' || c == '}') depth--;
            else if (depth == 0 && name.startsWith("::", i)) {
                parts.add(name.substring(start, i));
                start = i + 2;
                i++;
            }
        }
        parts.add(name.substring(start));
        return parts;
    }

    /** Ghidra symbol names cannot contain spaces. */
    static String ghidraName(String part) {
        return part.replace(", ", ",").replace(' ', '_');
    }

    /** One CSV line (RFC 4180 quoting) into fields. */
    static List<String> csvFields(String line) {
        List<String> fields = new ArrayList<>();
        StringBuilder f = new StringBuilder();
        boolean quoted = false;
        for (int i = 0; i < line.length(); i++) {
            char c = line.charAt(i);
            if (quoted) {
                if (c == '"' && i + 1 < line.length() && line.charAt(i + 1) == '"') { f.append('"'); i++; }
                else if (c == '"') quoted = false;
                else f.append(c);
            } else if (c == '"') quoted = true;
            else if (c == ',') { fields.add(f.toString()); f.setLength(0); }
            else f.append(c);
        }
        fields.add(f.toString());
        return fields;
    }

    private Namespace namespaceFor(List<String> parts) throws Exception {
        String key = String.join("::", parts);
        Namespace cached = namespaces.get(key);
        if (cached != null) return cached;
        SymbolTable st = currentProgram.getSymbolTable();
        Namespace ns = currentProgram.getGlobalNamespace();
        for (String part : parts) {
            String p = ghidraName(part);
            Namespace existing = st.getNamespace(p, ns);
            ns = existing != null ? existing : st.createNameSpace(ns, p, SourceType.IMPORTED);
        }
        if (classNames.contains(key)) {
            try {
                ns = NamespaceUtils.convertNamespaceToClass(ns);
            } catch (Exception e) {
                // already a class, or a library namespace
            }
        }
        namespaces.put(key, ns);
        return ns;
    }

    private void label(Address address, String fullName) throws Exception {
        List<String> parts = splitScope(fullName);
        Namespace ns = parts.size() > 1 ? namespaceFor(parts.subList(0, parts.size() - 1)) : currentProgram.getGlobalNamespace();
        Symbol s = currentProgram.getSymbolTable().createLabel(address, ghidraName(parts.get(parts.size() - 1)), ns, SourceType.IMPORTED);
        s.setPrimary();
    }

    // __savegprlr_14 / __restgprlr_14 / __savefpr_14 / __restfpr_14 (config/Crash.toml): entry N
    // is 4 bytes per register further, down to register 31.
    private static final String[] HELPERS = { "__savegprlr_", "__restgprlr_", "__savefpr_", "__restfpr_" };
    private static final long[] HELPER_BASES = { 0x823CEBE0L, 0x823CEC30L, 0x823CFED0L, 0x823CFF1CL };

    private int applyRegisterHelpers() throws Exception {
        int count = 0;
        for (int h = 0; h < HELPERS.length; h++) {
            for (int n = 14; n <= 31; n++) {
                Address a = toAddr(HELPER_BASES[h] + 4 * (n - 14));
                Function func = getFunctionAt(a);
                if (func == null) {
                    disassemble(a);
                    func = createFunction(a, null);
                }
                if (func == null || func.getSymbol().getSource() == SourceType.USER_DEFINED) continue;
                func.setName(HELPERS[h] + n, SourceType.IMPORTED);
                func.setParentNamespace(currentProgram.getGlobalNamespace());
                func.updateFunction(null, null, FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true,
                    SourceType.IMPORTED, new Parameter[0]);
                func.setReturnType(VoidDataType.dataType, SourceType.IMPORTED);
                count++;
            }
        }
        return count;
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File csv = args.length > 0 ? new File(args[0]) : askFile("functions.csv (tools/rtti_scan.py)", "Apply");
        JsonObject rtti = null;
        File rttiFile = new File(csv.getParentFile(), "rtti.json");
        if (rttiFile.isFile()) {
            try (InputStreamReader r = new InputStreamReader(new FileInputStream(rttiFile), StandardCharsets.UTF_8)) {
                rtti = JsonParser.parseReader(r).getAsJsonObject();
            }
            for (JsonElement c : rtti.getAsJsonArray("classes")) {
                classNames.add(c.getAsJsonObject().get("name").getAsString());
            }
        }

        int named = 0, kept = 0, failed = 0;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(csv), StandardCharsets.UTF_8))) {
            List<String> header = csvFields(r.readLine());
            int iAddr = header.indexOf("address"), iName = header.indexOf("name"), iSource = header.indexOf("source");
            int iConf = header.indexOf("confidence"), iNote = header.indexOf("note");
            String line;
            while ((line = r.readLine()) != null) {
                monitor.checkCancelled();
                if (line.isEmpty()) continue;
                List<String> f = csvFields(line);
                String name = f.get(iName);
                Address address = toAddr(Long.parseLong(f.get(iAddr).substring(2), 16));
                String tag = f.get(iSource) + (iConf >= 0 && !f.get(iConf).isEmpty() ? ", " + f.get(iConf) : "");
                String note = iNote >= 0 && iNote < f.size() ? f.get(iNote) : "";
                try {
                    Function func = getFunctionAt(address);
                    if (func == null) {
                        disassemble(address);
                        func = createFunction(address, null);
                    }
                    List<String> parts = splitScope(name);
                    Namespace ns = parts.size() > 1 ? namespaceFor(parts.subList(0, parts.size() - 1)) : currentProgram.getGlobalNamespace();
                    String leaf = ghidraName(parts.get(parts.size() - 1));
                    if (func != null) {
                        if (func.getSymbol().getSource() == SourceType.USER_DEFINED) { kept++; continue; }
                        func.setName(leaf, SourceType.IMPORTED);
                        func.setParentNamespace(ns);
                    } else {
                        currentProgram.getSymbolTable().createLabel(address, leaf, ns, SourceType.IMPORTED);
                    }
                    currentProgram.getListing().setComment(address, CodeUnit.PLATE_COMMENT,
                        name + " [" + tag + "]" + (note.isEmpty() ? "" : " " + note));
                    named++;
                } catch (Exception e) {
                    failed++;
                    if (failed <= 20) printerr(f.get(iAddr) + " " + name + ": " + e.getMessage());
                }
            }
        }

        int helpers = applyRegisterHelpers();

        int vtables = 0;
        if (rtti != null) {
            for (JsonElement ce : rtti.getAsJsonArray("classes")) {
                JsonObject c = ce.getAsJsonObject();
                String name = c.get("name").getAsString();
                try {
                    label(toAddr(Long.parseLong(c.get("type_descriptor").getAsString().substring(2), 16)), name + "::RTTI_Type_Descriptor");
                    JsonArray vts = c.getAsJsonArray("vtables");
                    for (JsonElement ve : vts) {
                        JsonObject vt = ve.getAsJsonObject();
                        String sub = !vt.has("subobject") || vt.get("subobject").isJsonNull() ? "" : vt.get("subobject").getAsString();
                        String suffix = sub.isEmpty() ? "" : "{for_" + sub + "}";
                        label(toAddr(Long.parseLong(vt.get("address").getAsString().substring(2), 16)), name + "::vftable" + suffix);
                        label(toAddr(Long.parseLong(vt.get("col").getAsString().substring(2), 16)), name + "::RTTI_Complete_Object_Locator" + suffix);
                        vtables++;
                    }
                } catch (Exception e) {
                    printerr(name + ": " + e.getMessage());
                }
            }
        }
        println("CrashRecomp symbols: " + named + " functions named, " + kept + " kept (user-defined), "
            + failed + " failed, " + vtables + " vtables labelled, " + helpers + " register helpers");
    }
}
