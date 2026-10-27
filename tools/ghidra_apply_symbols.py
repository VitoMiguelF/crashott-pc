# Apply the names recovered by tools/rtti_scan.py to a Crash of the Titans XEX opened in Ghidra.
#
# Load default.xex with XEXLoaderWV (addresses match the recompiler: base 0x82000000), let the
# auto-analysis finish, then run this script from the Script Manager (Jython, or PyGhidra on
# Ghidra 11+) and pick private/symbols/functions.csv. rtti.json next to it is used too, for
# vtable/RTTI labels and class namespaces.
#
# Headless: analyzeHeadless <proj> <name> -process default.xex -postScript ghidra_apply_symbols.py <path/to/functions.csv>
#
# What it does:
#   - functions.csv: creates the function if Ghidra has none there, names it, puts it in the
#     namespace of its class (CLifeDisplay::vf03 -> class CLifeDisplay, function vf03) and adds a
#     plate comment with the source (rtti*, heuristic, import, manual or one of the propagation
#     passes of tools/name_functions.py), its confidence and the note; a scope that is an RTTI
#     class becomes a Ghidra class, other scopes (frontend::, d3d::, static_init::) namespaces;
#   - rtti.json: labels each vtable (Class::vftable, Class::vftable{for_Base}), its
#     CompleteObjectLocator and the TypeDescriptor, and turns the namespaces into classes.
# Names already set by hand (SourceType.USER_DEFINED) are kept.
#
# @category CrashRecomp
# @runtime Jython
import csv
import json
import os

from ghidra.app.util import NamespaceUtils
from ghidra.program.model.listing import CodeUnit
from ghidra.program.model.symbol import SourceType


def split_scope(name):
    """'a::b<c::d>::e' -> ['a', 'b<c::d>', 'e'] (ignores :: inside template arguments)."""
    parts, depth, start, i = [], 0, 0, 0
    while i < len(name):
        c = name[i]
        if c in '<(':
            depth += 1
        elif c in '>)':
            depth -= 1
        elif depth == 0 and name.startswith('::', i):
            parts.append(name[start:i])
            start = i + 2
            i += 1
        i += 1
    parts.append(name[start:])
    return parts


def ghidra_name(part):
    # Ghidra symbol names cannot contain spaces (its own demangler writes `scalar_deleting_destructor').
    return part.replace(', ', ',').replace(' ', '_')


namespace_cache = {}


def namespace_for(parts, make_class):
    key = tuple(parts)
    if key in namespace_cache:
        return namespace_cache[key]
    st = currentProgram.getSymbolTable()
    ns = currentProgram.getGlobalNamespace()
    for part in parts:
        part = ghidra_name(part)
        existing = st.getNamespace(part, ns)
        ns = existing if existing is not None else st.createNameSpace(ns, part, SourceType.IMPORTED)
    if make_class:
        try:
            ns = NamespaceUtils.convertNamespaceToClass(ns)
        except Exception:
            pass
    namespace_cache[key] = ns
    return ns


def apply_name(address, full_name, comment, make_class):
    parts = split_scope(full_name)
    ns = namespace_for(parts[:-1], make_class) if len(parts) > 1 else currentProgram.getGlobalNamespace()
    leaf = ghidra_name(parts[-1])
    fm = currentProgram.getFunctionManager()
    func = fm.getFunctionAt(address)
    if func is None:
        disassemble(address)
        func = createFunction(address, None)
    if func is not None:
        if func.getSymbol().getSource() == SourceType.USER_DEFINED:
            return False
        func.setName(leaf, SourceType.IMPORTED)
        func.setParentNamespace(ns)
    else:
        currentProgram.getSymbolTable().createLabel(address, leaf, ns, SourceType.IMPORTED)
    if comment:
        currentProgram.getListing().setComment(address, CodeUnit.PLATE_COMMENT, comment)
    return True


def label(address, full_name):
    parts = split_scope(full_name)
    ns = namespace_for(parts[:-1], True) if len(parts) > 1 else currentProgram.getGlobalNamespace()
    sym = currentProgram.getSymbolTable().createLabel(address, ghidra_name(parts[-1]), ns, SourceType.IMPORTED)
    sym.setPrimary()


def run():
    args = getScriptArgs()
    if args:
        csv_path = args[0]
    else:
        csv_path = askFile('functions.csv (tools/rtti_scan.py)', 'Apply').getAbsolutePath()

    rtti = None
    rtti_path = os.path.join(os.path.dirname(csv_path), 'rtti.json')
    if os.path.exists(rtti_path):
        with open(rtti_path) as fp:
            rtti = json.load(fp)
    # A scope that is an RTTI class becomes a Ghidra class; the others (frontend::, d3d::,
    # static_init::...) stay plain namespaces.
    class_names = set(cls['name'] for cls in rtti['classes']) if rtti else set()

    named = skipped = 0
    with open(csv_path) as fp:
        rows = list(csv.DictReader(fp))
    monitor.initialize(len(rows))
    for row in rows:
        monitor.checkCanceled()
        monitor.incrementProgress(1)
        address = toAddr(int(row['address'], 16))
        tag = row['source'] + (', ' + row['confidence'] if row.get('confidence') else '')
        comment = '%s [%s]%s' % (row['name'], tag, (' ' + row['note']) if row.get('note') else '')
        parts = split_scope(row['name'])
        is_method = len(parts) > 1 and '::'.join(parts[:-1]) in class_names
        if not class_names:
            is_method = row['source'].startswith('rtti') or row['source'] == 'heuristic'
        try:
            if apply_name(address, row['name'], comment, is_method and '::' in row['name']):
                named += 1
            else:
                skipped += 1
        except Exception as e:
            printerr('%s %s: %s' % (row['address'], row['name'], e))

    vtables = 0
    if rtti:
        for cls in rtti['classes']:
            name = cls['name']
            try:
                label(toAddr(int(cls['type_descriptor'], 16)), name + "::`RTTI Type Descriptor'")
                for vt in cls['vtables']:
                    suffix = '' if not vt['subobject'] else '{for_%s}' % vt['subobject']
                    label(toAddr(int(vt['address'], 16)), name + "::vftable" + suffix)
                    label(toAddr(int(vt['col'], 16)), name + "::`RTTI Complete Object Locator'" + suffix)
                    vtables += 1
            except Exception as e:
                printerr('%s: %s' % (name, e))

    print('CrashRecomp symbols: %d functions named, %d kept (user-defined), %d vtables labelled' % (named, skipped, vtables))


run()
