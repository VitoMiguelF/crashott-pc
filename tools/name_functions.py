#!/usr/bin/env python3
"""Name more game functions after the RTTI pass: hand-curated names plus guesses from the binary.

tools/rtti_scan.py calls extend() before writing private/symbols/functions.csv, so one command
makes the whole file; running this script runs rtti_scan.py. Every row says where its name came
from (source) and how much to trust it (confidence), so the guesses stay apart from the facts:

  manual          config/function_names.csv (tracked: our names and notes, no game bytes),
                  written from what the runtime hooks and the docs found out. It overrides
                  any other name; the RTTI name it replaces goes into the note.
  lua-table       luaL_reg tables in the data ({char *name, lua_CFunction}, ending with
                  {0, 0}): the function is named after the library and the Lua name
                  (lua::str_format, lua::luaB_print, lua::vector_normalize).
  lua-string      Lua 5.1-work/5.0 and MSVC CRT functions recognised by an error message
                  that only that function references (LIBRARY_STRINGS below: Lua and CRT
                  knowledge, not game data).
  script-binding  the script binding registrations: the function address is loaded right
                  before its Lua name (AddMethod(class, fn, "GetJacker")); the class is the
                  first string of the registering function when it is one.
  ctor            stores a class's primary vtable at 0(this), makes calls and returns this:
                  Class::Class (medium after operator new in a caller, low otherwise).
  static-init     the CRT initializer tables (a function walks [lo, hi) of function pointers):
                  static_init::sub_XXXXXXXX, the dynamic initializer of a global, and
                  static_init::atexit_XXXXXXXX, the destructor such an initializer registers.
  thunk           only `b target` (optionally after `addi r3,r3,N`, a this-adjustor):
                  Target_thunk.
  callers         every direct caller, and every function that takes the address in code,
                  belongs to the same class or namespace, and no data points at it (no table
                  of callbacks): Scope::sub_XXXXXXXX. It places the function in its system
                  without guessing what it does; repeated until nothing changes, so helpers
                  of helpers follow.

Confidence: high (RTTI facts, curated names stated by the code, Lua tables), medium (inferred
names), low (callers: only the system is known; constructors nobody allocates).

class_layouts() then names fields for tools/ghidra/ClassLayouts.java: the script accessors that
read or write one field of this (GetLedgeSpeed = lfs f1,1248(r3); blr) plus the hand-curated
config/field_names.csv, and takes object sizes from the operator new before each constructor.
"""
import collections
import csv
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE_BASE = 0x82000000

SOURCE_CONFIDENCE = {
    'rtti': 'high', 'rtti-inherited': 'high', 'rtti-dtor': 'high', 'import': 'high',
    'rtti-shared': 'medium', 'heuristic': 'medium',
    'lua-table': 'high', 'lua-string': 'medium', 'script-binding': 'medium', 'ctor': 'medium',
    'thunk': 'medium', 'static-init': 'medium', 'callers': 'low',
}

# Error messages that name the library function referencing them (Lua 5.1-work, built into the
# game, and the MSVC CRT). Used only when exactly one function references the string.
LIBRARY_STRINGS = {
    'C stack overflow': 'lua::luaD_call',
    'attempt to yield across metamethod/C-call boundary': 'lua::lua_yield',
    'cannot resume non-suspended coroutine': 'lua::lua_resume',
    'loop in gettable': 'lua::luaV_gettable',
    'loop in settable': 'lua::luaV_settable',
    'string size overflow': 'lua::luaV_concat',
    'table overflow': 'lua::setnodevector',
    "invalid key for `next'": 'lua::luaH_index',
    '=(tail call)': 'lua::funcinfo',
    "value for `lua_getinfo' is not a function": 'lua::lua_getinfo',
    'attempt to %s a %s value': 'lua::luaG_typeerror',
    'attempt to compare %s with %s': 'lua::luaG_ordererror',
    'function arguments expected': 'lua::funcargs',
    'unexpected symbol': 'lua::prefixexp',
    'no loop to break': 'lua::breakstat',
    'chunk has too many syntax levels': 'lua::enterlevel',
    '<eof> expected': 'lua::luaY_parser',
    "calling `%s' on bad self (%s)": 'lua::luaL_argerror',
    '%s expected, got %s': 'lua::luaL_typerror',
    'value expected': 'lua::luaL_checkany',
    'stack overflow (%s)': 'lua::luaL_checkstack',
    '%s:%d: ': 'lua::luaL_where',
    'table contains non-strings': 'lua::tab_concat',
    'invalid order function for sorting': 'lua::auxsort',
    'base out of range': 'lua::luaB_tonumber',
    'table too big to unpack': 'lua::luaB_unpack',
    'assertion failed!': 'lua::luaB_assert',
    'reader function must return a string': 'lua::generic_reader',
    'boolean or proxy expected': 'lua::luaB_newproxy',
    "`_LOADED' is not a table": 'lua::luaB_require',
    'too many arguments to resume': 'lua::auxresume',
    'Lua function expected': 'lua::luaB_cocreate',
    'Bad dynamic_cast!': 'crt::__RTDynamicCast',
    'Attempted a typeid of NULL pointer!': 'crt::__RTtypeid',
}

