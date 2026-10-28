// Makes the decompiled C readable after ApplySymbols.java:
//   - splits the image into its PE sections with their permissions, so constants in the
//     read-only .rdata show as values (0.0) instead of DAT_82000dc8;
//   - gives every RTTI class a structure: its size from classes.csv, a vftable pointer at 0 and
//     the named fields of fields.csv (its own and those of its bases, at their offsets);
//   - makes the methods __thiscall, so r3 becomes `ClassName *this` and field accesses read
//     this->m_LedgeSpeed instead of *(float *)(param_1 + 0x4e0);
//   - with "fill", lets the decompiler add the fields the methods use (field_0x40, typed).
// fields.csv, classes.csv and rtti.json come from tools/rtti_scan.py, next to functions.csv:
//   analyzeHeadless private/ghidra CrashOTT -process image.elf -noanalysis \
//       -scriptPath tools/ghidra -postScript ClassLayouts.java private/symbols [fill]
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

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.util.FillOutStructureHelper;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.ArrayDataType;
import ghidra.program.model.data.BooleanDataType;
import ghidra.program.model.data.ByteDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeComponent;
import ghidra.program.model.data.DataTypeConflictHandler;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.DoubleDataType;
import ghidra.program.model.data.FloatDataType;
import ghidra.program.model.data.IntegerDataType;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.ShortDataType;
import ghidra.program.model.data.Structure;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.data.UnsignedIntegerDataType;
import ghidra.program.model.data.UnsignedShortDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.GhidraClass;
import ghidra.program.model.listing.VariableUtilities;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.pcode.HighVariable;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;

public class ClassLayouts extends GhidraScript {
    // Sources whose functions are methods (r3 = this). Excluded: rtti-shared (nullsub,
    // return_0...: one body for unrelated classes) and callers (could be static helpers).
    private static final Set<String> METHOD_SOURCES = Set.of("rtti", "rtti-inherited", "rtti-dtor", "heuristic",
        "ctor", "manual", "script-binding", "thunk");

    private DataTypeManager dtm;
    private final Map<String, Structure> structs = new HashMap<>();

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

