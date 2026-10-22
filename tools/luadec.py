"""Disassembler and decompiler for the game's compiled Lua scripts (.lua / .blua in default.rcf).

    python tools/luadec.py dis FILE [FILE...]          # functions, constants, locals, instructions
    python tools/luadec.py dec FILE [FILE...]          # readable pseudo-Lua
    python tools/luadec.py all [--src DIR] [--out DIR] # every script -> DIR/<path>.dis.txt and
                                                       # DIR/<path>.dec.lua (default
                                                       # private/assets/raw/default -> private/scripts_dec)
    python tools/luadec.py check [FILE...]             # writes private/luadec/check.lua for the
    python tools/run_game.py dev_luadec                # dev_luadec profile: the game's compiler
    python tools/luadec.py check --log RUN.LOG         # compiles every decompiled script; report

The game's scripts are Radical's "Lua 5.1 (work)": the chunk is a Lua 5.0 dump with a 5.1 version
byte, and the VM (luaV_execute = sub_8229E7A0, opcode switch at 0x8229E874) is 5.0 plus FORPREP.

Header (14 bytes): 1B 'Lua' 51, endianness 1 (little), sizeof int/size_t/Instruction/lua_Number
(4 4 4 4), then the 5.0 test number 3.14159265e7 as a float. Numbers are 4-byte floats.
Function (5.0 order): string source (u32 size with the NUL, 0 = none), int linedefined, byte nups,
byte numparams, byte is_vararg, byte maxstacksize, int n + u16 lineinfo[n] (u16, not int), int n +
locvars {string name, int startpc, int endpc}, int n + upvalue names, int n + constants {byte type
(0 nil, 1 boolean + byte, 3 number float, 4 string)}, int n + protos, int n + code (u32).

Instruction (5.0 layout): OP bits 0-5, C bits 6-14, B bits 15-23, A bits 24-31, Bx = bits 6-31,
sBx = Bx - 131071. RK(x) is a constant when x >= 250 (MAXSTACK). Opcodes 0-27 are the 5.0 ones;
then FORLOOP 28, FORPREP 29, TFORLOOP 30, TFORPREP 31, SETLIST 32, SETLISTO 33, CLOSE 34,
CLOSURE 35. The for loops are 5.1 style: the visible loop variables start at A+3 (FORLOOP copies
the index there, TFORLOOP calls R(A)(R(A+1), R(A+2)) into A+3.. and copies A+3 to A+2). SETLIST
stores Bx%32+1 values at index Bx-Bx%32+i. Conditional ops skip the following JMP: EQ/LT/LE jump
when the comparison equals A; TEST A B C jumps (and copies R(B) to R(A)) when R(B)'s truth != C.

The decompiler is structural (if/elseif/else, while, repeat, numeric and generic for, and/or,
table constructors, methods, closures); what it can't structure comes out as `goto pcN` with
`::pcN::` labels, so the output is pseudo-Lua for reading, not for recompiling as is.
"""
import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

OPNAMES = [
    'MOVE', 'LOADK', 'LOADBOOL', 'LOADNIL', 'GETUPVAL', 'GETGLOBAL', 'GETTABLE', 'SETGLOBAL',
    'SETUPVAL', 'SETTABLE', 'NEWTABLE', 'SELF', 'ADD', 'SUB', 'MUL', 'DIV', 'POW', 'UNM', 'NOT',
    'CONCAT', 'JMP', 'EQ', 'LT', 'LE', 'TEST', 'CALL', 'TAILCALL', 'RETURN', 'FORLOOP', 'FORPREP',
    'TFORLOOP', 'TFORPREP', 'SETLIST', 'SETLISTO', 'CLOSE', 'CLOSURE',
]
OP = {n: i for i, n in enumerate(OPNAMES)}
MAXSTACK = 250
NO_REG = 255
MAXARG_SBX = 131071
FIELDS_PER_FLUSH = 32
HEADER = bytes([0x1B, 0x4C, 0x75, 0x61, 0x51, 0x01, 4, 4, 4, 4])
TEST_NUMBER = 3.14159265358979e7


class LuaFormatError(Exception):
    pass


class Instr:
    __slots__ = ('raw', 'op', 'a', 'b', 'c', 'bx', 'sbx')

    def __init__(self, raw):
        self.raw = raw
        self.op = raw & 0x3F
        self.c = (raw >> 6) & 0x1FF
        self.b = (raw >> 15) & 0x1FF
        self.a = raw >> 24
        self.bx = (raw >> 6) & 0x3FFFF
        self.sbx = self.bx - MAXARG_SBX

    @property
    def name(self):
        return OPNAMES[self.op] if self.op < len(OPNAMES) else 'OP%d' % self.op


class LocVar:
    __slots__ = ('name', 'startpc', 'endpc', 'reg')

    def __init__(self, name, startpc, endpc):
        self.name, self.startpc, self.endpc, self.reg = name, startpc, endpc, 0


class Proto:
    def __init__(self):
        self.source = None
        self.linedefined = 0
        self.nups = self.numparams = self.is_vararg = self.maxstack = 0
        self.lineinfo = []
        self.locvars = []
        self.upvalues = []
        self.consts = []
        self.protos = []
        self.code = []

    def line(self, pc):
        return self.lineinfo[pc] if pc < len(self.lineinfo) else 0


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, n):
        if self.pos + n > len(self.data):
            raise LuaFormatError('truncated at 0x%X (need %d bytes)' % (self.pos, n))
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def u8(self):
        return self.take(1)[0]

    def int(self):
        return struct.unpack('<i', self.take(4))[0]

    def count(self, what, limit=1 << 20):
        n = self.int()
        if n < 0 or n > limit:
            raise LuaFormatError('bad %s count %d at 0x%X' % (what, n, self.pos - 4))
        return n

    def string(self):
        n = self.count('string', len(self.data))
        if n == 0:
            return None
        s = self.take(n)
        if s[-1] != 0:
            raise LuaFormatError('string without NUL at 0x%X' % (self.pos - n))
        return s[:-1].decode('latin-1')


def read_function(r, parent_source):
    f = Proto()
    f.source = r.string() or parent_source
    f.linedefined = r.int()
    f.nups, f.numparams, f.is_vararg, f.maxstack = r.take(4)
    f.lineinfo = list(struct.unpack('<%dH' % (n := r.count('lineinfo')), r.take(2 * n)))
    for _ in range(r.count('locvars')):
        name = r.string()
        f.locvars.append(LocVar(name, r.int(), r.int()))
    f.upvalues = [r.string() for _ in range(r.count('upvalues'))]
    for _ in range(r.count('constants')):
        t = r.u8()
        if t == 0:
            f.consts.append(None)
        elif t == 1:
            f.consts.append(bool(r.u8()))
        elif t == 3:
            f.consts.append(struct.unpack('<f', r.take(4))[0])
        elif t == 4:
            f.consts.append(Str(r.string() or ''))
        else:
            raise LuaFormatError('constant type %d at 0x%X' % (t, r.pos - 1))
    f.protos = [read_function(r, f.source) for _ in range(r.count('protos'))]
    n = r.count('code')
    f.code = [Instr(x) for x in struct.unpack('<%dI' % n, r.take(4 * n))]
    assign_registers(f)
    return f


class Str(str):
    """A string constant (to tell it apart from names when formatting)."""


def load(data):
    if data[:10] != HEADER:
        raise LuaFormatError('not a compiled chunk (header %s)' % data[:10].hex())
    test = struct.unpack('<f', data[10:14])[0]
    if abs(test - TEST_NUMBER) > 4:
        raise LuaFormatError('test number %r' % test)
    r = Reader(data)
    r.pos = 14
    f = read_function(r, None)
    if r.pos != len(data):
        raise LuaFormatError('%d trailing bytes' % (len(data) - r.pos))
    return f


def assign_registers(f):
    # Locals live in registers 0..n-1 in declaration order: a local's register is the number of
    # earlier locals still active when it starts.
    for i, lv in enumerate(f.locvars):
        lv.reg = sum(1 for o in f.locvars[:i] if o.startpc <= lv.startpc < o.endpc)