# Lua library prefixes as in the Lua sources (lbaselib.c luaB_, lstrlib.c str_, ...); Radical's
# own libraries (vector, object, schedule...) get their table name.
LUA_LIB_PREFIX = {'base': 'luaB_', 'string': 'str_', 'table': 'tab_', 'math': 'math_', 'io': 'io_',
                  'os': 'os_', 'coroutine': 'luaB_co', 'co': 'luaB_co', 'debug': 'db_'}

IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_]{1,60}')


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


# ---------------------------------------------------------------------------------------------
# The binary: per-function direct calls, tail jumps and the addresses it builds with lis.

class Program:
    def __init__(self, image, starts):
        self.image = image
        self.starts = sorted(starts)
        self.startset = set(self.starts)
        self.data_secs = [(va, va + vsize) for name, va, vsize, flags in image.sections if not flags & 0x20000000]
        self.writable_secs = [(va, va + vsize) for name, va, vsize, flags in image.sections if flags & 0x80000000]
        self.calls, self.tails, self.refs, self.ends = {}, {}, {}, {}
        for i, f in enumerate(self.starts):
            end = self.starts[i + 1] if i + 1 < len(self.starts) else f + 0x200
            self.ends[f] = end
            self.calls[f], self.tails[f], self.refs[f] = self._scan(f, end)
        self.callers = collections.defaultdict(set)
        for f in self.starts:
            for t in self.calls[f] + self.tails[f]:
                if t in self.startset and t != f:
                    self.callers[t].add(f)
        # Address taken: in the data (vtables, tables of callbacks) or built in code (lis/addi).
        self.data_words = dict(image.data_words())
        self.in_data = {v for v in self.data_words.values() if v in self.startset}
        self.takers = collections.defaultdict(set)
        for f in self.starts:
            for x in self.refs[f]:
                if x in self.startset and x != f:
                    self.takers[x].add(f)
        self._strings = {}

    def writable(self, addr):
        return any(lo <= addr < hi for lo, hi in self.writable_secs)

    def words(self, f):
        a, end = f, self.ends[f]
        while a < end and self.image.is_code(a):
            yield a, self.image.word(a)
            a += 4

    def _scan(self, f, end):
        """Linear pass: lis rD,hi then addi/ori/load/store with rD as base give an address."""
        hi = {}
        calls, tails, refs = [], [], []
        for a, w in self.words(f):
            op = w >> 26
            rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15 and ra == 0:                      # lis
                hi[rd] = (imm << 16) & 0xFFFFFFFF
            elif op == 14 and ra in hi:                   # addi
                refs.append((hi[ra] + sx16(imm)) & 0xFFFFFFFF)
                hi.pop(rd, None)
            elif op == 24 and rd in hi:                   # ori rA,rS,imm
                refs.append(hi[rd] | imm)
                hi.pop(ra, None)
            elif 32 <= op <= 55 and ra in hi:             # D-form load/store
                refs.append((hi[ra] + sx16(imm)) & 0xFFFFFFFF)
                if op < 48 and op not in (36, 37, 38, 39, 44, 45, 47):
                    hi.pop(rd, None)
            elif op == 18:                                # b / bl
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                t = (li if w & 2 else a + li) & 0xFFFFFFFF
                if w & 1:
                    calls.append(t)
                elif t in self.startset and not f <= t < end:
                    tails.append(t)
            elif op == 31 or op in (7, 8, 10, 11, 12, 13, 20, 21, 23, 25, 26, 27, 28, 29):
                hi.pop(rd, None)
                hi.pop(ra, None)
        return calls, tails, refs

    def cstring(self, addr):
        if addr in self._strings:
            return self._strings[addr]
        s = None
        if any(lo <= addr < hi for lo, hi in self.data_secs):
            d, o = self.image.data, addr - IMAGE_BASE
            e = o
            while e < len(d) and e - o < 300 and (32 <= d[e] < 127 or d[e] in (9, 10, 13)):
                e += 1
            if e < len(d) and d[e] == 0 and e - o >= 2:
                s = d[o:e].decode('ascii')
        self._strings[addr] = s
        return s


