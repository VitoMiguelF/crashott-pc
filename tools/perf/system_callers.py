"""Where a thread's time in system DLLs (D3D12, the driver, waits) comes from: the first runtime
frames above the system frames, for the thread whose stacks contain `--thread-function`; with
--lines, also the source line of each call (llvm-symbolizer and build\\CrashRecomp.pdb).

    python tools/perf/system_callers.py <samples.txt> [--thread-function CommandProcessorThread] [--depth 3] [--lines]
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
    parser.add_argument("--thread-function", default="CommandProcessorThread")
    parser.add_argument("--depth", type=int, default=3)
    parser.add_argument("--top", type=int, default=25)
    parser.add_argument("--lines", action="store_true")
    args = parser.parse_args()
    symbols = P.Symbols(ROOT / "build" / "CrashRecomp.map", ROOT / "private" / "symbols" / "functions.csv")
    base = 0x140000000
    exe = None
    stacks = collections.defaultdict(list)
    for line in open(args.samples):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "M":
            exe = int(parts[1], 16)
        elif parts[0] == "S":
            addresses = [int(x, 16) for x in parts[3:]]
            names = [symbols.name(a - exe + base) if exe <= a < exe + 0x4000000 else "[system]" for a in addresses]
            stacks[parts[1]].append((int(parts[2]), names, [a - exe + base for a in addresses]))
    for tid, samples in stacks.items():
        if not any(args.thread_function in n for _, names, _ in samples[:50] for n in names):
            continue
        total = sum(w for w, _, _ in samples)
        chains = collections.Counter()
        sites = collections.Counter()
        for weight, names, addresses in samples:
            k = 0
            while k < len(names) and names[k] == "[system]":
                k += 1
            if k and k < len(names):
                chains[" <- ".join(names[k:k + args.depth])] += weight
                sites[addresses[k] - 1] += weight  # inside the call instruction, not after it
        print(f"thread {tid}: time in system DLLs by caller")
        for chain, weight in chains.most_common(args.top):
            print(f"  {100.0 * weight / total:5.1f}%  {chain}")
        if args.lines:
            top = sites.most_common(args.top)
            query = "".join(f"0x{a:x}\n" for a, _ in top)
            out = subprocess.run([SYMBOLIZER, f"--obj={ROOT / 'build' / 'CrashRecomp.exe'}", "--no-inlines", "--relativenames"],
                                 input=query, capture_output=True, text=True).stdout
            blocks = out.strip().split("\n\n")
            print("  by call site:")
            for (address, weight), block in zip(top, blocks):
                lines = block.split("\n")
                where = lines[1] if len(lines) > 1 else "?"
                print(f"  {100.0 * weight / total:5.1f}%  {lines[0]}  {where}")


if __name__ == "__main__":
    main()
