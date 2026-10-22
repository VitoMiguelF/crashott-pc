#!/usr/bin/env python3
"""Find guest functions that the recompiler swallowed into another function.

XenonRecomp only emits a function (and a PPCFuncMappings entry, i.e. a slot in the indirect-call
table) for addresses it knows as starts: .pdata entries, bl targets, explicit `functions` from the
config and whatever its linear scan finds after the end of the previous function. A small leaf
function that is reachable only through a pointer (vtable, callback table in data, or an address
built in code with lis/addi) and that sits right after another function whose analysed range runs
past it gets no entry of its own; calling it at runtime ends in
"[fatal] indirect call to unmapped guest address ...".

This script:
  1. collects candidate entry points: aligned 32-bit words in the data sections that point into a
     code section, addresses built in code with `lis rX,hi` + `addi/ori rY,rX,lo`, and bl targets;
  2. reads the functions that were actually generated (names from ppc_func_mapping.cpp, sizes by
     counting the per-instruction comments in ppc_recomp.*.cpp) and the explicit ranges of the
     config;
  3. reports every candidate that is not the start of a generated function, classified:
       swallowed  previous word is blr / bctr / b / zero padding and nothing in the containing
                  function branches to it: a separate function hidden inside another one
       label      the containing function branches to it (or it is a jump-table label from
                  Crash_switch_tables.toml): an internal block; a pointer to it is data that
                  happens to look like an address, or a legitimate secondary entry
       table      address of a jump table placed right after its bctr (lis/addi of the table)
       fallthru   previous instruction falls through into it: a secondary entry or coincidence
       gap        not covered by any generated function
  4. with --suggest, prints the config edits that split each explicit range containing a
     swallowed entry (or adds a new explicit function when the container is not explicit),
     checking that every switch table base and label stays inside the piece it belongs to.

Usage (from the repository root):
  python tools/find_swallowed.py [--suggest] [--verbose]
Exit code 1 when swallowed entries are found (so it can be used as a check after regenerating).
"""
import argparse
import bisect
import glob
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE_BASE = 0x82000000

BLR = 0x4E800020
BCTR = 0x4E800420