def scope_of(name):
    """'pure3d::frontend::Menu::Update' -> 'pure3d::frontend::Menu' (None without a scope)."""
    depth, cut = 0, None
    for i, c in enumerate(name):
        if c in '<(':
            depth += 1
        elif c in '>)':
            depth -= 1
        elif depth == 0 and name.startswith('::', i):
            cut = i
    return name[:cut] if cut else None


def last_part(name):
    s = scope_of(name)
    return name[len(s) + 2:] if s else name


# ---------------------------------------------------------------------------------------------
# Passes. Each returns {address: (name, note)} for functions that have no name yet.

def load_manual(path):
    rows = {}
    if not os.path.exists(path):
        return rows
    with open(path, newline='', encoding='utf-8') as fp:
        lines = [l for l in fp if l.strip() and not l.startswith('#')]
    for row in csv.DictReader(lines):
        rows[int(row['address'], 16)] = (row['name'].strip(), (row.get('confidence') or 'high').strip(),
                                         (row.get('note') or '').strip())
    return rows


def pass_lua_tables(prog, named):
    """luaL_reg arrays: runs of {string, function start} word pairs ending with {0, 0}."""
    words = prog.data_words
    entries = {}
    for a, v in words.items():
        if v in prog.startset:
            s = prog.cstring(words.get(a - 4, 0))
            if s and IDENT.fullmatch(s):
                entries[a - 4] = (s, v)
    tables = []
    for a in sorted(entries):
        if a - 8 in entries:
            continue
        run, b = [], a
        while b in entries:
            run.append(entries[b])
            b += 8
        if len(run) >= 2 and words.get(b) == 0 and words.get(b + 4) == 0:
            tables.append((a, run))
    table_at = {a: run for a, run in tables}

    def opens_table(fn):
        return any(x in table_at for g in [fn] + prog.calls[fn] if g in prog.refs for x in prog.refs[g])

    # The library list ({"math", luaopen_math}, ...): most entries open a table themselves or
    # through a callee.
    out = {}
    lists = set()
    opened_by = collections.defaultdict(set)    # table -> libraries whose luaopen_* builds it
    for a, run in tables:
        if 2 * sum(opens_table(fn) for _, fn in run) > len(run):
            lists.add(a)
            for libname, fn in run:
                out.setdefault(fn, ('lua::luaopen_' + libname, 'opens the Lua library %s (list 0x%08X)' % (libname, a)))
                for g in [fn] + prog.calls[fn]:
                    for x in prog.refs.get(g, ()):
                        if x in table_at:
                            opened_by[x].add(libname)
    # luaL_openlib(L, "math", mathlib, 0): the library name is the string built next to the table.
    lib_of = collections.defaultdict(set)
    for f in prog.starts:
        refs = prog.refs[f]
        for i, x in enumerate(refs):
            if x in table_at:
                near = [prog.cstring(refs[j]) for j in (i + 1, i - 1) if 0 <= j < len(refs)]
                near = [s for s in near if s and re.fullmatch(r'[a-z][a-z0-9_]{1,30}', s)]
                if near:
                    lib_of[x].add(near[0])
    for a, run in tables:
        if a in lists:
            continue
        libs = lib_of[a] if len(lib_of[a]) == 1 else opened_by[a]
        lib = next(iter(libs)) if len(libs) == 1 else None
        if lib:
            prefix, shown = LUA_LIB_PREFIX.get(lib, lib + '_'), lib + '.'
        else:   # metatables (__index, __gc...) and tables opened another way
            prefix, shown = 'reg%08X_' % a, ''
        for lname, fn in run:
            out.setdefault(fn, ('lua::%s%s' % (prefix, lname), 'Lua "%s%s" (luaL_reg 0x%08X)' % (shown, lname, a)))
    return {f: v for f, v in out.items() if f not in named}