# ---------------------------------------------------------------- formatting helpers

def fmt_number(x):
    if x != x:
        return '(0/0)'
    if x in (float('inf'), float('-inf')):
        return '(1/0)' if x > 0 else '(-1/0)'
    if x == int(x) and abs(x) < 1e15:
        return str(int(x))
    for digits in range(1, 10):
        s = '%.*g' % (digits, x)
        if struct.unpack('<f', struct.pack('<f', float(s)))[0] == x:
            return s
    return repr(x)


def fmt_string(s):
    out = []
    for ch in s:
        o = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == '\\':
            out.append('\\\\')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\r':
            out.append('\\r')
        elif ch == '\t':
            out.append('\\t')
        elif o < 32 or o >= 127:
            out.append('\\%d' % o)
        else:
            out.append(ch)
    return '"' + ''.join(out) + '"'


def fmt_const(k):
    if k is None:
        return 'nil'
    if isinstance(k, bool):
        return 'true' if k else 'false'
    if isinstance(k, Str):
        return fmt_string(k)
    return fmt_number(k)


KEYWORDS = set('and break do else elseif end false for function if in local nil not or repeat '
               'return then true until while'.split())


def is_ident(s):
    return (isinstance(s, str) and s and (s[0].isalpha() or s[0] == '_') and
            all(c.isalnum() or c == '_' for c in s) and s.isascii() and s not in KEYWORDS)


# ---------------------------------------------------------------- disassembler

def disassemble(f, out, path='main'):
    kind = 'main chunk' if path == 'main' else 'function'
    out.append('; %s %s  line %d  params %d%s  upvalues %d  maxstack %d  (%d instructions)' % (
        kind, path, f.linedefined, f.numparams, ' + vararg' if f.is_vararg else '', f.nups,
        f.maxstack, len(f.code)))
    if f.source and path == 'main':
        out.append('; source %s' % f.source)
    if f.consts:
        out.append('; constants (%d)' % len(f.consts))
        for i, k in enumerate(f.consts):
            out.append(';   K%-4d %s' % (i, fmt_const(k)))
    if f.locvars:
        out.append('; locals (%d)' % len(f.locvars))
        for i, lv in enumerate(f.locvars):
            out.append(';   %-4d R%-3d %-24s pc %d-%d' % (i, lv.reg, lv.name, lv.startpc + 1,
                                                          lv.endpc))
    if f.upvalues:
        out.append('; upvalues (%d)' % len(f.upvalues))
        for i, u in enumerate(f.upvalues):
            out.append(';   U%-4d %s' % (i, u))

    def rk(x):
        return 'K%d' % (x - MAXSTACK) if x >= MAXSTACK else 'R%d' % x

    def krk(x):
        return fmt_const(f.consts[x - MAXSTACK]) if x >= MAXSTACK and x - MAXSTACK < len(f.consts) else rk(x)

    def kname(i):
        return fmt_const(f.consts[i]) if i < len(f.consts) else '?K%d' % i

    for pc, ins in enumerate(f.code):
        op, a, b, c, bx, sbx = ins.op, ins.a, ins.b, ins.c, ins.bx, ins.sbx
        name = ins.name
        if op in (OP['LOADK'], OP['GETGLOBAL'], OP['SETGLOBAL'], OP['CLOSURE'], OP['SETLIST'],
                  OP['SETLISTO']):
            args = '%d %d' % (a, bx)
        elif op in (OP['JMP'], OP['FORLOOP'], OP['FORPREP'], OP['TFORPREP']):
            args = ('%d ' % a if op != OP['JMP'] else '') + '%d' % sbx
        else:
            args = '%d %d %d' % (a, b, c)
        cm = ''
        if op == OP['MOVE']:
            cm = 'R%d := R%d' % (a, b)
        elif op == OP['LOADK']:
            cm = 'R%d := %s' % (a, kname(bx))
        elif op == OP['LOADBOOL']:
            cm = 'R%d := %s%s' % (a, 'true' if b else 'false', '; pc++' if c else '')
        elif op == OP['LOADNIL']:
            cm = 'R%d..R%d := nil' % (a, b)
        elif op == OP['GETUPVAL']:
            cm = 'R%d := U%d %s' % (a, b, f.upvalues[b] if b < len(f.upvalues) else '')
        elif op == OP['GETGLOBAL']:
            cm = 'R%d := _G[%s]' % (a, kname(bx))
        elif op == OP['GETTABLE']:
            cm = 'R%d := R%d[%s]' % (a, b, krk(c))
        elif op == OP['SETGLOBAL']:
            cm = '_G[%s] := R%d' % (kname(bx), a)
        elif op == OP['SETUPVAL']:
            cm = 'U%d := R%d' % (b, a)
        elif op == OP['SETTABLE']:
            cm = 'R%d[%s] := %s' % (a, krk(b), krk(c))
        elif op == OP['NEWTABLE']:
            cm = 'R%d := {} (array %d, hash 2^%d)' % (a, b, c)
        elif op == OP['SELF']:
            cm = 'R%d := R%d; R%d := R%d[%s]' % (a + 1, b, a, b, krk(c))
        elif OP['ADD'] <= op <= OP['POW']:
            cm = 'R%d := %s %s %s' % (a, krk(b), '+-*/^'[op - OP['ADD']], krk(c))
        elif op == OP['UNM']:
            cm = 'R%d := -R%d' % (a, b)
        elif op == OP['NOT']:
            cm = 'R%d := not R%d' % (a, b)
        elif op == OP['CONCAT']:
            cm = 'R%d := R%d .. ... .. R%d' % (a, b, c)
        elif op == OP['JMP']:
            cm = 'to %d' % (pc + 2 + sbx)
        elif op == OP['TEST']:
            cm = 'if R%d is %s then pc++ else R%d := R%d' % (b, 'true' if not c else 'false/nil',
                                                              a, b)
        elif op in (OP['CALL'], OP['TAILCALL']):
            args_s = 'R%d..top' % (a + 1) if b == 0 else ('R%d..R%d' % (a + 1, a + b - 1) if b > 1 else '')
            res_s = 'R%d..top' % a if c == 0 else ('R%d..R%d' % (a, a + c - 2) if c > 1 else '')
            cm = '%s%sR%d(%s)' % ('return ' if op == OP['TAILCALL'] else '',
                                  res_s + ' := ' if res_s else '', a, args_s)
        elif op == OP['RETURN']:
            cm = 'return ' + ('R%d..top' % a if b == 0 else ', '.join('R%d' % r for r in range(a, a + b - 1)))
        elif op == OP['FORLOOP']:
            cm = 'R%d += R%d; if R%d <?= R%d then { R%d := R%d; to %d }' % (
                a, a + 2, a, a + 1, a + 3, a, pc + 2 + sbx)
        elif op == OP['FORPREP']:
            cm = 'R%d -= R%d; to %d' % (a, a + 2, pc + 2 + sbx)
        elif op == OP['TFORLOOP']:
            cm = 'R%d..R%d := R%d(R%d, R%d); if R%d ~= nil then R%d := R%d else pc++' % (
                a + 3, a + 2 + c, a, a + 1, a + 2, a + 3, a + 2, a + 3)
        elif op == OP['TFORPREP']:
            cm = 'if R%d is a table then R%d := next, R%d; to %d' % (a, a, a, pc + 2 + sbx)
        elif op in (OP['SETLIST'], OP['SETLISTO']):
            first = bx - bx % FIELDS_PER_FLUSH + 1
            cm = 'R%d[%d..] := R%d..%s' % (a, first, a + 1,
                                          'R%d' % (a + 1 + bx % FIELDS_PER_FLUSH) if op == OP['SETLIST'] else 'top')
        elif op == OP['CLOSE']:
            cm = 'close upvalues >= R%d' % a
        elif op == OP['CLOSURE']:
            cm = 'R%d := closure(F%d)' % (a, bx)
        if op in (OP['EQ'], OP['LT'], OP['LE']):
            sym = {OP['EQ']: '==', OP['LT']: '<', OP['LE']: '<='}[op]
            cm = 'if (%s %s %s) ~= %s then pc++' % (krk(b), sym, krk(c), bool(a) and 'true' or 'false')
        out.append('  [%4d] %-5s %-9s %-14s ; %s' % (pc + 1, '%d' % f.line(pc) if f.lineinfo else '',
                                                    name, args, cm))
    for i, p in enumerate(f.protos):
        out.append('')
        disassemble(p, out, ('' if path == 'main' else path + '.') + 'F%d' % i)
    return out