    static List<Map<String, String>> readCsv(File file) throws Exception {
        List<Map<String, String>> rows = new ArrayList<>();
        if (!file.isFile()) return rows;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            List<String> header = csvFields(r.readLine());
            String line;
            while ((line = r.readLine()) != null) {
                if (line.isEmpty()) continue;
                List<String> f = csvFields(line);
                Map<String, String> row = new HashMap<>();
                for (int i = 0; i < header.size() && i < f.size(); i++) row.put(header.get(i), f.get(i));
                rows.add(row);
            }
        }
        return rows;
    }

    private DataType typeFor(String name) {
        name = name.trim();
        if (name.endsWith("*")) return new PointerDataType(typeFor(name.substring(0, name.length() - 1)), 4, dtm);
        Structure cls = name.contains("::") || name.matches("C[A-Z]\\w*") ? structFor(name) : null;
        if (cls != null) return cls;
        switch (name) {
            case "float": return FloatDataType.dataType;
            case "double": return DoubleDataType.dataType;
            case "bool": return BooleanDataType.dataType;
            case "byte": return ByteDataType.dataType;
            case "short": return ShortDataType.dataType;
            case "ushort": return UnsignedShortDataType.dataType;
            case "int": return IntegerDataType.dataType;
            case "uint": return UnsignedIntegerDataType.dataType;
            default: return Undefined4DataType.dataType;
        }
    }

    /** PE sections of the image (its header is at the image base): split blocks, set permissions. */
    private int applySections() throws Exception {
        Memory mem = currentProgram.getMemory();
        Address base = toAddr(0x82000000L);
        int pe = mem.getInt(base.add(0x3C), false);
        int count = mem.getShort(base.add(pe + 6), false) & 0xFFFF;
        int optSize = mem.getShort(base.add(pe + 20), false) & 0xFFFF;
        int done = 0;
        for (int i = 0; i < count; i++) {
            Address sec = base.add(pe + 24 + optSize + 40 * i);
            byte[] raw = new byte[8];
            mem.getBytes(sec, raw);
            String name = new String(raw, StandardCharsets.US_ASCII).replace("\0", "");
            long vsize = mem.getInt(sec.add(8), false) & 0xFFFFFFFFL;
            long va = mem.getInt(sec.add(12), false) & 0xFFFFFFFFL;
            int flags = mem.getInt(sec.add(36), false);
            Address start = base.add(va);
            Address end = start.add(vsize);
            MemoryBlock block = mem.getBlock(start);
            if (block == null) continue;
            if (!block.getStart().equals(start)) {
                mem.split(block, start);
                block = mem.getBlock(start);
            }
            if (block.getEnd().compareTo(end) >= 0 && mem.getBlock(end) == block) {
                mem.split(block, end);
                block = mem.getBlock(start);
            }
            block.setName(name.isEmpty() ? "sec" + i : name);
            block.setRead(true);
            block.setWrite((flags & 0x80000000) != 0);
            block.setExecute((flags & 0x20000000) != 0);
            done++;
        }
        return done;
    }

    private Structure structFor(String className) {
        if (structs.containsKey(className)) return structs.get(className);
        Structure s = null;
        Namespace ns = null;
        try {
            ns = ghidra.app.util.NamespaceUtils.getNamespaceByPath(currentProgram, null, className.replace(", ", ","))
                .stream().findFirst().orElse(null);
        } catch (Exception e) {
            ns = null;
        }
        if (ns instanceof GhidraClass) {
            // The class structure the decompiler gives `this` (a new one is not in the program
            // yet: resolve() adds it, or returns the one already there).
            s = (Structure) dtm.resolve(VariableUtilities.findOrCreateClassStruct((GhidraClass) ns, dtm),
                DataTypeConflictHandler.KEEP_HANDLER);
        }
        structs.put(className, s);
        return s;
    }

    private void setField(Structure s, int offset, DataType dt, String name, String comment) {
        if (offset < 0) return;
        int need = offset + dt.getLength();
        if (s.isNotYetDefined() || s.getLength() < need) {
            s.growStructure(need - (s.isNotYetDefined() ? 0 : s.getLength()));
        }
        DataTypeComponent existing = s.getComponentContaining(offset);
        if (existing != null && existing.getOffset() == offset && existing.getFieldName() != null
                && !existing.getFieldName().matches("field\\d*_0x[0-9a-f]+")) {
            return;   // already named: the class's own rows come before its bases'; names from fill give way
        }
        try {
            s.replaceAtOffset(offset, dt, dt.getLength(), name, comment);
        } catch (Exception e) {
            printerr(s.getName() + "+0x" + Integer.toHexString(offset) + " " + name + ": " + e.getMessage());
        }
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File dir = new File(args.length > 0 ? args[0] : askDirectory("private/symbols", "Use").getAbsolutePath());
        boolean fill = args.length > 1 && args[1].equals("fill");
        dtm = currentProgram.getDataTypeManager();

        int sections = applySections();

        JsonObject rtti;
        try (InputStreamReader r = new InputStreamReader(new FileInputStream(new File(dir, "rtti.json")), StandardCharsets.UTF_8)) {
            rtti = JsonParser.parseReader(r).getAsJsonObject();
        }
        Map<String, List<String[]>> bases = new HashMap<>();   // class -> [base name, mdisp]
        Set<String> withVtable = new HashSet<>();
        for (JsonElement ce : rtti.getAsJsonArray("classes")) {
            JsonObject c = ce.getAsJsonObject();
            String name = c.get("name").getAsString();
            List<String[]> list = new ArrayList<>();
            for (JsonElement be : c.getAsJsonArray("bases")) {
                JsonObject b = be.getAsJsonObject();
                if (b.get("pdisp").getAsInt() == -1) list.add(new String[] { b.get("name").getAsString(), b.get("mdisp").getAsString() });
            }
            bases.put(name, list);
            if (c.getAsJsonArray("vtables").size() > 0) withVtable.add(name);
        }

        // Sizes, vftable, named fields (each class gets its bases' fields too).
        Map<String, List<Map<String, String>>> fieldsByClass = new HashMap<>();
        for (Map<String, String> row : readCsv(new File(dir, "fields.csv"))) {
            fieldsByClass.computeIfAbsent(row.get("class"), k -> new ArrayList<>()).add(row);
        }
        Map<String, Integer> sizes = new HashMap<>();
        for (Map<String, String> row : readCsv(new File(dir, "classes.csv"))) {
            sizes.put(row.get("class"), Integer.decode(row.get("size")));
        }
        int classes = 0, fields = 0;
        for (String cls : bases.keySet()) {
            monitor.checkCancelled();
            Structure s = structFor(cls);
            if (s == null) continue;
            classes++;
            Integer size = sizes.get(cls);
            if (size != null && (s.isNotYetDefined() || s.getLength() < size)) {
                s.growStructure(size - (s.isNotYetDefined() ? 0 : s.getLength()));
            }
            if (withVtable.contains(cls)) setField(s, 0, new PointerDataType(Undefined4DataType.dataType, 4, dtm), "vftable", null);
            List<String[]> chain = new ArrayList<>();
            chain.add(new String[] { cls, "0" });
            chain.addAll(bases.get(cls));
            for (String[] b : chain) {
                List<Map<String, String>> rows = fieldsByClass.get(b[0]);
                if (rows == null) continue;
                int disp = Integer.parseInt(b[1]);
                for (Map<String, String> row : rows) {
                    String comment = row.get("source") + ", " + row.get("confidence") + (b[0].equals(cls) ? "" : ", from " + b[0])
                        + (row.get("note").isEmpty() ? "" : ": " + row.get("note"));
                    DataType dt = typeFor(row.get("type"));
                    int span = row.get("size") == null || row.get("size").isEmpty() ? dt.getLength() : Integer.decode(row.get("size"));
                    if (span > dt.getLength() && dt.getLength() > 0 && span % dt.getLength() == 0) {
                        dt = new ArrayDataType(dt, span / dt.getLength(), dt.getLength(), dtm);   // float[16] for a 4x4 matrix
                    }
                    setField(s, disp + Integer.decode(row.get("offset")), dt, row.get("name"), comment);
                    fields++;
                }
            }
        }

        // Methods: __thiscall.
        List<Function> methods = new ArrayList<>();
        for (Map<String, String> row : readCsv(new File(dir, "functions.csv"))) {
            if (!METHOD_SOURCES.contains(row.get("source"))) continue;
            Function f = getFunctionAt(toAddr(Long.decode(row.get("address"))));
            if (f == null || !(f.getParentNamespace() instanceof GhidraClass)) continue;
            try {
                f.setCallingConvention("__thiscall");
                methods.add(f);
            } catch (Exception e) {
                printerr(f.getName(true) + ": " + e.getMessage());
            }
        }

        // The decompiler adds the fields each method uses through this.
        int filled = 0;
        if (fill) {
            DecompInterface decomp = new DecompInterface();
            decomp.setOptions(new DecompileOptions());
            decomp.toggleCCode(true);
            decomp.toggleSyntaxTree(true);
            decomp.setSimplificationStyle("decompile");
            decomp.openProgram(currentProgram);
            FillOutStructureHelper helper = new FillOutStructureHelper(currentProgram, monitor);
            for (Function f : methods) {
                monitor.checkCancelled();
                ghidra.program.model.listing.Parameter self = null;
                for (ghidra.program.model.listing.Parameter p : f.getParameters()) {
                    if (p.isAutoParameter() && "this".equals(p.getName())) self = p;
                }
                if (self == null || self.getMinAddress() == null) continue;
                try {
                    HighVariable hv = helper.computeHighVariable(self.getMinAddress(), f, decomp);
                    if (hv == null) continue;
                    Structure s = helper.processStructure(hv, f, false, false, decomp);
                    if (s != null) filled++;
                } catch (Exception e) {
                    // functions the decompiler cannot handle keep the structure as it is
                }
            }
            decomp.dispose();
        }
        println("CrashRecomp class layouts: " + sections + " sections, " + classes + " class structures, " + fields
            + " named fields, " + methods.size() + " methods __thiscall" + (fill ? ", " + filled + " filled from use" : ""));
    }
}
