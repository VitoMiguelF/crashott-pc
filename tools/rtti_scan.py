#!/usr/bin/env python3
"""Recover class names, inheritance and virtual functions from the MSVC RTTI in the XEX image.

The game was built with MSVC for a 32-bit big-endian target, so the RTTI structures are the
usual x86 ones with absolute 32-bit pointers stored big-endian:

  TypeDescriptor             { pVFTable (type_info vtable), spare, char name[] ".?AVFoo@ns@@" }
  CompleteObjectLocator      { signature 0, offset, cdOffset, pTypeDescriptor, pClassHierarchy }
  ClassHierarchyDescriptor   { signature 0, attributes (1 MI, 2 VI, 4 ambiguous), numBaseClasses,
                               pBaseClassArray }
  BaseClassArray             pointers to BaseClassDescriptor, the class itself first, then its
                             bases in pre-order
  BaseClassDescriptor        { pTypeDescriptor, numContainedBases, mdisp, pdisp, vdisp, attributes }

Each vtable is preceded by a pointer to its CompleteObjectLocator (vtable[-1]); its entries run
until the first word that does not point into a code section. Every entry is named after the
class that owns the slot:

  rtti            the function appears only in vtables of one class: Class::vfNN (slot NN,
                  byte offset 4*NN), or Class::Base::vfNN for a secondary vtable (multiple
                  inheritance: the Base subobject at COL.offset)
  rtti-inherited  several classes share it and one of them is a base of all the others: the
                  slot belongs to that base (Base::vfNN), the others inherit it
  rtti-shared     classes without a common base share it: identical code folded by the linker
                  (nullsub, return 0/1, ...) or _purecall. Named after the body when it is trivial,
                  otherwise after the first class (note says how many classes use it)
  rtti-dtor       a slot recognised as `scalar deleting destructor' (tests flags & 1 of r4 and
                  calls operator delete) or `vector deleting destructor' (also tests flags & 2)
  heuristic       functions found through those: Class::~Class (the only call before the
                  flags test), operator delete, _purecall, _amsg_exit
  import          import thunks from private/symbols.txt

Outputs (derived from your copy of the game: keep them in private/, do not publish):
  private/symbols/rtti.json      classes, bases, vtables, methods, per-function users
  private/symbols/functions.csv  address,name,source,note (one row per named function)

The runtime loads functions.csv to print names in [fatal]/[crash]/[watchdog] reports
(runtime/symbols.cpp). tools/annotate_ppc.py, tools/ghidra_apply_symbols.py and
tools/ida_apply_symbols.py consume it.

Usage (from the repository root):
  python tools/rtti_scan.py [--image private/image.bin] [--out private/symbols]
"""
import argparse
import bisect
import collections
import csv
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE_BASE = 0x82000000

BLR = 0x4E800020


# ---------------------------------------------------------------------------------------------
# Image access