# ---------------------------------------------------------------- expressions

P_OR, P_AND, P_CMP, P_CONCAT, P_ADD, P_MUL, P_UNARY, P_POW, P_ATOM = 1, 2, 3, 4, 5, 6, 7, 8, 10
BINPREC = {'or': P_OR, 'and': P_AND, '==': P_CMP, '~=': P_CMP, '<': P_CMP, '<=': P_CMP,
           '>': P_CMP, '>=': P_CMP, '..': P_CONCAT, '+': P_ADD, '-': P_ADD, '*': P_MUL, '/': P_MUL,
           '^': P_POW}
RIGHT_ASSOC = {'..', '^'}
NEGATE = {'==': '~=', '~=': '==', '<': '>=', '>=': '<', '<=': '>', '>': '<='}


class Expr:
    def __init__(self, text, prec=P_ATOM, multi=False):
        self.text, self.prec, self.multi = text, prec, multi

    def __str__(self):
        return self.text


class Bin(Expr):
    def __init__(self, op, l, r):
        p = BINPREC[op]
        lt = paren(l, p if op not in RIGHT_ASSOC else p + 1)
        # and/or are associative: a and (b and c) reads as a and b and c
        same = op in ('and', 'or') and isinstance(r, Bin) and r.op == op
        rt = paren(r, p + 1 if op not in RIGHT_ASSOC and not same else p)
        super().__init__('%s %s %s' % (lt, op, rt), p)
        self.op, self.l, self.r = op, l, r


class Not(Expr):
    def __init__(self, e):
        super().__init__('not ' + paren(e, P_UNARY), P_UNARY)
        self.e = e


def paren(e, minprec):
    return e.text if e.prec >= minprec else '(' + e.text + ')'


def negate(e):
    if e.text in ('true', 'false') and not isinstance(e, Not):
        return Expr('false' if e.text == 'true' else 'true')
    if isinstance(e, Not):
        return e.e
    if isinstance(e, Bin) and e.op in NEGATE:
        return Bin(NEGATE[e.op], e.l, e.r)
    return Not(e)


class TableExpr(Expr):
    def __init__(self):
        super().__init__('{}')
        self.items = []    # (key or None, value)

    def render(self):
        if not self.items:
            return '{}'
        parts = []
        for k, v in self.items:
            if k is None:
                parts.append(v.text)
            elif isinstance(k, Expr) and k.text.startswith('"') and is_ident(k.raw if hasattr(k, 'raw') else None):
                parts.append('%s = %s' % (k.raw, v.text))
            else:
                parts.append('[%s] = %s' % (k.text, v.text))
        one = '{' + ', '.join(parts) + '}'
        if len(one) <= 100 and '\n' not in one:
            return one
        return '{\n' + ',\n'.join(indent_text(p, 1) for p in parts) + '\n}'

    def finish(self):
        self.text = self.render()
        return self


class KExpr(Expr):
    def __init__(self, k):
        super().__init__(fmt_const(k))
        self.raw = k


class MethodRef:
    def __init__(self, obj, name):
        self.obj, self.name = obj, name


class FuncExpr(Expr):
    def __init__(self, lines):
        super().__init__('\n'.join(lines))
        self.lines = lines


class MultiResult:
    def __init__(self, call, index, count):
        self.call, self.index, self.count = call, index, count


def indent_text(text, n):
    pad = '    ' * n
    return '\n'.join(pad + l if l else l for l in text.split('\n'))


# ---------------------------------------------------------------- statements

class Stmt:
    pass


class Line(Stmt):
    def __init__(self, text):
        self.text = text


class If(Stmt):
    def __init__(self, cond, then, other):
        self.cond, self.then, self.other = cond, then, other


class Loop(Stmt):
    def __init__(self, head, body, tail='end'):
        self.head, self.body, self.tail = head, body, tail


def render(stmts, level, out):
    pad = '    ' * level
    for i, s in enumerate(stmts):
        if isinstance(s, Line):
            text = s.text
            if (text == 'break' or text == 'return' or text.startswith('return ')) and any(
                    not (isinstance(o, Line) and o.text.startswith('--')) for o in stmts[i + 1:]):
                text = 'do %s end' % text   # Lua only allows return/break last in a block
            out.append(indent_text(text, level))
        elif isinstance(s, If):
            out.append(indent_text('if %s then' % s.cond.text, level))
            render(s.then, level + 1, out)
            other = s.other
            while other:
                if len(other) == 1 and isinstance(other[0], If):
                    out.append(indent_text('elseif %s then' % other[0].cond.text, level))
                    render(other[0].then, level + 1, out)
                    other = other[0].other
                else:
                    out.append(pad + 'else')
                    render(other, level + 1, out)
                    break
            out.append(pad + 'end')
        elif isinstance(s, Loop):
            out.append(indent_text(s.head, level))
            render(s.body, level + 1, out)
            out.append(indent_text(s.tail, level))
    return out


# ---------------------------------------------------------------- decompiler

class Unstructured(Exception):
    pass


class Branch:
    __slots__ = ('start', 'test', 'jmp', 'target', 'ins')

    def __init__(self, start, test, target, ins):
        self.start, self.test, self.jmp, self.target, self.ins = start, test, test + 1, target, ins


