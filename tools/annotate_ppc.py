#!/usr/bin/env python3
"""Copy generated/ppc to a reading copy with the recovered function names as comments.

For every function with a name in private/symbols/functions.csv (tools/rtti_scan.py):
  - a `// Class::vfNN  [source] note` line above its PPC_FUNC_IMPL(__imp__sub_XXXXXXXX);
  - a trailing `// Class::vfNN` on each direct call `sub_XXXXXXXX(ctx, base);`.

The compiled generated/ppc is never touched (it only changes by regenerating with XenonRecomp);
the copy goes to private/ppc_annotated by default, because the names come from the game binary.

Usage (from the repository root):
  python tools/annotate_ppc.py [--src generated/ppc] [--out private/ppc_annotated] [--symbols private/symbols/functions.csv]
"""
import argparse
import csv
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

IMPL = re.compile(r'^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)')
CALL = re.compile(r'^(\s*)sub_([0-9A-F]{8})\(ctx, base\);\s*$')


def load_symbols(path):
    names = {}
    with open(path, newline='', encoding='utf-8') as fp:
        for row in csv.DictReader(fp):
            names[int(row['address'], 16)] = (row['name'], row['source'], row.get('note', ''))
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--src', default=os.path.join(ROOT, 'generated', 'ppc'))
    parser.add_argument('--out', default=os.path.join(ROOT, 'private', 'ppc_annotated'))
    parser.add_argument('--symbols', default=os.path.join(ROOT, 'private', 'symbols', 'functions.csv'))
    args = parser.parse_args()

    if os.path.abspath(args.out) == os.path.abspath(args.src):
        sys.exit('refusing to write into the compiled generated/ppc; pick another --out')
    if not os.path.exists(args.symbols):
        sys.exit('%s not found: run python tools/rtti_scan.py first' % args.symbols)
    names = load_symbols(args.symbols)
    os.makedirs(args.out, exist_ok=True)

    functions = calls = 0
    for src in sorted(glob.glob(os.path.join(args.src, '*'))):
        if not os.path.isfile(src):
            continue
        out_lines = []
        # newline='' keeps the generated files' line endings, so a diff shows only the comments.
        with open(src, encoding='utf-8', errors='surrogateescape', newline='') as fp:
            for line in fp:
                eol = '\r\n' if line.endswith('\r\n') else '\n'
                m = IMPL.match(line)
                if m and int(m.group(1), 16) in names:
                    name, source, note = names[int(m.group(1), 16)]
                    out_lines.append('// %s  [%s]%s%s' % (name, source, ' ' + note if note else '', eol))
                    functions += 1
                else:
                    m = CALL.match(line)
                    if m and int(m.group(2), 16) in names:
                        line = '%ssub_%s(ctx, base); // %s%s' % (m.group(1), m.group(2), names[int(m.group(2), 16)][0], eol)
                        calls += 1
                out_lines.append(line)
        with open(os.path.join(args.out, os.path.basename(src)), 'w', encoding='utf-8', errors='surrogateescape', newline='') as fp:
            fp.writelines(out_lines)

    print('annotated %d functions and %d call sites in %s' % (functions, calls, args.out))


if __name__ == '__main__':
    main()
