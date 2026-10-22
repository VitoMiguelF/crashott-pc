"""Self time by source line inside the given runtime functions (samples whose RIP is in them),
with llvm-symbolizer and build\CrashRecomp.pdb.

    python tools/perf/hot_lines.py <samples.txt> <function substring> [...] [--top 20]
"""
import argparse
import collections
import subprocess
from pathlib import Path

import profile as P

ROOT = Path(__file__).resolve().parents[2]
SYMBOLIZER = r"C:\Program Files\LLVM\bin\llvm-symbolizer.exe"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("samples")
    parser.add_argument("functions", nargs="+")
    parser.add_argument("--top", type=int, default=20)
    args = parser.parse_args()
    symbols = P.Symbols(ROOT / "build" / "CrashRecomp.map", ROOT / "private" / "symbols" / "functions.csv")
    base = 0x140000000
    exe = None
    per_function = {f: collections.Counter() for f in args.functions}
    totals = collections.Counter()
    for line in open(args.samples):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "M":
            exe = int(parts[1], 16)
        elif parts[0] == "S" and len(parts) > 3:
            rip = int(parts[3], 16)
            if not exe <= rip < exe + 0x4000000:
                continue
            name = symbols.name(rip - exe + base)
            for f in args.functions:
                if f in name:
                    per_function[f][rip - exe + base] += int(parts[2])
                    totals[f] += int(parts[2])
    for f, counter in per_function.items():
        query = "".join(f"0x{a:x}\n" for a in counter)
        out = subprocess.run([SYMBOLIZER, f"--obj={ROOT / 'build' / 'CrashRecomp.exe'}", "--relativenames"],
                             input=query, capture_output=True, text=True).stdout
        blocks = out.strip().split("\n\n")
        lines = collections.Counter()
        for (address, weight), block in zip(counter.items(), blocks):
            rows = block.split("\n")
            # Innermost frame first (inlined code): its line, plus the function it was inlined into.
            where = rows[1].rsplit(":", 1)[0] if len(rows) > 1 else "?"
            lines[f"{rows[0]}  {where.split(chr(92))[-1]}"] += weight
        print(f"== {f}: {sum(counter.values())} ciclos amostrados")
        for key, weight in lines.most_common(args.top):
            print(f"  {100.0 * weight / max(1, totals[f]):5.1f}%  {key}")


if __name__ == "__main__":
    main()