class FuncDecompiler:
    def __init__(self, f, upnames=None):
        self.f = f
        self.code = f.code
        self.regs = {}
        self.labels = set()
        self.loops = []        # stack of break targets
        self.upnames = f.upvalues or upnames or ['U%d' % i for i in range(f.nups)]
        self.declared = set()  # locvar indices already declared
        self.by_reg = {}
        for lv in f.locvars:
            self.by_reg.setdefault(lv.reg, []).append(lv)
        self.starts = {lv.startpc for lv in f.locvars}
        self.by_start = {}
        for i, lv in enumerate(f.locvars):
            if i >= f.numparams:
                self.by_start.setdefault(lv.startpc, []).append(lv)
        self.back = {}         # target pc -> JMPs jumping back to it
        for j, ins in enumerate(self.code):
            if ins.op == OP['JMP'] and ins.sbx < 0:
                self.back.setdefault(j + 1 + ins.sbx, []).append(j)

    # -- registers and locals

    def local_at(self, r, pc):
        for lv in self.by_reg.get(r, ()):
            if lv.startpc <= pc < lv.endpc:
                return lv
        return None

    def read(self, r, pc):
        lv = self.local_at(r, pc)
        if lv is not None:
            self.regs.pop(r, None)
            return Expr(lv.name)
        e = self.regs.pop(r, None)
        if e is None:
            return Expr('R%d' % r)
        if isinstance(e, MultiResult):
            return Expr('R%d' % r)
        if isinstance(e, TableExpr):
            e.finish()
        return e

    def rk(self, x, pc):
        if x >= MAXSTACK:
            return KExpr(self.f.consts[x - MAXSTACK])
        return self.read(x, pc)

    def write(self, r, e, pc, out):
        lv = self.local_at(r, pc)
        if lv is not None and lv.startpc != pc + 1:
            if isinstance(e, FuncExpr) and lv.startpc == pc:
                out.append(Line('local ' + func_header(lv.name, e)))
                self.declared.add(id(lv))
                return
            if isinstance(e, TableExpr):
                e.finish()
            out.append(Line('%s = %s' % (lv.name, e.text)))
        else:
            self.regs[r] = e

    def declare_locals(self, pc, out):
        news = [lv for i, lv in enumerate(self.f.locvars)
                if lv.startpc == pc and id(lv) not in self.declared and i >= self.f.numparams]
        if not news:
            return
        for lv in news:
            self.declared.add(id(lv))
        news = [lv for lv in news if not lv.name.startswith('(') and not (
            self.f.is_vararg and lv.name == 'arg' and lv.reg == self.f.numparams)]
        if not news:
            return
        news.sort(key=lambda lv: lv.reg)
        if self.code[pc].op == OP['CLOSURE'] and self.code[pc].a == news[0].reg and len(news) == 1:
            self.declared.discard(id(news[0]))   # local function: written by the CLOSURE
            return
        names, vals = [], []
        multi = None
        for lv in news:
            names.append(lv.name)
            e = self.regs.pop(lv.reg, None)
            if isinstance(e, MultiResult):
                if multi is None or multi is not e.call:
                    multi = e.call
                    vals.append(e.call)
                continue
            if e is None:
                continue
            if isinstance(e, TableExpr):
                e.finish()
            vals.append(e)
        while vals and vals[-1].text == 'nil' and len(vals) > 0 and all(v.text == 'nil' for v in vals):
            vals.pop()
        if vals:
            if len(vals) == 1 and isinstance(vals[0], FuncExpr) and len(names) == 1:
                out.append(Line('local ' + func_header(names[0], vals[0])))
            else:
                out.append(Line('local %s = %s' % (', '.join(names), ', '.join(v.text for v in vals))))
        else:
            out.append(Line('local ' + ', '.join(names)))

    # -- single instructions

    def index(self, obj, key):
        if isinstance(key, KExpr) and isinstance(key.raw, Str) and is_ident(key.raw):
            return Expr('%s.%s' % (paren(obj, P_ATOM) if not obj.text.startswith('{') else '(' + obj.text + ')', key.raw))
        return Expr('%s[%s]' % (paren(obj, P_ATOM) if not obj.text.startswith('{') else '(' + obj.text + ')', key.text))

    def global_name(self, k):
        if isinstance(k, Str) and is_ident(k):
            return Expr(str(k))
        return Expr('_G[%s]' % fmt_const(k))

    def call_args(self, a, b, pc):
        if b == 0:
            regs = []
            r = a + 1
            while r in self.regs and not isinstance(self.regs[r], MultiResult):
                regs.append(r)
                r += 1
            n = len(regs)
        else:
            n = b - 1
        args = [self.read(a + 1 + i, pc) for i in range(n)]
        return args

    def make_call(self, a, b, pc):
        func = self.regs.pop(a, None) if self.local_at(a, pc) is None else None
        if func is None:
            func = self.read(a, pc)
        args = self.call_args(a, b, pc)
        if isinstance(func, MethodRef):
            text = '%s:%s(%s)' % (paren(func.obj, P_ATOM), func.name,
                                  ', '.join(x.text for x in args[1:]))
        else:
            ft = func.text
            if func.prec != P_ATOM or isinstance(func, (FuncExpr, TableExpr)):
                ft = '(' + ft + ')'
            text = '%s(%s)' % (ft, ', '.join(x.text for x in args))
        return Expr(text, P_ATOM, multi=True)

    def step(self, pc, out):
        """Simulates one straight-line instruction. Returns the next pc."""
        f = self.f
        ins = self.code[pc]
        op, a, b, c = ins.op, ins.a, ins.b, ins.c
        if op == OP['MOVE']:
            self.write(a, self.read(b, pc), pc, out)
        elif op == OP['LOADK']:
            self.write(a, KExpr(f.consts[ins.bx]), pc, out)
        elif op == OP['LOADBOOL']:
            self.write(a, Expr('true' if b else 'false'), pc, out)
            if c:
                return pc + 2
        elif op == OP['LOADNIL']:
            for r in range(a, b + 1):
                self.write(r, Expr('nil'), pc, out)
        elif op == OP['GETUPVAL']:
            self.write(a, Expr(self.upnames[b] if b < len(self.upnames) else 'U%d' % b), pc, out)
        elif op == OP['GETGLOBAL']:
            self.write(a, self.global_name(f.consts[ins.bx]), pc, out)
        elif op == OP['GETTABLE']:
            key = self.rk(c, pc)
            obj = self.read(b, pc)
            self.write(a, self.index(obj, key), pc, out)
        elif op == OP['SETGLOBAL']:
            v = self.read(a, pc)
            name = self.global_name(f.consts[ins.bx]).text
            out.append(Line(assign_text(name, v)))
        elif op == OP['SETUPVAL']:
            v = self.read(a, pc)
            out.append(Line(assign_text(self.upnames[b] if b < len(self.upnames) else 'U%d' % b, v)))
        elif op == OP['SETTABLE']:
            val = self.rk(c, pc)
            key = self.rk(b, pc)
            t = self.regs.get(a) if self.local_at(a, pc) is None else None
            if isinstance(t, TableExpr):
                if isinstance(val, TableExpr):
                    val.finish()
                t.items.append((key, val))
            else:
                obj = self.read(a, pc)
                target = self.index(obj, key).text
                out.append(Line(assign_text(target, val, method=True)))
        elif op == OP['NEWTABLE']:
            self.write(a, TableExpr(), pc, out)
        elif op == OP['SELF']:
            key = self.rk(c, pc)
            obj = self.read(b, pc)
            name = key.raw if isinstance(key, KExpr) and is_ident(key.raw) else None
            if name is None:
                self.write(a, self.index(obj, key), pc, out)
                self.write(a + 1, obj, pc, out)
            else:
                self.regs[a] = MethodRef(obj, name)
                self.regs[a + 1] = Expr('<self>')
        elif OP['ADD'] <= op <= OP['POW']:
            r_ = self.rk(c, pc)
            l_ = self.rk(b, pc)
            self.write(a, Bin('+-*/^'[op - OP['ADD']], l_, r_), pc, out)
        elif op == OP['UNM']:
            self.write(a, Expr('-' + paren(self.read(b, pc), P_UNARY), P_UNARY), pc, out)
        elif op == OP['NOT']:
            self.write(a, Not(self.read(b, pc)), pc, out)
        elif op == OP['CONCAT']:
            parts = [self.read(r, pc) for r in range(b, c + 1)]
            e = parts[-1]
            for p in reversed(parts[:-1]):
                e = Bin('..', p, e)
            self.write(a, e, pc, out)
        elif op == OP['CALL']:
            call = self.make_call(a, b, pc)
            if c == 1:
                out.append(Line(call.text))
            elif c == 2:
                call.multi = False
                self.write(a, call, pc, out)
            elif c == 0:
                self.regs[a] = call
            else:
                n = c - 1
                regs = list(range(a, a + n))
                if all(self.local_at(r, pc + 1) is not None and self.local_at(r, pc + 1).startpc == pc + 1
                       for r in regs):
                    for i, r in enumerate(regs):
                        self.regs[r] = MultiResult(call, i, n)
                elif all(self.local_at(r, pc) is None for r in regs) and self.loop_regs_follow(pc, a):
                    for i, r in enumerate(regs):
                        self.regs[r] = MultiResult(call, i, n)
                else:
                    names = ['_r%d' % r for r in regs]
                    out.append(Line('local %s = %s' % (', '.join(names), call.text)))
                    for r, nm in zip(regs, names):
                        self.write(r, Expr(nm), pc, out)
        elif op == OP['TAILCALL']:
            call = self.make_call(a, b, pc)
            out.append(Line('return ' + call.text))
            if pc + 1 < len(self.code) and self.code[pc + 1].op == OP['RETURN']:
                return pc + 2
        elif op == OP['RETURN']:
            if b == 0:
                vals = []
                r = a
                while r in self.regs or self.local_at(r, pc) is not None:
                    vals.append(self.read(r, pc))
                    r += 1
                    if vals[-1].multi:
                        break
            else:
                vals = [self.read(r, pc) for r in range(a, a + b - 1)]
            if not (pc == len(self.code) - 1 and not vals):
                out.append(Line('return' + (' ' + ', '.join(v.text for v in vals) if vals else '')))
        elif op in (OP['SETLIST'], OP['SETLISTO']):
            t = self.regs.get(a)
            if op == OP['SETLIST']:
                n = ins.bx % FIELDS_PER_FLUSH + 1
            else:
                n = 0
                while (a + 1 + n) in self.regs:
                    n += 1
            vals = [self.read(a + 1 + i, pc) for i in range(n)]
            if isinstance(t, TableExpr):
                for v in vals:
                    if isinstance(v, TableExpr):
                        v.finish()
                    t.items.append((None, v))
            else:
                out.append(Line('-- SETLIST R%d: %s' % (a, ', '.join(v.text for v in vals))))
        elif op == OP['CLOSE']:
            pass
        elif op == OP['CLOSURE']:
            p = f.protos[ins.bx]
            names = []
            for i in range(p.nups):
                pi = self.code[pc + 1 + i]
                if pi.op == OP['GETUPVAL']:
                    names.append(self.upnames[pi.b] if pi.b < len(self.upnames) else 'U%d' % pi.b)
                else:
                    lv = self.local_at(pi.b, pc)
                    names.append(lv.name if lv else 'R%d' % pi.b)
            sub = FuncDecompiler(p, names)
            fe = sub.closure()
            self.write(a, fe, pc, out)
            return pc + 1 + p.nups
        else:
            raise Unstructured('unexpected %s at %d' % (ins.name, pc + 1))
        return pc + 1

    def loop_regs_follow(self, pc, a):
        n = pc + 1
        return n < len(self.code) and self.code[n].op == OP['TFORPREP'] and self.code[n].a == a

    # -- conditions

    def is_test(self, pc):
        return (pc + 1 < len(self.code) and self.code[pc].op in (OP['EQ'], OP['LT'], OP['LE'], OP['TEST'])
                and self.code[pc + 1].op == OP['JMP'])

    def branch_cond(self, pc):
        """Expression that is true when the branch at pc jumps."""
        ins = self.code[pc]
        if ins.op == OP['TEST']:
            v = self.read(ins.b, pc)
            return v if ins.c else negate(v)
        r_ = self.rk(ins.c, pc)
        l_ = self.rk(ins.b, pc)
        sym = {OP['EQ']: '==', OP['LT']: '<', OP['LE']: '<='}[ins.op]
        if sym != '==' and isinstance(l_, KExpr) and not isinstance(r_, KExpr):
            # `x > 1` compiles to LT 1 x
            e = Bin('>' if sym == '<' else '>=', r_, l_)
        else:
            e = Bin(sym, l_, r_)
        return e if ins.a else negate(e)

    def pure(self, start, end, dest=None):
        """True when [start, end) only computes temporaries (can be part of an expression); dest
        is a register that may be written even if it is a local."""
        pc = start
        while pc < end:
            ins = self.code[pc]
            op = ins.op
            if op in (OP['SETGLOBAL'], OP['SETUPVAL'], OP['JMP'], OP['RETURN'], OP['TAILCALL'],
                      OP['FORPREP'], OP['FORLOOP'], OP['TFORPREP'], OP['TFORLOOP'], OP['CLOSE'],
                      OP['EQ'], OP['LT'], OP['LE'], OP['TEST']):
                return False
            if op == OP['CALL'] and ins.c == 1:
                return False
            if pc + 1 in self.starts and pc + 1 < end:
                return False
            if op not in (OP['SETTABLE'], OP['SETLIST'], OP['SETLISTO']) and ins.a != dest and \
                    self.local_at(ins.a, pc) is not None:
                return False
            if op in (OP['SETTABLE'], OP['SETLIST'], OP['SETLISTO']) and self.local_at(ins.a, pc) is not None:
                return False
            if op == OP['LOADNIL'] and any(self.local_at(r, pc) for r in range(ins.a, ins.b + 1)):
                return False
            if op == OP['LOADBOOL'] and ins.c:
                return False
            if op == OP['CLOSURE']:
                pc += self.f.protos[ins.bx].nups
            pc += 1
        return True

    def collect_branches(self, pc, limit, value=False):
        """Branches (test + JMP) starting at pc, each following segment pure, up to limit. With
        value, segments may write the register the first TEST copies into."""
        dest = self.code[pc].a if value else None
        brs = [Branch(pc, pc, pc + 2 + self.code[pc + 1].sbx, self.code[pc])]
        p = pc + 2
        while p < limit:
            q = p
            while q < limit and not self.is_test(q):
                if self.code[q].op in (OP['JMP'], OP['RETURN'], OP['TAILCALL'], OP['FORPREP'],
                                       OP['FORLOOP'], OP['TFORPREP'], OP['TFORLOOP']):
                    break
                q += 1
            if q == p and q < limit and self.code[q].op == OP['JMP'] and self.code[q].sbx > 0:
                # an unconditional jump right after a branch: a constant condition (`or false`) or
                # an empty then-block; taken as a branch that always jumps
                brs.append(Branch(p, p - 1, p + 1 + self.code[p].sbx, None))
                p += 1
                continue
            if q >= limit or not self.is_test(q) or not self.pure(p, q, dest):
                break
            brs.append(Branch(p, q, q + 2 + self.code[q + 1].sbx, self.code[q]))
            p = q + 2
        return brs

    @staticmethod
    def chain_ok(brs, m, T, F):
        starts = {b.start: i for i, b in enumerate(brs[:m + 1])}
        for i, b in enumerate(brs[:m + 1]):
            t = b.target
            if t == T or t == F:
                continue
            k = starts.get(t)
            if k is None or k <= i:
                return False
        return True

    def pick_chain(self, brs, accept):
        """Longest prefix of brs whose targets stay inside it; accept(T, F, m) filters."""
        for m in range(len(brs) - 1, -1, -1):
            T = brs[m].jmp + 1
            F = brs[m].target
            if self.chain_ok(brs, m, T, F) and accept(T, F, m):
                return m
        return None

    def eval_conds(self, brs, m):
        """Builds each branch's jump condition, simulating the segments between them."""
        conds = []
        for i, b in enumerate(brs[:m + 1]):
            if i > 0:
                pc = b.start
                junk = []
                while pc < b.test:
                    pc = self.step(pc, junk)
                if junk:
                    raise Unstructured('statement inside condition at %d' % (b.start + 1))
            conds.append(self.branch_cond(b.test) if b.ins is not None else Expr('true'))
        return conds

    def vbuild(self, brs, conds, lo, hi, ends, final):
        """Expression for branches lo..hi-1. ends maps jump targets to their meaning: 'T' (leave
        true), 'F' (leave false) or 'L' (leave with the tested value: TEST C=1 is an 'or', C=0 an
        'and'). After the last branch comes `final` (a value), or None when the last branch's own
        operand is the result (a plain condition)."""
        if lo == hi:
            return final

        def kind_of(i, t, ends):
            k = ends.get(t)
            if k == 'L':
                ins = brs[i].ins
                if ins is None or ins.op != OP['TEST']:
                    raise Unstructured('comparison leaving with a value at %d' % (brs[i].test + 1))
                k = 'T' if ins.c else 'F'
            return k

        def operand(i, kind):
            return conds[i] if kind == 'T' else negate(conds[i])

        t = brs[lo].target
        kind = kind_of(lo, t, ends)
        if kind:
            x = operand(lo, kind)
            if lo == hi - 1 and final is None:
                return x
            rest = self.vbuild(brs, conds, lo + 1, hi, ends, final)
            if final is None and rest.text == ('false' if kind == 'T' else 'true'):
                return x
            return Bin('or' if kind == 'T' else 'and', x, rest)
        k = next((i for i in range(lo + 1, hi) if brs[i].start == t), None)
        if k is None:
            raise Unstructured('branch into the middle of a condition at %d' % (brs[lo].test + 1))
        # branches lo..k-1 form a group; the last of them decides how it joins the rest
        last_kind = kind_of(k - 1, brs[k - 1].target, ends)
        if not last_kind:
            raise Unstructured('nested condition groups at %d' % (brs[lo].test + 1))
        inner = dict(ends)
        inner[t] = 'F' if last_kind == 'T' else 'T'
        group = self.vbuild(brs, conds, lo, k - 1, inner, operand(k - 1, last_kind))
        if k == hi and final is None:
            return group
        return Bin('or' if last_kind == 'T' else 'and', group,
                   self.vbuild(brs, conds, k, hi, ends, final))

    def condition(self, brs, m, T, F):
        conds = self.eval_conds(brs, m)
        return self.vbuild(brs[:m + 1], conds, 0, m + 1, {T: 'T', F: 'F'}, None)

    # -- value expressions: a and b, a or b, comparisons into a register

    def is_loadbool_pair(self, pc):
        if pc < 0 or pc + 1 >= len(self.code):
            return False
        lf, lt = self.code[pc], self.code[pc + 1]
        return (lf.op == OP['LOADBOOL'] and lf.b == 0 and lf.c == 1 and lt.op == OP['LOADBOOL'] and
                lt.b == 1 and lt.c == 0 and lt.a == lf.a)

    def writes_reg_last(self, start, end, reg):
        last = None
        pc = start
        while pc < end:
            ins = self.code[pc]
            if ins.op not in (OP['SETTABLE'], OP['SETLIST'], OP['SETLISTO']):
                last = ins.a
            if ins.op == OP['CLOSURE']:
                pc += self.f.protos[ins.bx].nups
            pc += 1
        return last == reg

    def try_value_expr(self, pc, end, out):
        """`a and b`, `a or b`, `x == y` and mixes of them computed into one register. Returns the
        pc after the expression, or None."""
        brs = self.collect_branches(pc, end, value=True)
        for m in range(len(brs) - 1, -1, -1):
            r = self.value_shape(brs, m, end)
            if r is None:
                continue
            dest, ends, final_start, final_end, L = r
            saved = dict(self.regs)
            try:
                conds = self.eval_conds(brs, m)
                final = None
                if final_end > final_start:
                    p, junk = final_start, []
                    while p < final_end:
                        p = self.step(p, junk)
                    if junk:
                        raise Unstructured('statement in value at %d' % (final_start + 1))
                    final = self.regs.pop(dest, None) or Expr('R%d' % dest)
                e = self.vbuild(brs[:m + 1], conds, 0, m + 1, ends, final)
            except Unstructured:
                self.regs = saved
                continue
            self.write(dest, e, L - 1, out)
            return L
        return None

    def value_shape(self, brs, m, end):
        """(dest, ends, final segment start/end, join pc) when brs[:m+1] compute a value."""
        if any(b.ins is None for b in brs[:m + 1]):
            return None
        last_end = brs[m].jmp + 1
        tests = [b for b in brs[:m + 1] if b.ins.op == OP['TEST'] and b.ins.a != b.ins.b]
        starts = {b.start for b in brs[:m + 1]}
        # with a LOADBOOL pair: jumps go to false (Lf), true (Lf+1) or the join (Lf+2)
        for b in brs[:m + 1]:
            for Lf in (b.target, b.target - 1, b.target - 2):
                if Lf < last_end or not self.is_loadbool_pair(Lf):
                    continue
                L = Lf + 2
                dest = self.code[Lf].a
                if L > end:
                    continue
                if Lf == last_end:
                    fs, fe = last_end, last_end
                else:
                    j = Lf - 1
                    if not (self.code[j].op == OP['JMP'] and j + 1 + self.code[j].sbx == L):
                        continue
                    fs, fe = last_end, j
                    if not (self.pure(fs, fe, dest) and self.writes_reg_last(fs, fe, dest)):
                        continue
                ends = {Lf: 'F', Lf + 1: 'T', L: 'L'}
                if all(x.target in ends or x.target in starts for x in brs[:m + 1]) and \
                        all(x.ins.a == dest for x in brs[:m + 1]
                            if x.target == L) and (fe > fs or brs[m].target != Lf):
                    if fe == fs and brs[m].target == L:
                        continue
                    return dest, ends, fs, fe, L
        # plain and/or: TESTs that copy the tested value to dest and jump to the join
        if not tests and not all(b.ins.op == OP['TEST'] for b in brs[:m + 1]):
            return None
        dest = brs[0].ins.a
        if not all(b.ins.op == OP['TEST'] and b.ins.a == dest for b in brs[:m + 1]):
            return None
        if self.local_at(dest, brs[0].test) is not None and not tests:
            return None    # if x then x = ... end reads better as a statement
        L = max(b.target for b in brs[:m + 1])
        if L <= last_end or L > end or not self.pure(last_end, L, dest) or \
                not self.writes_reg_last(last_end, L, dest):
            return None
        if not all(b.target == L or b.target in starts for b in brs[:m + 1]):
            return None
        return dest, {L: 'L'}, last_end, L, L

    # -- blocks

    def block(self, start, end, exits, out):
        """Decompiles [start, end). exits = pcs that mean 'fall off the end of this block'."""
        pc = start
        scopes = []    # (local declared in this block, index in out of its declaration)
        last = len(self.code) - 1
        while pc < end:
            # locals of this block whose scope ends before the block does were in a do ... end
            # (only the implicit final return comes after the main chunk's locals, and a block
            # whose locals are upvalues ends with CLOSE)
            ending = [i for lv, i in scopes if lv.endpc <= pc]
            if ending and not (pc == last and self.code[pc].op == OP['RETURN']) and                     not all(self.code[q].op == OP['CLOSE'] for q in range(pc, end)):
                idx = min(ending)
                body = out[idx:]
                del out[idx:]
                out.append(Loop('do', body))
                scopes = [(lv, i) for lv, i in scopes if i < idx]
            if pc in self.labels:
                out.append(Line('::pc%d::' % (pc + 1)))
            n = len(out)
            scopes += [(lv, n) for lv in self.by_start.get(pc, ())
                       if lv.endpc > lv.startpc and not lv.name.startswith('(')]
            self.declare_locals(pc, out)
            pc = self.statement(pc, end, exits, out)
        return out

    def back_jumps_to(self, pc, end):
        """Last instruction in (pc, end) that jumps back to pc (loop latch), or None."""
        js = [j for j in self.back.get(pc, ()) if j < end]
        return max(js) if js else None

    def statement(self, pc, end, exits, out):
        ins = self.code[pc]
        op = ins.op
        # loops whose latch jumps back here
        j = self.back_jumps_to(pc, end)
        if j is not None and not (j > 0 and self.code[j - 1].op == OP['TFORLOOP']):
            return self.loop_at(pc, j, end, exits, out)
        if op == OP['FORPREP']:
            q = pc + 1 + ins.sbx
            a = ins.a
            init, limit, stepv = (self.regs.pop(r, None) or Expr('R%d' % r) for r in (a, a + 1, a + 2))
            lv = self.local_at(a + 3, pc + 1)
            var = lv.name if lv else 'R%d' % (a + 3)
            if lv:
                self.declared.add(id(lv))
            head = 'for %s = %s, %s%s do' % (var, init.text, limit.text,
                                             '' if stepv.text == '1' else ', ' + stepv.text)
            body = self.loop_body(pc + 1, q, {q}, q + 1)
            out.append(Loop(head, body))
            return q + 1
        if op == OP['TFORPREP']:
            q = pc + 1 + ins.sbx
            a = ins.a
            tl = self.code[q]
            gen = self.regs.get(a)
            if isinstance(gen, MultiResult):
                for r in (a, a + 1, a + 2):
                    self.regs.pop(r, None)
                explist = gen.call.text
            else:
                parts = [self.regs.pop(r, None) or Expr('nil') for r in (a, a + 1, a + 2)]
                while len(parts) > 1 and parts[-1].text == 'nil':
                    parts.pop()
                explist = ', '.join(p.text for p in parts)
            names = []
            for r in range(a + 3, a + 3 + max(tl.c, 1)):
                lv = self.local_at(r, pc + 1)
                names.append(lv.name if lv else 'R%d' % r)
                if lv:
                    self.declared.add(id(lv))
            head = 'for %s in %s do' % (', '.join(names), explist)
            body = self.loop_body(pc + 1, q, {q}, q + 2)
            out.append(Loop(head, body))
            return q + 2
        if op == OP['JMP']:
            t = pc + 1 + ins.sbx
            if self.loops and t == self.loops[-1]:
                out.append(Line('break'))
                return pc + 1
            if t in exits:
                return pc + 1
            if t > pc + 1 and t < len(self.code):
                # 5.0 while: jump to the condition at the bottom
                w = self.while50(pc, t, end, exits, out)
                if w is not None:
                    return w
            if pc + 1 < t <= end:
                # a jump over code: what `if false then ... [else ...] end` compiles to
                j = t - 1
                E = j + 1 + self.code[j].sbx if self.code[j].op == OP['JMP'] else None
                if E is not None and t < E <= end and not (self.loops and E == self.loops[-1]):
                    then = self.block(pc + 1, j, {j, E}, [])
                    other = self.block(t, E, {E} | (exits if E == end else set()), [])
                    out.append(If(Expr('false'), then, other or None))
                    return E
                out.append(If(Expr('false'), self.block(pc + 1, t, {t}, []), None))
                return t
            self.labels.add(t)
            out.append(Line('goto pc%d' % (t + 1)))
            return pc + 1
        if self.is_test(pc) and self.code[pc + 1].sbx == 0:
            # both ways lead to the next instruction (an if whose exits were patched away)
            out.append(If(self.branch_cond(pc), [], None))
            return pc + 2
        if self.is_test(pc):
            v = self.try_value_expr(pc, end, out)
            if v is not None:
                return v
            return self.if_at(pc, end, exits, out)
        if op in (OP['EQ'], OP['LT'], OP['LE'], OP['TEST']):
            raise Unstructured('test without JMP at %d' % (pc + 1))
        return self.step(pc, out)

    def loop_body(self, start, end, exits, brk):
        self.loops.append(brk)
        try:
            return self.block(start, end, exits, [])
        finally:
            self.loops.pop()

    def loop_at(self, pc, j, end, exits, out):
        latch = self.code[j]
        if j > pc and self.code[j - 1].op in (OP['EQ'], OP['LT'], OP['LE'], OP['TEST']):
            # repeat ... until cond: the condition's false edge is the latch back to pc
            brs_start = self.cond_start(pc, j - 1)
            body = self.loop_body(pc, brs_start, {brs_start}, j + 1)
            brs = self.collect_branches(brs_start, j + 1)
            m = len(brs) - 1
            while m >= 0 and brs[m].jmp != j:
                m -= 1
            if m < 0:
                raise Unstructured('repeat condition at %d' % (j + 1))
            cond = self.condition(brs, m, j + 1, pc)
            out.append(Loop('repeat', body, 'until ' + cond.text))
            return j + 1
        # while cond do ... end (5.1 style: condition on top, latch at j)
        if self.is_test(pc) or self.cond_head(pc, j):
            brs = self.collect_branches(self.first_test(pc, j), j)
            m = self.pick_chain(brs, lambda T, F, m: F == j + 1)
            if m is not None:
                tmp = []
                p = pc
                while p < brs[0].test:
                    p = self.step(p, tmp)
                if not tmp:
                    T = brs[m].jmp + 1
                    cond = self.condition(brs, m, T, j + 1)
                    body = self.loop_body(T, j, {j, pc}, j + 1)
                    out.append(Loop('while %s do' % cond.text, body))
                    return j + 1
        body = self.loop_body(pc, j, {j, pc}, j + 1)
        out.append(Loop('while true do', body))
        return j + 1

    def first_test(self, pc, j):
        p = pc
        while p < j and not self.is_test(p):
            p += 1
        return p

    def cond_head(self, pc, j):
        p = self.first_test(pc, j)
        return p < j and self.pure(pc, p)

    def cond_start(self, body_start, test_pc):
        # walk back over the pure code feeding the last test
        p = test_pc
        while p > body_start and self.pure(p - 1, test_pc) and not (
                self.code[p - 1].op == OP['JMP']):
            p -= 1
        # earlier branches of the same chain
        while p - 2 >= body_start and self.is_test(p - 2):
            p -= 2
            while p > body_start and self.pure(p - 1, p) and self.code[p - 1].op != OP['JMP']:
                p -= 1
        return p

    def while50(self, pc, c, end, exits, out):
        # JMP c; body; c: condition jumping back to pc+1 when true
        start = pc + 1
        k = c
        while k < len(self.code) and not self.is_test(k) and self.code[k].op != OP['JMP']:
            k += 1
        if k >= len(self.code):
            return None
        if self.code[k].op == OP['JMP'] and k + 1 + self.code[k].sbx == start and self.pure(c, k):
            if c != k:
                return None
            body = self.loop_body(start, c, {c}, k + 1)
            out.append(Loop('while true do', body))
            return k + 1
        if not self.is_test(k):
            return None
        brs = self.collect_branches(k, len(self.code))
        m = None
        for i in range(len(brs) - 1, -1, -1):
            if brs[i].target == start and self.chain_ok(brs, i, start, brs[i].jmp + 1):
                m = i
                break
        if m is None:
            return None
        exit_pc = brs[m].jmp + 1
        body = self.loop_body(start, c, {c}, exit_pc)
        saved = dict(self.regs)
        p = c
        junk = []
        while p < k:
            p = self.step(p, junk)
        cond = self.condition(brs, m, start, exit_pc)
        out.extend(junk)
        out.append(Loop('while %s do' % cond.text, body))
        return exit_pc

    def if_at(self, pc, end, exits, out):
        brs = self.collect_branches(pc, end)

        def accept(T, F, m):
            return (T <= F <= end) or F in exits or (self.loops and F == self.loops[-1])

        m = self.pick_chain(brs, accept)
        if m is None:
            cond = self.branch_cond(pc)
            t = pc + 2 + self.code[pc + 1].sbx
            if self.loops and t == self.loops[-1]:
                out.append(If(cond, [Line('break')], None))
            else:
                self.labels.add(t)
                out.append(If(cond, [Line('goto pc%d' % (t + 1))], None))
            return pc + 2
        T = brs[m].jmp + 1
        F = brs[m].target
        if self.loops and F == self.loops[-1] and not (T <= F <= end):
            # if not cond then break end
            cond = self.condition(brs, m, T, F)
            out.append(If(negate(cond), [Line('break')], None))
            return T
        cond = self.condition(brs, m, T, F)
        if F in exits and not (T <= F <= end):
            then = self.block(T, end, exits, [])
            out.append(If(cond, then, None))
            return end
        # else part: the then block ends with a JMP over it
        if F - 1 > T and self.code[F - 1].op == OP['JMP']:
            E = F + self.code[F - 1].sbx
            brk = self.loops and E == self.loops[-1]
            if not brk and (E > F and (E <= end or E in exits)):
                Ee = min(E, end)
                then = self.block(T, F - 1, {F - 1, E} | (exits if Ee == end else set()), [])
                other = self.block(F, Ee, {E} | (exits if Ee == end else set()), [])
                out.append(If(cond, then, other or None))
                return Ee
            if not brk and E in exits:
                then = self.block(T, F - 1, {F - 1, E} | exits, [])
                other = self.block(F, end, exits, [])
                out.append(If(cond, then, other or None))
                return end
        then = self.block(T, F, {F}, [])
        out.append(If(cond, then, None))
        return F

    # -- whole function

    def params(self):
        names = [lv.name for lv in self.f.locvars[:self.f.numparams]]
        names += ['R%d' % i for i in range(len(names), self.f.numparams)]
        if self.f.is_vararg:
            names.append('...')
        return names

    def body_lines(self):
        stmts = self.block(0, len(self.code), {len(self.code)}, [])
        for r, e in sorted(self.regs.items()):
            if isinstance(e, Expr):
                stmts.append(Line('-- R%d left as %s' % (r, e.text.split('\n')[0])))
        return render(stmts, 0, [])

    def closure(self):
        """The function as an expression: function(params) body end."""
        try:
            body = self.body_lines()
        except (Unstructured, StopIteration, IndexError, KeyError) as e:
            body = ['-- decompiler: %s; disassembly:' % e] + ['-- ' + l for l in disassemble(self.f, [], 'F')]
        fe = FuncExpr(['function(%s)' % ', '.join(self.params())] +
                      ['    ' + l if l else l for l in '\n'.join(body).split('\n')] + ['end'])
        fe.params, fe.body = self.params(), body
        return fe