class Image:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        opt = struct.unpack_from('<H', d, pe + 20)[0]
        self.sections = []
        for i in range(nsec):
            off = pe + 24 + opt + 40 * i
            name, vsize, va = struct.unpack_from('<8sII', d, off)
            flags = struct.unpack_from('<I', d, off + 36)[0]
            self.sections.append((name.rstrip(b'\0').decode(), IMAGE_BASE + va, vsize, flags))
        self.code = [(s[1], s[1] + s[2]) for s in self.sections if s[3] & 0x20000000]
        self.end = IMAGE_BASE + len(d)

    def word(self, addr):
        return struct.unpack_from('>I', self.data, addr - IMAGE_BASE)[0]

    def sword(self, addr):
        return struct.unpack_from('>i', self.data, addr - IMAGE_BASE)[0]

    def contains(self, addr):
        return IMAGE_BASE <= addr < self.end - 3

    def is_code(self, addr):
        return addr & 3 == 0 and any(lo <= addr < hi for lo, hi in self.code)

    def data_words(self):
        """(address, value) of every aligned word outside the code sections."""
        d = self.data
        for name, va, vsize, flags in self.sections:
            if flags & 0x20000000 or name == '.reloc':
                continue
            lo = va - IMAGE_BASE
            hi = min(lo + vsize, len(d)) & ~3
            values = struct.unpack_from('>%dI' % ((hi - lo) // 4), d, lo)
            for i, v in enumerate(values):
                yield va + 4 * i, v


def load_generated_functions(ppc_dir):
    """Start addresses of the functions XenonRecomp generated (empty if generated/ is missing)."""
    path = os.path.join(ppc_dir, 'ppc_func_mapping.cpp')
    if not os.path.exists(path):
        return []
    return sorted(int(m, 16) for m in re.findall(r'\{ 0x([0-9A-Fa-f]+),', open(path).read()))


def load_imports(path):
    """private/symbols.txt: 'address size type name' per import thunk."""
    imports = {}
    if os.path.exists(path):
        for line in open(path):
            parts = line.split()
            if len(parts) >= 4:
                imports[int(parts[0], 16)] = parts[3].removeprefix('__imp__')
    return imports


# ---------------------------------------------------------------------------------------------
# Demangling

def find_undname():
    """llvm-undname from LLVM, or undname.exe from Visual Studio."""
    for name in ('llvm-undname', 'undname'):
        path = shutil.which(name)
        if path:
            return path
    candidates = [r'C:\Program Files\LLVM\bin\llvm-undname.exe']
    for vs in glob.glob(r'C:\Program Files*\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\undname.exe'):
        candidates.append(vs)
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def simple_demangle(type_name):
    """Fallback for '.?AVName@ns1@ns2@@' without templates or back-references."""
    body = type_name[4:-2]
    if '?' in body or '$' in body:
        return None
    parts = [p for p in body.split('@') if p]
    return '::'.join(reversed(parts)) if parts else None


def demangle_all(type_names):
    """Map '.?AV...@@' type names to readable class names (vftable symbols through undname)."""
    result = {}
    tool = find_undname()
    names = sorted(set(type_names))
    if tool:
        is_llvm = 'llvm' in os.path.basename(tool).lower()
        # ??_7<name>6B@ is the vftable symbol: undname prints "const Foo::`vftable'".
        symbols = ['??_7' + n[4:] + '6B@' for n in names]
        demangled = []
        if is_llvm:
            out = subprocess.run([tool], input='\n'.join(symbols) + '\n', capture_output=True, text=True).stdout
            demangled = [l for l in out.splitlines() if l.strip()]
            # llvm-undname echoes the input line before each result.
            demangled = [l for l in demangled if not l.startswith('??_7')]
        else:
            for i in range(0, len(symbols), 200):
                out = subprocess.run([tool] + symbols[i:i + 200], capture_output=True, text=True).stdout
                demangled += re.findall(r'is :- "(.*)"', out)
        if len(demangled) == len(names):
            for n, text in zip(names, demangled):
                text = text.removeprefix('const ')
                if text.endswith("::`vftable'"):
                    result[n] = tidy_class_name(text[:-len("::`vftable'")])
        else:
            print('warning: %s returned %d names for %d inputs; using the simple demangler'
                  % (tool, len(demangled), len(names)), file=sys.stderr)
    else:
        print('warning: neither llvm-undname nor undname found; templates keep their mangled names', file=sys.stderr)
    for n in names:
        if n not in result:
            result[n] = simple_demangle(n) or n
    return result


def tidy_class_name(name):
    name = re.sub(r'\b(class|struct|union|enum) ', '', name)
    return name.replace(' >', '>').replace(',', ', ').replace(',  ', ', ')


# ---------------------------------------------------------------------------------------------
# PowerPC helpers (just enough to recognise destructor and _purecall bodies)

def bl_target(w, pc):
    if (w >> 26) == 18 and (w & 3) == 1:
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        return (pc + li) & 0xFFFFFFFF
    return None


def bit_test(w):
    """(source register, mask) for 'clrlwi[.] rA,rS,31' / 'rlwinm[.] rA,rS,0,30,30' / 'andi. rA,rS,m'."""
    op = w >> 26
    rs = (w >> 21) & 31
    if op == 28:
        return rs, w & 0xFFFF
    if op == 21 and ((w >> 11) & 31) == 0:
        mb, me = (w >> 6) & 31, (w >> 1) & 31
        if mb == me:
            return rs, 1 << (31 - mb)
    return None


def mr_source(w):
    """'mr rA,rS' (or rA,rS,rS) -> (rA, rS)."""
    if (w >> 26) == 31 and ((w >> 1) & 0x3FF) == 444 and ((w >> 21) & 31) == ((w >> 11) & 31):
        return (w >> 16) & 31, (w >> 21) & 31
    return None


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


def trivial_body(words):
    """Name for tiny bodies the linker folds across classes, or None."""
    if not words:
        return None
    if words[0] == BLR:
        return 'nullsub'
    if len(words) >= 2 and words[1] == BLR and (words[0] >> 16) == 0x3860:  # li r3,imm ; blr
        imm = words[0] & 0xFFFF
        imm = imm - 0x10000 if imm & 0x8000 else imm
        return 'return_%d' % imm if imm >= 0 else 'return_minus%d' % -imm
    if len(words) >= 2 and words[1] == BLR and words[0] == 0x7C631B78:  # mr r3,r3 ; blr
        return 'return_this'
    return None


class Functions:
    def __init__(self, image, starts):
        self.image = image
        self.starts = starts

    def body(self, f, limit=0x200):
        """Instruction words from f to the next generated function (or limit bytes)."""
        i = bisect.bisect_right(self.starts, f)
        end = self.starts[i] if i < len(self.starts) else f + limit
        end = min(end, f + limit)
        words = []
        for a in range(f, end, 4):
            if not self.image.is_code(a):
                break
            words.append(self.image.word(a))
        return words


def analyse_deleting_dtor(funcs, f):
    """If f looks like an MSVC deleting destructor, return (kind, dtor, delete).

    Pattern: r4 (flags) is kept in a register, `flags & 1` decides whether operator delete is
    called after the object's destructor (a call, or inlined). The vector form also tests
    `flags & 2`. dtor is the target of the only bl before the test (None when inlined);
    delete is the first bl after it.
    """
    words = funcs.body(f, 0x180)
    flag_regs = {4}
    calls_before = []
    test_index = None
    masks = set()
    for i, w in enumerate(words):
        pc = f + 4 * i
        mr = mr_source(w)
        if mr and mr[1] in flag_regs:
            flag_regs.add(mr[0])
            continue
        t = bit_test(w)
        if t and t[0] in flag_regs and t[1] in (1, 2):
            masks.add(t[1])
            if t[1] == 1 and test_index is None:
                test_index = i
            continue
        target = bl_target(w, pc)
        if target is not None:
            if test_index is None:
                calls_before.append(target)
                # A call clobbers r4..r12; only the non-volatile copies survive.
                flag_regs = {r for r in flag_regs if r >= 14}
            else:
                kind = 'vector deleting destructor' if 2 in masks else 'scalar deleting destructor'
                dtor = calls_before[0] if len(calls_before) == 1 else None
                return kind, dtor, target
        if w == BLR:
            break
    return None


def looks_like_purecall(funcs, f):
    """_purecall: optional call through __pPurecall, then _amsg_exit(25) (_RT_PUREVIRT)."""
    words = funcs.body(f, 0x80)
    for i, w in enumerate(words[:-1]):
        if w == 0x38600019:  # li r3,25
            return bl_target(words[i + 1], f + 4 * (i + 1))
    return None


# ---------------------------------------------------------------------------------------------
# RTTI

def scan(image, funcs, imports):
    d = image.data

    # TypeDescriptors: the mangled name sits 8 bytes in, after pVFTable and spare.
    type_descs = {}
    for m in re.finditer(rb'\.\?A[VUW][^\x00]{1,4000}?@@\x00', d):
        off = m.start() - 8
        if off % 4 == 0 and off >= 0:
            type_descs[IMAGE_BASE + off] = m.group()[:-1].decode('latin1')
    type_info_vtables = collections.Counter(image.word(a) for a in type_descs)
    type_info_vtable = type_info_vtables.most_common(1)[0][0] if type_info_vtables else 0
    type_descs = {a: n for a, n in type_descs.items() if image.word(a) == type_info_vtable}

    def read_chd(addr):
        if not image.contains(addr + 12):
            return None
        sig, attrs, count, array = struct.unpack_from('>4I', d, addr - IMAGE_BASE)
        if sig != 0 or attrs > 7 or not 1 <= count <= 512 or not image.contains(array):
            return None
        bases = []
        for i in range(count):
            bcd = image.word(array + 4 * i)
            if not image.contains(bcd + 20):
                return None
            td, contained = image.word(bcd), image.word(bcd + 4)
            mdisp, pdisp, vdisp = image.sword(bcd + 8), image.sword(bcd + 12), image.sword(bcd + 16)
            battrs = image.word(bcd + 20)
            if td not in type_descs:
                return None
            bases.append({'type_descriptor': td, 'contained': contained, 'mdisp': mdisp,
                          'pdisp': pdisp, 'vdisp': vdisp, 'attributes': battrs})
        return {'attributes': attrs, 'bases': bases}

    # CompleteObjectLocators and their hierarchy descriptors.
    cols = {}
    chds = {}
    words = list(image.data_words())
    for addr, sig in words:
        if sig != 0 or not image.contains(addr + 16):
            continue
        _, offset, cd_offset, td, chd = struct.unpack_from('>5I', d, addr - IMAGE_BASE)
        if td not in type_descs:
            continue
        if chd not in chds:
            chds[chd] = read_chd(chd)
        if chds[chd] is None:
            continue
        cols[addr] = {'offset': offset, 'cd_offset': cd_offset, 'type_descriptor': td, 'hierarchy': chd}

    # Vtables: the word before each one is its COL.
    vtables = []
    for addr, value in words:
        if value not in cols:
            continue
        start = addr + 4
        entries = []
        a = start
        while image.contains(a) and image.is_code(image.word(a)):
            entries.append(image.word(a))
            a += 4
        vtables.append({'address': start, 'col': value, 'entries': entries})
    vtables.sort(key=lambda v: v['address'])

    names = demangle_all(type_descs.values())

    # Classes: one per TypeDescriptor; hierarchy from any COL (all COLs of a class share it).
    classes = {}
    for td, mangled in type_descs.items():
        classes[td] = {'name': names[mangled], 'mangled': mangled, 'type_descriptor': td,
                       'kind': {'V': 'class', 'U': 'struct', 'W': 'enum'}[mangled[3]],
                       'hierarchy': None, 'bases': [], 'direct_bases': [], 'vtables': []}
    for col_addr, col in cols.items():
        cls = classes[col['type_descriptor']]
        if cls['hierarchy'] is None:
            chd = chds[col['hierarchy']]
            cls['hierarchy'] = col['hierarchy']
            cls['multiple_inheritance'] = bool(chd['attributes'] & 1)
            cls['virtual_inheritance'] = bool(chd['attributes'] & 2)
            bases = chd['bases']
            cls['bases'] = [{'name': classes[b['type_descriptor']]['name'] if b['type_descriptor'] in classes else type_descs[b['type_descriptor']],
                             'type_descriptor': b['type_descriptor'], 'mdisp': b['mdisp'], 'pdisp': b['pdisp'],
                             'vdisp': b['vdisp'], 'attributes': b['attributes']} for b in bases[1:]]
            # Direct bases: walk the pre-order array skipping each base's own bases.
            i = 1
            while i < len(bases):
                cls['direct_bases'].append(bases[i]['type_descriptor'])
                i += 1 + bases[i]['contained']
    # Hierarchy-only classes (no vtable seen) can still be resolved through BCDs of others.
    for cls in classes.values():
        for b in cls['bases']:
            b['name'] = classes[b['type_descriptor']]['name']

    ancestors = {td: {b['type_descriptor'] for b in cls['bases']} for td, cls in classes.items()}

    def subobject_name(cls, offset):
        """Base class whose subobject starts at offset (non-virtual bases only)."""
        candidates = [b for b in cls['bases'] if b['mdisp'] == offset and b['pdisp'] == -1]
        # The outermost base at that offset is the first one in pre-order.
        return candidates[0]['name'] if candidates else '+0x%X' % offset

    # Per-function users.
    users = collections.defaultdict(list)   # func -> [(class td, vtable addr, slot, offset)]
    for vt in vtables:
        col = cols[vt['col']]
        td = col['type_descriptor']
        cls = classes[td]
        vt_info = {'address': vt['address'], 'col': vt['col'], 'offset': col['offset'],
                   'cd_offset': col['cd_offset'],
                   'subobject': None if col['offset'] == 0 else subobject_name(cls, col['offset']),
                   'methods': []}
        cls['vtables'].append(vt_info)
        for slot, f in enumerate(vt['entries']):
            users[f].append((td, vt['address'], slot, col['offset']))
            vt_info['methods'].append({'slot': slot, 'address': f})

    def slot_name(td, offset, slot):
        cls = classes[td]
        if offset == 0:
            return '%s::vf%02d' % (cls['name'], slot)
        return '%s::%s::vf%02d' % (cls['name'], subobject_name(cls, offset), slot)

    def slot_classes(td, offset):
        """Classes whose vtable layout this (class, subobject offset) slot belongs to: the class
        itself (primary vtable) and every non-virtual base that sits at that offset."""
        chain = {b['type_descriptor'] for b in classes[td]['bases'] if b['mdisp'] == offset and b['pdisp'] == -1}
        if offset == 0:
            chain.add(td)
        return chain

    # Name every vtable entry.
    func_names = {}       # addr -> (name, source, note)
    func_info = {}
    slot_owner = {}       # addr -> class (type descriptor) that owns the slot, for primary vtables
    for f, us in users.items():
        tds = {u[0] for u in us}
        if len(tds) == 1:
            td = next(iter(tds))
            u = min((x for x in us if x[0] == td), key=lambda x: (x[3], x[2]))
            name, source, note = slot_name(td, u[3], u[2]), 'rtti', ''
            if u[3] == 0:
                slot_owner[f] = td
        else:
            # Owner: the most derived class whose vtable layout every user shares at that slot.
            common = set.intersection(*(slot_classes(u[0], u[3]) for u in us))
            slots = {u[2] for u in us}
            if common and len(slots) == 1:
                owner = max(common, key=lambda td: (len(ancestors[td]), classes[td]['name']))
                name, source = '%s::vf%02d' % (classes[owner]['name'], next(iter(slots))), 'rtti-inherited'
                slot_owner[f] = owner
                if owner in tds:
                    note = 'inherited by %d classes' % (len(tds) - 1)
                else:
                    note = '%s has another function in this slot; this one is shared by %d classes deriving from it' % (
                        classes[owner]['name'], len(tds))
            else:
                trivial = trivial_body(funcs.body(f, 16))
                first = min(us, key=lambda x: x[1])
                name = trivial or slot_name(first[0], first[3], first[2])
                source = 'rtti-shared'
                note = 'shared by %d unrelated classes' % len(tds)
        func_names[f] = (name, source, note)
        func_info[f] = {'name': name, 'source': source, 'note': note,
                        'users': [{'class': classes[u[0]]['name'], 'vtable': u[1], 'slot': u[2]} for u in us]}

    # Deleting destructors, destructors, operator delete.
    delete_votes = collections.Counter()
    dtor_of = collections.defaultdict(set)    # dtor addr -> classes whose deleting dtor calls it
    for f in users:
        res = analyse_deleting_dtor(funcs, f)
        if not res:
            continue
        kind, dtor, delete = res
        delete_votes[delete] += 1
        name, source, note = func_names[f]
        if '::vf' not in name:
            continue  # folded trivial body (nullsub etc.)
        owner, slot = name.rsplit('::vf', 1)
        func_names[f] = ("%s::`%s'" % (owner, kind), 'rtti-dtor', 'slot %s%s' % (slot, '; ' + note if note else ''))
        if dtor is not None and f in slot_owner:
            dtor_of[dtor].add(slot_owner[f])

    heuristic = {}
    if delete_votes:
        delete, votes = delete_votes.most_common(1)[0]
        if delete not in func_names:
            heuristic[delete] = ('operator delete', 'heuristic', 'called by %d deleting destructors' % votes)
    for dtor, owners in dtor_of.items():
        if dtor in func_names or dtor in heuristic:
            continue
        # Several deleting destructors calling the same function: the derived destructors were
        # inlined down to a call to their common base's destructor (or folded by the linker).
        common = set.intersection(*(ancestors[o] | {o} for o in owners))
        if common:
            owner = classes[max(common, key=lambda td: (len(ancestors[td]), classes[td]['name']))]['name']
            short = re.sub(r'<.*', '', split_scope(owner)[-1])
            note = 'called by the deleting destructor' + (' of %d classes' % len(owners) if len(owners) > 1 else '')
            heuristic[dtor] = ('%s::~%s' % (owner, short), 'heuristic', note)
        else:
            heuristic[dtor] = ('shared_dtor', 'heuristic', 'destructor folded across %d unrelated classes: %s' % (
                len(owners), ', '.join(sorted(classes[o]['name'] for o in owners)[:6])))

    # _purecall: shared by classes without a common base, ends in _amsg_exit(25).
    for f, (name, source, note) in list(func_names.items()):
        if source == 'rtti-shared':
            amsg = looks_like_purecall(funcs, f)
            if amsg:
                func_names[f] = ('_purecall', source, note)
                if amsg not in func_names:
                    heuristic[amsg] = ('_amsg_exit', 'heuristic', 'called by _purecall with _RT_PUREVIRT')

    func_names.update(heuristic)
    for f, name in imports.items():
        func_names[f] = (name, 'import', '')

    # Unique names: when several functions end up with the same name (implementations shared by
    # subsets of a base's descendants, folded trivial bodies), the one that really owns the slot
    # (or, if none does, the one most classes use) keeps it; the others get their address appended.
    by_name = collections.defaultdict(list)
    for f, (name, source, note) in func_names.items():
        by_name[name].append(f)
    for name, fs in by_name.items():
        if len(fs) < 2:
            continue
        def rank(f):
            source, note = func_names[f][1], func_names[f][2]
            owns = source in ('rtti', 'import') or 'inherited by' in note
            return (owns, len({u[0] for u in users.get(f, [])}), -f)
        keep = max(fs, key=rank)
        for f in fs:
            if f != keep:
                n, s, note = func_names[f]
                func_names[f] = ('%s_%08X' % (n, f), s, note)

    for f, info in func_info.items():
        info['name'], info['source'], info['note'] = func_names[f]

    # Fill vtable method names.
    for cls in classes.values():
        for vt in cls['vtables']:
            for m in vt['methods']:
                n, s, note = func_names[m['address']]
                m['name'] = n
                m['source'] = s
                m['shared'] = len({u[0] for u in users[m['address']]}) > 1

    return {
        'type_info_vtable': type_info_vtable,
        'type_descriptors': len(type_descs),
        'cols': len(cols),
        'classes': classes,
        'vtables': vtables,
        'functions': func_names,
        'function_info': func_info,
    }


def write_outputs(result, out_dir, generated):
    os.makedirs(out_dir, exist_ok=True)
    classes = sorted(result['classes'].values(), key=lambda c: c['name'])
    hexify = lambda v: '0x%08X' % v

    json_classes = []
    for cls in classes:
        json_classes.append({
            'name': cls['name'],
            'mangled': cls['mangled'],
            'kind': cls['kind'],
            'type_descriptor': hexify(cls['type_descriptor']),
            'multiple_inheritance': cls.get('multiple_inheritance', False),
            'virtual_inheritance': cls.get('virtual_inheritance', False),
            'direct_bases': [result['classes'][td]['name'] for td in cls['direct_bases']],
            'bases': [{'name': b['name'], 'mdisp': b['mdisp'], 'pdisp': b['pdisp'], 'vdisp': b['vdisp'],
                       'attributes': hexify(b['attributes'])} for b in cls['bases']],
            'vtables': [{'address': hexify(vt['address']), 'col': hexify(vt['col']), 'offset': vt['offset'],
                         'subobject': vt['subobject'],
                         'methods': [{'slot': m['slot'], 'address': hexify(m['address']), 'name': m['name'],
                                      'source': m['source'], 'shared': m['shared']} for m in vt['methods']]}
                        for vt in cls['vtables']],
        })

    functions = result['functions']
    # ppc_func_mapping.cpp also lists the import thunks; count only recompiled game functions.
    gen = set(generated) - {f for f, (_, s, _) in functions.items() if s == 'import'}
    named_generated = sum(1 for f in functions if f in gen)
    by_source = collections.Counter(s for _, s, _ in functions.values())
    stats = {
        'type_descriptors': result['type_descriptors'],
        'complete_object_locators': result['cols'],
        'classes': len(classes),
        'classes_with_vtables': sum(1 for c in classes if c['vtables']),
        'vtables': len(result['vtables']),
        'vtable_entries': sum(len(v['entries']) for v in result['vtables']),
        'distinct_virtual_functions': len(result['function_info']),
        'recompiled_functions': len(gen),
        'named_recompiled_functions': named_generated,
        'named_by_source': dict(sorted(by_source.items())),
        'vtable_entries_without_generated_function': sorted(hexify(f) for f in result['function_info'] if gen and f not in gen),
    }

    with open(os.path.join(out_dir, 'rtti.json'), 'w', encoding='utf-8') as fp:
        json.dump({'stats': stats, 'classes': json_classes,
                   'functions': {hexify(f): {**info, 'users': [{**u, 'vtable': hexify(u['vtable'])} for u in info['users']]}
                                 for f, info in sorted(result['function_info'].items())}},
                  fp, indent=1, ensure_ascii=False)

    with open(os.path.join(out_dir, 'functions.csv'), 'w', newline='', encoding='utf-8') as fp:
        writer = csv.writer(fp)
        writer.writerow(['address', 'name', 'source', 'note'])
        for f in sorted(functions):
            name, source, note = functions[f]
            writer.writerow([hexify(f), name, source, note])
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--image', default=os.path.join(ROOT, 'private', 'image.bin'))
    parser.add_argument('--symbols', default=os.path.join(ROOT, 'private', 'symbols.txt'))
    parser.add_argument('--ppc', default=os.path.join(ROOT, 'generated', 'ppc'))
    parser.add_argument('--out', default=os.path.join(ROOT, 'private', 'symbols'))
    args = parser.parse_args()

    image = Image(args.image)
    generated = load_generated_functions(args.ppc)
    if not generated:
        print('warning: %s has no ppc_func_mapping.cpp; function bodies end at 0x200 bytes' % args.ppc, file=sys.stderr)
    funcs = Functions(image, generated)
    result = scan(image, funcs, load_imports(args.symbols))
    stats = write_outputs(result, args.out, generated)

    print('type descriptors      %d (type_info vtable 0x%08X)' % (stats['type_descriptors'], result['type_info_vtable']))
    print('object locators       %d' % stats['complete_object_locators'])
    print('classes               %d (%d with vtables)' % (stats['classes'], stats['classes_with_vtables']))
    print('vtables               %d, %d entries, %d distinct functions' % (stats['vtables'], stats['vtable_entries'], stats['distinct_virtual_functions']))
    for source, count in stats['named_by_source'].items():
        print('  %-20s %d' % (source, count))
    if generated:
        print('named functions       %d of %d recompiled functions (%.1f%%), plus %d imports' % (
            stats['named_recompiled_functions'], stats['recompiled_functions'],
            100.0 * stats['named_recompiled_functions'] / max(1, stats['recompiled_functions']),
            stats['named_by_source'].get('import', 0)))
    if stats['vtable_entries_without_generated_function']:
        print('warning: vtable entries with no generated function (swallowed? see find_swallowed.py): %s'
              % ', '.join(stats['vtable_entries_without_generated_function'][:20]))
    print('wrote %s and %s' % (os.path.join(args.out, 'rtti.json'), os.path.join(args.out, 'functions.csv')))


if __name__ == '__main__':
    main()