def pass_library_strings(prog, named):
    users = collections.defaultdict(set)
    for f in prog.starts:
        for x in prog.refs[f]:
            s = prog.cstring(x)
            if s in LIBRARY_STRINGS:
                users[s].add(f)
    out = {}
    for s, fs in users.items():
        if len(fs) == 1:
            f = next(iter(fs))
            if f not in named:
                out[f] = (LIBRARY_STRINGS[s], 'references "%s"' % s[:40])
    return out


def pass_script_bindings(prog, named, class_names):
    """A function address loaded right before a name string: a script method/property binding."""
    pairs = collections.defaultdict(set)     # function -> {(class, name)}
    for f in prog.starts:
        refs = prog.refs[f]
        strings = [prog.cstring(x) for x in refs]
        cls = next((s for s in strings if s), None)
        cls = cls if cls in class_names else None
        for i in range(len(refs) - 1):
            fn, s = refs[i], strings[i + 1]
            if fn in prog.startset and s and IDENT.fullmatch(s) and s != cls:
                pairs[fn].add((cls, s, f))
    out = {}
    for fn, uses in pairs.items():
        names = {(c, s) for c, s, _ in uses}
        if len(names) != 1 or fn in named:
            continue
        cls, s = names.pop()
        reg = sorted(u[2] for u in uses)[0]
        out[fn] = ('%s::%s' % (cls or 'script', s), 'bound to Lua as "%s" by 0x%08X' % (s, reg))
    return out


def pass_constructors(prog, named, primary_vtables, allocators):
    """Stores a primary vtable at 0(this) and gives this back in r3.

    Destructors look the same when they make no calls, so the name needs a call: medium when a
    caller allocates the object first (operator new right before the call), low otherwise.
    """
    out = {}
    for f in prog.starts:
        if f in named or not prog.calls[f]:
            continue
        this, val = {3}, {}
        stored = []
        for a, w in prog.words(f):
            op = w >> 26
            rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15 and ra == 0:                      # lis
                val[rd] = (imm << 16) & 0xFFFFFFFF
                this.discard(rd)
            elif op == 14:                                # addi
                val[rd] = (val[ra] + sx16(imm)) & 0xFFFFFFFF if val.get(ra) is not None else None
                this.discard(rd)
            elif op == 31 and (w >> 1) & 0x3FF == 444 and rd == (w >> 11) & 31:   # mr ra,rs
                if rd in this:
                    this.add(ra)
                else:
                    this.discard(ra)
                val[ra] = val.get(rd)
            elif op == 36 and imm == 0 and ra in this and val.get(rd) in primary_vtables:   # stw rs,0(this)
                stored.append(primary_vtables[val[rd]])
            elif op == 18 and w & 1:                      # bl: volatile registers are gone
                for r in [0] + list(range(3, 13)):
                    this.discard(r)
                    val.pop(r, None)
            elif w == 0x4E800020:                         # blr
                break
            elif op not in (36, 37, 38, 44, 45, 52, 53, 54, 55, 62, 10, 11, 16):
                this.discard(rd)
                val.pop(rd, None)
        if stored and 3 in this:
            cls = stored[-1]
            allocated = any(cs[i - 1] in allocators for g in prog.callers.get(f, ())
                            for cs in [prog.calls[g]] for i in range(1, len(cs)) if cs[i] == f)
            out[f] = ('%s::%s' % (cls, last_part(cls).split('<')[0]),
                      'stores the %s vtable at 0(this) and returns this%s' % (cls, '; called after operator new' if allocated else ''),
                      'medium' if allocated else 'low')
    return out