def func_header(name, fe, method=False):
    params = list(fe.params)
    if method and params and params[0] == 'self':
        params = params[1:]
    lines = ['function %s(%s)' % (name, ', '.join(params))]
    lines += ['    ' + l if l else l for l in '\n'.join(fe.body).split('\n')] if fe.body else []
    lines.append('end')
    return '\n'.join(lines)


def assign_text(target, v, method=False):
    if isinstance(v, FuncExpr):
        if method and v.params and v.params[0] == 'self' and '.' in target and \
                is_ident(target.rsplit('.', 1)[1]):
            obj, name = target.rsplit('.', 1)
            return func_header('%s:%s' % (obj, name), v, method=True)
        if all(is_ident(p) for p in target.split('.')):
            return func_header(target, v)
    if isinstance(v, TableExpr):
        v.finish()
    return '%s = %s' % (target, v.text)


def decompile(f):
    d = FuncDecompiler(f)
    try:
        body = d.body_lines()
    except (Unstructured, StopIteration, IndexError, KeyError) as e:
        body = ['-- decompiler: %s; disassembly:' % e] + ['-- ' + l for l in disassemble(f, [])]
    head = ['-- decompiled from %s by tools/luadec.py (pseudo-Lua)' % (f.source or '?')]
    return head + body


