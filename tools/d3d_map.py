#!/usr/bin/env python3
"""Map the game's Xbox Direct3D layer in the recompiled code (docs/internals/native-gpu-plan.md).

The title links the Xbox 360 D3D library statically (no symbols). Pure3D's Xenon backend
(`pure3d::xn*` classes, named by RTTI) is the layer above it and makes almost every D3D call. This
script reads generated/ppc (the per-instruction asm comments) and private/symbols, and reports:

  1. the D3D entry points: functions in the D3D range called from outside it, with the regions
     and the named xn methods that call them;
  2. for each xn vtable slot (xnContext, xnDisplay, xnDevice, xnTexture, xnPrimBuffer, the shader
     classes...), the D3D functions it reaches directly;
  3. who else calls D3D (not xn): the places a D3D-level replacement must also cover.

    python tools/d3d_map.py                  # text report to stdout
    python tools/d3d_map.py --json build/native_gpu/d3d_map.json

Ranges are this dump's (title 565507E4); they come from where the make-space function
(sub_8226CCC8) and its callers sit. Nothing is written to private/ or generated/.
"""
import argparse
import collections
import csv
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

D3D_RANGE = (0x82268C00, 0x822845A0)      # Xbox D3D
D3D_DRAWS = (0x823EA9A8, 0x823EBBD8)      # D3D draw functions (4 variants), linked apart from the rest
XN_RANGE = (0x8238B000, 0x823AC000)       # pure3d::xn* (pddi Xenon backend) incl. effect shaders

FUNC = re.compile(r"^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)")
BL = re.compile(r"^\t// bl 0x([0-9a-f]{8})")
B = re.compile(r"^\t// b 0x([0-9a-f]{8})")
INSN = re.compile(r"^\t// [a-z]")


def call_graph(ppc):
    funcs = {}
    cur = None
    for path in sorted(ppc.glob("ppc_recomp.*.cpp")):
        for line in path.open(encoding="utf-8", errors="replace"):
            m = FUNC.match(line)
            if m:
                cur = funcs[int(m.group(1), 16)] = {"calls": set(), "tails": set(), "n": 0}
                continue
            if cur is None:
                continue
            m = BL.match(line)
            if m:
                cur["calls"].add(int(m.group(1), 16))
            m = B.match(line)
            if m:
                cur["tails"].add(int(m.group(1), 16))
            if INSN.match(line):
                cur["n"] += 1
    for address, f in funcs.items():
        # A `b` to another function's start is a tail call.
        f["calls"] |= {t for t in f["tails"] if t in funcs and t != address}
        del f["tails"]
    return funcs


def in_range(address, r):
    return r[0] <= address < r[1]


def is_d3d(address):
    return in_range(address, D3D_RANGE) or in_range(address, D3D_DRAWS)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ppc", default=str(ROOT / "generated" / "ppc"))
    parser.add_argument("--symbols", default=str(ROOT / "private" / "symbols"))
    parser.add_argument("--json")
    args = parser.parse_args()

    funcs = call_graph(Path(args.ppc))
    names = {}
    with open(Path(args.symbols) / "functions.csv", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            names[int(row["address"], 16)] = row["name"]
    rtti = json.loads((Path(args.symbols) / "rtti.json").read_text(encoding="utf-8"))

    callers = collections.defaultdict(set)
    for address, f in funcs.items():
        for c in f["calls"]:
            callers[c].add(address)

    def label(address):
        return f"{address:08X}" + (f" {names[address]}" if address in names else "")

    def region(address):
        if is_d3d(address):
            return "d3d"
        if in_range(address, XN_RANGE):
            return "xn"
        return f"{address >> 16:04X}xxxx"

    # 1. Entry points.
    api = sorted(a for a in funcs if is_d3d(a) and any(not is_d3d(c) for c in callers[a]))
    report = {"entry_points": [], "xn_methods": [], "other_callers": []}
    print(f"== D3D entry points: {len(api)} (D3D range {D3D_RANGE[0]:08X}-{D3D_RANGE[1]:08X} "
          f"+ draws {D3D_DRAWS[0]:08X}-{D3D_DRAWS[1]:08X})")
    for a in api:
        outside = sorted(c for c in callers[a] if not is_d3d(c))
        regions = collections.Counter(region(c) for c in outside)
        named = [names[c] for c in outside if c in names and "pure3d::xn" in names[c]]
        print(f"  {a:08X} {funcs[a]['n']:4d} insns  " + ", ".join(f"{k} x{v}" for k, v in regions.most_common())
              + (f"  [{'; '.join(sorted(set(named))[:4])}]" if named else ""))
        report["entry_points"].append({"address": f"0x{a:08X}", "instructions": funcs[a]["n"],
                                       "callers": [f"0x{c:08X}" for c in outside], "named_callers": sorted(set(named))})

    # 2. xn vtables.
    print("\n== pure3d::xn* vtable slots that call D3D directly")
    for cls in sorted(rtti["classes"], key=lambda c: c["name"]):
        if not cls["name"].startswith("pure3d::xn"):
            continue
        for vt in cls.get("vtables", []):
            rows = []
            for m in vt["methods"]:
                address = int(m["address"], 16)
                if address not in funcs:
                    continue
                d3d = sorted(c for c in funcs[address]["calls"] if is_d3d(c))
                if d3d:
                    rows.append((m["slot"], address, d3d))
            if rows:
                print(f"  {cls['name']} (vtable {vt['address']})")
                for slot, address, d3d in rows:
                    print(f"    vf{slot:<3d} {address:08X} ({funcs[address]['n']} insns) -> " + " ".join(f"{c:08X}" for c in d3d))
                    report["xn_methods"].append({"class": cls["name"], "slot": slot, "address": f"0x{address:08X}",
                                                 "d3d": [f"0x{c:08X}" for c in d3d]})

    # 3. Other callers.
    print("\n== D3D callers outside pure3d::xn (a D3D-level replacement covers these too)")
    other = collections.defaultdict(set)
    for a in api:
        for c in callers[a]:
            if not is_d3d(c) and not in_range(c, XN_RANGE):
                other[c].add(a)
    for c in sorted(other):
        print(f"  {label(c)} -> " + " ".join(f"{a:08X}" for a in sorted(other[c])))
        report["other_callers"].append({"caller": f"0x{c:08X}", "name": names.get(c, ""), "d3d": [f"0x{a:08X}" for a in sorted(other[c])]})

    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(report, indent=1), encoding="utf-8")
        print(f"\nJSON: {args.json}")


if __name__ == "__main__":
    main()