def pass_thunks(prog, names):
    out = {}
    for f in prog.starts:
        if f in names:
            continue
        ws = [w for _, w in prog.words(f)][:3]
        adjust = None
        if ws and ws[0] >> 26 == 14 and (ws[0] >> 21) & 31 == 3 and (ws[0] >> 16) & 31 == 3:
            adjust = sx16(ws[0] & 0xFFFF)
            ws = ws[1:]
        if ws and ws[0] >> 26 == 18 and not ws[0] & 1 and prog.tails[f]:
            t = prog.tails[f][0]
            if t in names:
                note = 'jumps to 0x%08X' % t + (' with this %+d' % adjust if adjust is not None else '')
                out[f] = (names[t] + '_thunk', note)
    return out


def pass_static_init(prog, named):
    """The CRT initializer tables: a function walks [lo, hi) of function pointers in .data (_initterm).

    Each entry is the dynamic initializer of a global; a function whose address only such an
    initializer takes is the destructor it registers with atexit.
    """
    out = {}
    inits = set()
    for g in prog.starts:
        refs = sorted({x for x in prog.refs[g] if x in prog.data_words and prog.writable(x)})
        for i, lo in enumerate(refs):
            stop = lo     # the table: only null words and function starts from lo on
            while stop < lo + 0x4000 and (prog.data_words.get(stop) == 0 or prog.data_words.get(stop) in prog.startset):
                stop += 4
            his = [hi for hi in refs[i + 1:] if hi <= stop]
            if not his:
                continue
            hi = his[-1]
            entries = [prog.data_words[a] for a in range(lo, hi, 4) if prog.data_words[a]]
            if len(entries) < 8:
                continue
            for w in entries:
                if not prog.callers.get(w):
                    inits.add(w)
                    out.setdefault(w, ('static_init::sub_%08X' % w, 'global initializer: CRT table 0x%08X-0x%08X walked by 0x%08X' % (lo, hi, g)))
    for f in prog.starts:
        takers = prog.takers.get(f)
        if takers and takers <= inits and f not in prog.in_data and not prog.callers.get(f):
            out.setdefault(f, ('static_init::atexit_%08X' % f, 'destructor of a global, registered by 0x%08X' % min(takers)))
    return {f: v for f, v in out.items() if f not in named}


def pass_callers(prog, names, sources):
    """All direct callers, and the functions that take the address in code, share one scope."""
    out = {}
    for f in prog.starts:
        if f in names or f in prog.in_data:
            continue
        callers = prog.callers.get(f, set())
        users = callers | prog.takers.get(f, set())
        if not users or any(c not in names or sources.get(c) in ('import', 'rtti-shared') for c in users):
            continue
        scopes = {scope_of(names[c]) for c in users}
        if len(scopes) == 1 and None not in scopes:
            scope = scopes.pop()
            if scope in ('crt', 'kernel') or scope.endswith('_thunk'):
                continue
            via = sorted(users)
            how = 'only called from' if users == callers else 'only used (called or address taken) by'
            out[f] = ('%s::sub_%08X' % (scope, f), '%s %s' % (how,
                names[via[0]] if len(via) == 1 else '%d functions of %s' % (len(via), scope)))
    return out


def find_allocators(prog, functions):
    """operator new (config/function_names.csv) and the functions that only jump to it."""
    allocators = {f for f, (n, _, _) in functions.items() if n == 'operator new'}
    allocators |= {f for f in prog.starts if set(prog.tails[f]) & allocators and prog.ends[f] - f <= 8}
    return allocators


# ---------------------------------------------------------------------------------------------
# Class layouts: field names from the script accessors, object sizes from operator new.

BLR = 0x4E800020
LOADS = {32: ('undefined4', 4), 34: ('byte', 1), 40: ('ushort', 2), 42: ('short', 2), 48: ('float', 4), 50: ('double', 8)}
STORES = {36: ('undefined4', 4), 38: ('byte', 1), 44: ('ushort', 2), 52: ('float', 4), 54: ('double', 8)}
FLOAT_OPS = {48, 50, 52, 54}


def field_name(method):
    """GetLedgeSpeed / SetLedgeSpeed -> m_LedgeSpeed; attack_special_min_number -> m_attack_special_min_number."""
    for prefix in ('Get', 'Set'):
        if method.startswith(prefix) and method[len(prefix):len(prefix) + 1].isupper():
            return 'm_' + method[len(prefix):]
    return 'm_' + method