# ---------------------------------------------------------------- checking the output in the game

# The game's own compiler checks that the pseudo-Lua is valid Lua for its dialect: `check` writes a
# CRASH_DEV_LUA_FILE whose blocks loadstring (compile, never run) each decompiled script and log
# "LUADEC <n> ok" or the compiler's error. Comparing bytecode is not possible in the game:
# string.dump crashes in the retail build (Radical's DumpFunction reads the source from a Proto
# field that is not a string for functions compiled at run time).

CHECK_BLOCK_BYTES = 600000
CHECK_HEAD = '''function __luadec_check(id, src)
    local f, err = loadstring(src, "=" .. id)
    if f then
        devprint("LUADEC " .. id .. " ok")
    else
        devprint("LUADEC " .. id .. " error " .. tostring(err))
    end
end'''


def check_write(files, out):
    blocks, cur, size = [], [CHECK_HEAD], 0
    for i, p in enumerate(files):
        src = '\n'.join(decompile(load(p.read_bytes()))) + '\n'
        cur.append('__luadec_check("%d", %s)' % (i, fmt_string(src)))
        size += len(src)
        if size >= CHECK_BLOCK_BYTES:
            blocks.append(cur)
            cur, size = [], 0
    cur.append('devprint("LUADEC done")')
    blocks.append(cur)
    lines = []
    for k, b in enumerate(blocks):
        # one block per second: each chunk is compiled, run and freed on its own
        lines.append('@gameplay_ready+%d' % k if k else '@gameplay_ready')
        lines.extend(b)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text('\n'.join(lines) + '\n', encoding='latin-1')
    out.with_suffix('.txt').write_text('\n'.join(str(p) for p in files) + '\n', encoding='utf-8')
    return len(blocks)