def load_image(path):
    data = open(path, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    opt = struct.unpack_from('<H', data, pe + 20)[0]
    sections = []
    for i in range(nsec):
        off = pe + 24 + opt + 40 * i
        name, vsize, va = struct.unpack_from('<8sII', data, off)
        flags = struct.unpack_from('<I', data, off + 36)[0]
        sections.append((name.rstrip(b'\0').decode(), IMAGE_BASE + va, vsize, flags))
    return data, sections


def word(image, addr):
    return struct.unpack_from('>I', image, addr - IMAGE_BASE)[0]


def simm16(v):
    return v - 0x10000 if v & 0x8000 else v


def branch_target(w, pc):
    """Target of b/bl/bc (relative only), or None."""
    op = w >> 26
    if op == 18 and not (w & 2):
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        return (pc + li) & 0xFFFFFFFF
    if op == 16 and not (w & 2):
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        return (pc + bd) & 0xFFFFFFFF
    return None


def is_uncond_b(w):
    return (w >> 26) == 18 and not (w & 1)


def is_bl(w):
    return (w >> 26) == 18 and (w & 3) == 1


# X-form (opcode 31) extended opcodes whose destination is rA (bits 16-20) instead of rD.
_XO31_WRITES_RA = {28, 60, 444, 412, 316, 124, 476, 284, 24, 536, 792, 824, 26, 954, 922, 27, 539, 794, 58, 986, 413}
_LOAD_OPS = {32, 33, 34, 35, 40, 41, 42, 43, 46, 48, 49, 50, 51, 58}
_STORE_UPDATE_OPS = {37, 39, 45, 53, 55}
# X-form update loads/stores (write rA) and forms that write no GPR at all (compares, mt*, cache
# ops, stores, FP/vector loads).
_XO31_UPDATE = {55, 119, 311, 375, 53, 373, 183, 247, 439, 181, 567, 631, 695, 759}
_XO31_NO_GPR = {0, 32, 4, 68, 144, 146, 178, 467, 54, 86, 246, 278, 470, 1014, 598, 854, 982,
                151, 183, 215, 247, 407, 439, 149, 181, 662, 918, 150, 214, 663, 695, 727, 759, 983,
                535, 567, 599, 631, 7, 39, 71, 103, 359, 135, 167, 199, 231, 487, 519, 551, 647, 679,
                775, 807, 6, 38}


def written_gprs(w):
    """GPRs an integer instruction writes (rough decoder, enough to track lis/addi pairs)."""
    op = w >> 26
    rd, ra = (w >> 21) & 31, (w >> 16) & 31
    if op in (7, 8, 12, 13, 14, 15) or op in _LOAD_OPS:
        return {rd, ra} if op in (33, 35, 41, 43, 49, 51) else {rd}
    if op in (20, 21, 23, 24, 25, 26, 27, 28, 29, 30):
        return {ra}
    if op in _STORE_UPDATE_OPS:
        return {ra}
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo in _XO31_WRITES_RA:
            return {ra}
        out = {ra} if xo in _XO31_UPDATE else set()
        if xo in _XO31_NO_GPR:
            return out
        return out | {rd}  # arithmetic, GPR loads, mf* (unknown forms: assume rD, which only drops pairs)
    return set()


def load_switch_tables(path):
    tables = []
    cur = None
    for line in open(path):
        line = line.strip()
        if line == '[[switch]]':
            cur = {'labels': []}
            tables.append(cur)
            continue
        m = re.match(r'(base|default)\s*=\s*(0x[0-9A-Fa-f]+)', line)
        if m and cur is not None:
            cur[m.group(1)] = int(m.group(2), 16)
            continue
        m = re.match(r'(0x[0-9A-Fa-f]+),?$', line)
        if m and cur is not None:
            cur['labels'].append(int(m.group(1), 16))
    return tables


def load_config_functions(path):
    text = open(path).read()
    return [(int(a, 16), int(s, 16)) for a, s in
            re.findall(r'\{\s*address\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*size\s*=\s*(0x[0-9A-Fa-f]+)\s*\}', text)]


def load_generated(gen_dir):
    """{start: size} of the functions XenonRecomp emitted."""
    name_to_addr = {}
    for m in re.finditer(r'\{\s*(0x[0-9A-Fa-f]+),\s*(\w+)\s*\}', open(os.path.join(gen_dir, 'ppc_func_mapping.cpp')).read()):
        name_to_addr[m.group(2)] = int(m.group(1), 16)
    funcs = {}
    impl = re.compile(r'^PPC_FUNC_IMPL\(__imp__(\w+)\)')
    for path in glob.glob(os.path.join(gen_dir, 'ppc_recomp.*.cpp')):
        name = None
        count = 0
        for line in open(path):
            if name is None:
                m = impl.match(line)
                if m:
                    name, count = m.group(1), 0
                continue
            if line.startswith('\t// ') and not line.startswith('\t// ERROR'):
                count += 1
            elif line.startswith('}'):
                if name in name_to_addr:
                    funcs[name_to_addr[name]] = count * 4
                name = None
    # Mapped names without a body (none expected) still count as starts.
    for addr in name_to_addr.values():
        funcs.setdefault(addr, 0)
    return funcs


def collect_candidates(image, sections):
    code = [(b, b + s) for n, b, s, f in sections if f & 0x20]
    data_secs = [(n, b, s) for n, b, s, f in sections
                 if not (f & 0x20) and n not in ('.pdata', '.reloc', '.XBLD', '.idata')]

    def in_code(a):
        return any(lo <= a < hi for lo, hi in code)

    cands = {}  # addr -> list of (kind, where)

    def add(a, kind, where):
        cands.setdefault(a, []).append((kind, where))

    for name, base, size in data_secs:
        end = min(base + size, IMAGE_BASE + len(image))
        for a in range(base, end - 3, 4):
            v = word(image, a)
            if v & 3 == 0 and in_code(v):
                add(v, 'data', a)

    for lo, hi in code:
        lis = {}  # reg -> (value, pc)
        for pc in range(lo, hi, 4):
            w = word(image, pc)
            op = w >> 26
            if w in (BLR, BCTR) or is_uncond_b(w) and not is_bl(w):
                lis.clear()
            if op == 15 and ((w >> 16) & 31) == 0:  # lis rD,imm
                lis[(w >> 21) & 31] = ((w & 0xFFFF) << 16, pc)
                continue
            if op in (14, 24):  # addi rD,rA,simm / ori rA,rS,uimm
                if op == 14:
                    dst, src, lo16 = (w >> 21) & 31, (w >> 16) & 31, simm16(w & 0xFFFF)
                else:
                    dst, src, lo16 = (w >> 16) & 31, (w >> 21) & 31, w & 0xFFFF
                if src in lis and pc - lis[src][1] <= 0x100:
                    v = (lis[src][0] + lo16) & 0xFFFFFFFF
                    if v & 3 == 0 and in_code(v):
                        add(v, 'code', pc)
                lis.pop(dst, None)
                continue
            for r in written_gprs(w):
                lis.pop(r, None)
            if is_bl(w):
                t = branch_target(w, pc)
                if in_code(t):
                    add(t, 'bl', pc)
    return cands


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--image', default=os.path.join(ROOT, 'private', 'image.bin'))
    ap.add_argument('--config', default=os.path.join(ROOT, 'config', 'Crash.toml'))
    ap.add_argument('--switch-tables', default=os.path.join(ROOT, 'config', 'Crash_switch_tables.toml'))
    ap.add_argument('--generated', default=os.path.join(ROOT, 'generated', 'ppc'))
    ap.add_argument('--suggest', action='store_true', help='print the config edits for swallowed entries')
    ap.add_argument('--verbose', action='store_true', help='also list label/fallthru/gap candidates')
    args = ap.parse_args()

    image, sections = load_image(args.image)
    tables = load_switch_tables(args.switch_tables)
    explicit = dict(load_config_functions(args.config))
    funcs = load_generated(args.generated)
    starts = sorted(funcs)
    cands = collect_candidates(image, sections)

    switch_labels = set()
    for t in tables:
        switch_labels.update(t['labels'])
    switch_bases = sorted(t['base'] for t in tables)

    def is_table_data(a):
        # lis/addi of a jump table that sits right after its bctr (the table base in the TOML is
        # the start of the lis/addi/lwzx/mtctr/bctr sequence, a few words before the table).
        i = bisect.bisect_left(switch_bases, a) - 1
        return word(image, a - 4) == BCTR and i >= 0 and a - switch_bases[i] <= 0x40

    def container(a):
        """Generated function whose range covers a (the innermost one if they overlap)."""
        best = None
        i = bisect.bisect_right(starts, a) - 1
        while i >= 0 and a - starts[i] < 0x10000:
            s = starts[i]
            if s < a < s + funcs[s] and (best is None or s > best):
                best = s
                break
            i -= 1
        return best

    results = {'swallowed': [], 'label': [], 'table': [], 'fallthru': [], 'gap': []}
    for a in sorted(cands):
        if a in funcs:
            continue
        refs = cands[a]
        c = container(a)
        prev = word(image, a - 4)
        if c is None:
            kind = 'gap'
        elif a in switch_labels:
            kind = 'label'
        elif is_table_data(a):
            kind = 'table'
        else:
            branched = any(branch_target(word(image, pc), pc) == a and not is_bl(word(image, pc))
                           for pc in range(c, c + funcs[c], 4))
            ends = prev in (BLR, BCTR, 0) or is_uncond_b(prev)
            if branched:
                kind = 'label'
            elif ends:
                kind = 'swallowed'
            else:
                kind = 'fallthru'
        results[kind].append((a, c, prev, refs))

    def fmt_refs(refs):
        out = []
        for k, w in refs[:4]:
            out.append(f'{k}@{w:08X}')
        if len(refs) > 4:
            out.append(f'+{len(refs) - 4}')
        return ' '.join(out)

    print(f'{len(funcs)} generated functions, {len(explicit)} explicit, {len(cands)} candidate entry points, '
          f'{sum(len(v) for v in results.values())} not mapped')
    for kind in ('swallowed', 'label', 'table', 'fallthru', 'gap'):
        print(f'  {kind}: {len(results[kind])}')
    for kind in ('swallowed', 'label', 'table', 'fallthru', 'gap'):
        if kind != 'swallowed' and not args.verbose:
            continue
        if not results[kind]:
            continue
        print(f'\n[{kind}]')
        for a, c, prev, refs in results[kind]:
            where = f'in sub_{c:08X} (size 0x{funcs[c]:X}{", explicit" if c in explicit else ""})' if c else ''
            print(f'  0x{a:08X} {where} prev=0x{prev:08X} refs: {fmt_refs(refs)}')

    if args.suggest and results['swallowed']:
        print('\n[suggested config]')
        # Group the swallowed entries by container.
        by_container = {}
        for a, c, prev, refs in results['swallowed']:
            by_container.setdefault(c, []).append(a)
        ok = True
        for c, entries in sorted(by_container.items()):
            end = c + funcs[c]
            cuts = {c, end, *entries}

            def make_pieces():
                pieces = []
                bounds = sorted(cuts)
                for lo, hi in zip(bounds, bounds[1:]):
                    # Trim trailing zero padding of each piece.
                    while hi - 4 > lo and word(image, hi - 4) == 0:
                        hi -= 4
                    pieces.append((lo, hi - lo))
                return pieces

            # A piece that branches into a sibling piece below a cut needs that target as an entry
            # too (shared tail reached by `b`); add it when it starts after blr/b/padding.
            extra = []
            while True:
                pieces = make_pieces()
                added = False
                for lo, sz in pieces:
                    for pc in range(lo, lo + sz, 4):
                        w = word(image, pc)
                        t = branch_target(w, pc)
                        if t is None or is_bl(w) or lo <= t < lo + sz or t in funcs or t in cuts:
                            continue
                        if not (c <= t < end):
                            continue
                        prev = word(image, t - 4)
                        if is_uncond_b(w) and (prev in (BLR, BCTR, 0) or is_uncond_b(prev)):
                            cuts.add(t)
                            extra.append(t)
                            added = True
                        else:
                            print(f'  !! branch at 0x{pc:08X} to 0x{t:08X} leaves piece 0x{lo:08X}')
                            ok = False
                if not added:
                    break
            # Every switch table must stay inside one piece.
            for t in tables:
                if not (c <= t['base'] < end):
                    continue
                piece = next(p for p in pieces if p[0] <= t['base'] < p[0] + p[1])
                for lab in t['labels'] + ([t['default']] if t.get('default') else []):
                    if not (piece[0] <= lab < piece[0] + piece[1]):
                        print(f'  !! switch at 0x{t["base"]:08X}: label 0x{lab:08X} leaves piece 0x{piece[0]:08X}')
                        ok = False
            src = 'explicit' if c in explicit else 'not explicit'
            print(f'  # sub_{c:08X} ({src}, 0x{funcs[c]:X}) -> ' + ', '.join(f'0x{e:08X}' for e in entries)
                  + (' + tail-call targets ' + ', '.join(f'0x{e:08X}' for e in sorted(extra)) if extra else ''))
            for lo, sz in pieces:
                print(f'    {{ address = 0x{lo:08X}, size = 0x{sz:X} }},')
        if not ok:
            print('  some switch tables would be split: fix those by hand')

    return 1 if results['swallowed'] else 0


if __name__ == '__main__':
    sys.exit(main())