def accessor_field(prog, f):
    """('get' | 'set', offset, type, size) when f only reads or writes one field of this (r3)."""
    body = []
    for _, w in prog.words(f):
        if w == BLR:
            break
        body.append(w)
        if len(body) > 10:
            return None
    else:
        return None
    if prog.calls[f] or prog.tails[f]:
        return None
    value_reg = lambda w: 1 if w >> 26 in FLOAT_OPS else 4
    stores = [(w >> 26, sx16(w & 0xFFFF)) for w in body
              if w >> 26 in STORES and (w >> 16) & 31 == 3 and (w >> 21) & 31 == value_reg(w)]
    if stores:      # a setter (a clamp may store the value again: the first store is the field)
        op, off = stores[0]
        return ('set', off) + STORES[op]
    w = body[0] if len(body) == 1 else 0
    if w >> 26 in LOADS and (w >> 16) & 31 == 3 and (w >> 21) & 31 == (1 if w >> 26 in FLOAT_OPS else 3):
        return ('get', sx16(w & 0xFFFF)) + LOADS[w >> 26]
    return None


def load_manual_fields(path):
    if not os.path.exists(path):
        return []
    with open(path, newline='', encoding='utf-8') as fp:
        lines = [l for l in fp if l.strip() and not l.startswith('#')]
    return list(csv.DictReader(lines))


def class_layouts(prog, functions, classes, manual_path=None):
    """Field rows [class, offset, size, type, name, source, confidence, note] and {class: (size, note)}.

    Fields: the accessors the scripts bind (GetLedgeSpeed = lfs f1,1248(r3); blr -> +1248
    m_LedgeSpeed float), plus config/field_names.csv. Sizes: the `li r3,N` before the
    operator new whose result goes to the class's constructor.
    """
    manual_path = manual_path or os.path.join(ROOT, 'config', 'field_names.csv')
    class_names = {c['name'] for c in classes.values()}
    found = collections.defaultdict(list)       # (class, offset) -> [(name, type, size, note)]
    for f, (name, source, _) in functions.items():
        scope = scope_of(name)
        if source not in ('script-binding', 'manual') or scope not in class_names:
            continue
        acc = accessor_field(prog, f)
        if acc:
            kind, off, typ, size = acc
            method = last_part(name)
            if typ == 'byte' and kind == 'get' and method.startswith(('Is', 'Has', 'Can', 'Should')):
                typ = 'bool'
            found[(scope, off)].append((field_name(method), typ, size, '%s %s 0x%08X' % (kind, method, f)))
    rows = {}
    for (cls, off), uses in found.items():
        best = collections.Counter(u[0] for u in uses).most_common(1)[0][0]
        typ, size = next((u[1], u[2]) for u in uses if u[0] == best)
        rows[(cls, off)] = [cls, off, size, typ, best, 'accessor', 'medium', '; '.join(sorted(u[3] for u in uses))]
    for row in load_manual_fields(manual_path):
        off = int(row['offset'], 0)
        rows[(row['class'], off)] = [row['class'], off, int(row.get('size') or 4), row['type'], row['name'], 'manual',
                                     row.get('confidence') or 'high', row.get('note', '')]

    # Object sizes: in each caller of a constructor, the last `li r3,N` before the call to
    # operator new that comes right before the constructor call.
    allocators = find_allocators(prog, functions)
    ctors = {f: scope_of(n) for f, (n, s, _) in functions.items()
             if scope_of(n) and last_part(n) == last_part(scope_of(n)).split('<')[0]}
    sizes = collections.defaultdict(collections.Counter)
    for g in {c for f in ctors for c in prog.callers.get(f, ())}:
        li_r3 = pending = None
        for a, w in prog.words(g):
            op = w >> 26
            if op == 14 and (w >> 21) & 31 == 3 and (w >> 16) & 31 == 0:
                li_r3 = sx16(w & 0xFFFF)
            elif op == 18 and w & 1:
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                t = (a + li) & 0xFFFFFFFF
                if t in allocators:
                    pending = li_r3
                else:
                    if pending and pending > 0 and t in ctors:
                        sizes[ctors[t]][pending] += 1
                    pending = None
                li_r3 = None
    class_sizes = {}
    for cls, c in sizes.items():
        size, n = c.most_common(1)[0]
        class_sizes[cls] = (size, 'operator new(%d) right before the constructor (%d of %d call sites)' % (size, n, sum(c.values())))
    return sorted(rows.values(), key=lambda r: (r[0], r[1])), class_sizes