def check_log(manifest, log):
    files = [Path(l) for l in manifest.read_text(encoding='utf-8').splitlines() if l]
    status, done = {}, False
    for line in log.read_text(encoding='utf-8', errors='replace').splitlines():
        i = line.find('LUADEC ')
        if i < 0:
            continue
        w = line[i + 7:].split(' ', 2)
        if w[0] == 'done':
            done = True
        elif w[0].isdigit():
            status[int(w[0])] = w[1:]
    bad = 0
    for i, p in enumerate(files):
        st = status.get(i)
        if st and st[0] == 'ok':
            continue
        bad += 1
        if not st:
            print('%s: not checked (no line in the log)' % p)
            continue
        msg = st[1] if len(st) > 1 else ''
        print('%s: %s' % (p, msg))
        try:
            n = int(msg.split(':')[1])
            src = decompile(load(p.read_bytes()))
            print('    %d: %s' % (n, src[n - 1].strip()))
        except (ValueError, IndexError):
            pass
    print('%d/%d decompiled scripts compile in the game%s' % (
        len(files) - bad, len(files), '' if done else ' (the log has no "LUADEC done")'))
    return bad == 0 and done


# ---------------------------------------------------------------- command line

def is_compiled(path):
    with open(path, 'rb') as fh:
        return fh.read(10) == HEADER


def script_files(src):
    for p in sorted(src.rglob('*')):
        if p.suffix.lower() in ('.lua', '.blua') and p.is_file():
            yield p


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('dis', 'dec'):
        s = sub.add_parser(name)
        s.add_argument('files', nargs='+', type=Path)
    s = sub.add_parser('check')
    s.add_argument('files', nargs='*', type=Path, help='default: every compiled script in --src')
    s.add_argument('--src', type=Path, default=ROOT / 'private/assets/raw/default')
    s.add_argument('--lua', type=Path, default=ROOT / 'private/luadec/check.lua')
    s.add_argument('--log', type=Path, help='run.log of the dev_luadec run to read')
    s = sub.add_parser('all')
    s.add_argument('--src', type=Path, default=ROOT / 'private/assets/raw/default')
    s.add_argument('--out', type=Path, default=ROOT / 'private/scripts_dec')
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

    if args.cmd in ('dis', 'dec'):
        for p in args.files:
            data = p.read_bytes()
            if data[:10] != HEADER:
                print('-- %s: plain source, not compiled' % p)
                continue
            f = load(data)
            lines = disassemble(f, []) if args.cmd == 'dis' else decompile(f)
            print('\n'.join(lines))
        return 0

    if args.cmd == 'check':
        if args.log:
            return 0 if check_log(args.lua.with_suffix('.txt'), args.log) else 1
        files = [p for p in (args.files or script_files(args.src)) if is_compiled(p)]
        n = check_write(files, args.lua)
        print('wrote %s (%d scripts, %d blocks); now run\n'
              '  python tools/run_game.py dev_luadec\n'
              '  python tools/luadec.py check --log build/runs/<that run>/run.log' % (args.lua, len(files), n))
        return 0

    ok = text = failed = gotos = 0
    for p in script_files(args.src):
        rel = p.relative_to(args.src)
        data = p.read_bytes()
        if data[:10] != HEADER:
            text += 1
            continue
        try:
            f = load(data)
        except LuaFormatError as e:
            print('%s: %s' % (rel, e))
            failed += 1
            continue
        out = args.out / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        (out.parent / (out.name + '.dis.txt')).write_text('\n'.join(disassemble(f, [])) + '\n',
                                                          encoding='utf-8')
        dec = '\n'.join(decompile(f)) + '\n'
        if 'goto pc' in dec or '-- decompiler:' in dec:
            gotos += 1
        (out.parent / (out.stem + '.dec.lua')).write_text(dec, encoding='utf-8')
        ok += 1
    print('%d compiled scripts parsed (%d with goto/fallback), %d plain source skipped, %d failed'
          ' -> %s' % (ok, gotos, text, failed, args.out))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