def write_layouts(out_dir, fields, class_sizes):
    with open(os.path.join(out_dir, 'fields.csv'), 'w', newline='', encoding='utf-8') as fp:
        w = csv.writer(fp)
        w.writerow(['class', 'offset', 'size', 'type', 'name', 'source', 'confidence', 'note'])
        for r in fields:
            w.writerow([r[0], '0x%X' % r[1]] + r[2:])
    with open(os.path.join(out_dir, 'classes.csv'), 'w', newline='', encoding='utf-8') as fp:
        w = csv.writer(fp)
        w.writerow(['class', 'size', 'note'])
        for cls in sorted(class_sizes):
            w.writerow([cls, '0x%X' % class_sizes[cls][0], class_sizes[cls][1]])


# ---------------------------------------------------------------------------------------------

def extend(functions, image, starts, classes, manual_path=None, prog=None):
    """Add the manual and propagated names to functions {address: (name, source, note)}.

    Returns {address: confidence} for every row. classes: rtti_scan's result['classes'].
    """
    manual_path = manual_path or os.path.join(ROOT, 'config', 'function_names.csv')
    prog = prog or Program(image, starts)
    confidence = {f: SOURCE_CONFIDENCE.get(s, 'medium') for f, (_, s, _) in functions.items()}

    for f, (name, conf, note) in load_manual(manual_path).items():
        if f not in prog.startset:
            print('warning: %s: 0x%08X (%s) is not a generated function start' % (manual_path, f, name), file=sys.stderr)
            continue
        if f in functions and functions[f][0] != name:
            note = '%s (was %s [%s])' % (note, functions[f][0], functions[f][1]) if note else 'was %s' % functions[f][0]
        functions[f] = (name, 'manual', note)
        confidence[f] = conf

    used = collections.Counter(n for n, _, _ in functions.values())

    def add(found, source):
        for f, (name, note, *conf) in sorted(found.items()):
            if f in functions:
                continue
            if used[name]:
                name = '%s_%08X' % (name, f)
            used[name] += 1
            functions[f] = (name, source, note)
            confidence[f] = conf[0] if conf else SOURCE_CONFIDENCE[source]
        return len(found)

    class_names = {c['name'] for c in classes.values()}
    primary = {}
    for c in classes.values():
        for vt in c['vtables']:
            if vt['offset'] == 0:
                primary[vt['address']] = c['name']

    add(pass_lua_tables(prog, functions), 'lua-table')
    add(pass_library_strings(prog, functions), 'lua-string')
    add(pass_script_bindings(prog, functions, class_names), 'script-binding')
    allocators = find_allocators(prog, functions)
    add(pass_constructors(prog, functions, primary, allocators), 'ctor')
    add(pass_static_init(prog, functions), 'static-init')
    for _ in range(50):
        names = {f: n for f, (n, _, _) in functions.items()}
        sources = {f: s for f, (_, s, _) in functions.items()}
        found = pass_thunks(prog, names)
        found.update({f: v for f, v in pass_callers(prog, names, sources).items() if f not in found})
        if not found:
            break
        for f, v in found.items():
            add({f: v}, 'thunk' if v[0].endswith('_thunk') else 'callers')
    return confidence


def system_of(name, source):
    """Coverage bucket: the namespace, or 'game' for global C* classes, '(no scope)' otherwise."""
    if source == 'import':
        return 'kernel imports'
    first = name.split('::')[0].split('<')[0].lstrip('`')
    if '::' not in name:
        return '(no scope)'
    if re.match(r'C[A-Z]', first) or first in ('SaveGameManager',):
        return 'game (C* classes)'
    return first


def coverage(functions, generated):
    """{system: count} of the named recompiled functions plus the totals."""
    gen = set(generated) - {f for f, (_, s, _) in functions.items() if s == 'import'}
    by_system = collections.Counter(system_of(n, s) for f, (n, s, _) in functions.items() if f in gen)
    return by_system, sum(by_system.values()), len(gen)


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import rtti_scan
    rtti_scan.main()
