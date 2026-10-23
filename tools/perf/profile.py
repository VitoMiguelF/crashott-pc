"""Names the samples of tools/perf/sampler.exe and prints a profile per thread.

    python tools/perf/profile.py <samples.txt> [--log run.log] [--map build/CrashRecomp.map] [--top 25]
                                 [--thread TID] [--out report.txt]

Each sample weighs the CPU cycles its thread used since the previous tick (sampler line "S"), so a
thread's numbers are a share of its own CPU time. Self = the function the RIP was in; inclusive =
anywhere on the stack. Recompiled functions (sub_XXXXXXXX) get the RTTI names of
private/symbols/functions.csv. Threads are named from the run.log ([thread] starting ...) or by the
runtime function at the bottom of their stack.
"""
import argparse
import bisect
import collections
import csv
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAP_LINE = re.compile(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+(.*)$")

# Bottom-of-stack runtime functions that identify host threads.
THREAD_ROOTS = [
    ("CommandProcessorThread", "GPU command processor"),
    ("DecoderThread", "XMA decoder"),
    ("InterruptThread", "gpu interrupts (vblank)"),
    ("ClockThread", "audio clock"),
    ("WatchdogThread", "watchdog"),
    ("ReadLoop", "hid pads"),
    ("Preload", "hd textures preload"),
]


def demangle_itanium(name):
    """The qualified name of an Itanium-mangled symbol (llvm-mingw builds), without the parameters:
    _Z12sub_82353478R10PPCContextPh -> sub_82353478, _ZN3gpu6Upload4FlushEv -> gpu::Upload::Flush."""
    body, nested = name[2:], name.startswith("_ZN")
    if nested:
        body = body[1:].lstrip("rVK")
    parts = []
    while body[:1].isdigit():
        digits = len(body) - len(body.lstrip("0123456789"))
        length = int(body[:digits])
        parts.append(body[digits:digits + length])
        body = body[digits + length:]
        if not nested:
            break
    return "::".join(parts) if parts else name


def demangle(name):
    if name.startswith("__imp__sub_"):
        return name[len("__imp__"):]
    if name.startswith("_Z"):
        return demangle_itanium(name)
    if not name.startswith("?"):
        return name
    body = name[1:]
    if body.startswith("?"):
        return name  # operators, templates: keep as is
    head = body.split("@@", 1)[0]
    parts = [p for p in head.split("@") if p and not p.startswith("?A0x") and not p.startswith("?")]
    if not parts:
        return name
    return "::".join(reversed(parts[1:])) + ("::" if len(parts) > 1 else "") + parts[0]


class Symbols:
    def __init__(self, map_path, functions_csv):
        entries = []
        for line in open(map_path, encoding="utf-8", errors="replace"):
            m = MAP_LINE.match(line)
            if not m:
                continue
            address = int(m.group(2), 16)
            if address < 0x140001000:
                continue
            entries.append((address, m.group(1), m.group(3).strip()))
        entries.sort()
        # Several names per address (alias __imp__sub_X / ?sub_X@@...): keep the most readable.
        dedup = []
        for address, name, obj in entries:
            if dedup and dedup[-1][0] == address:
                if dedup[-1][1].startswith("__imp__sub_") or "dtor$" in name:
                    continue
                if name.startswith("__imp__sub_"):
                    dedup[-1] = (address, name, obj)
                continue
            dedup.append((address, name, obj))
        self.addresses = [e[0] for e in dedup]
        self.names = [e[1] for e in dedup]
        self.objects = [e[2] for e in dedup]
        self.guest = {}
        if functions_csv and Path(functions_csv).is_file():
            for row in csv.DictReader(open(functions_csv, encoding="utf-8", errors="replace")):
                try:
                    self.guest[int(row["address"], 16)] = row["name"]
                except (ValueError, KeyError):
                    pass
        self.cache = {}

    def name(self, rva_address):
        if rva_address in self.cache:
            return self.cache[rva_address]
        i = bisect.bisect_right(self.addresses, rva_address) - 1
        if i < 0:
            result = f"?{rva_address:x}"
        else:
            raw = self.names[i]
            if "dtor$" in raw or "catch$" in raw:
                raw = raw.split("??", 1)[-1]
            result = demangle(raw)
            m = re.match(r"(?:__imp__)?sub_([0-9A-F]{8})$", result) or re.match(r"\?sub_([0-9A-F]{8})@@", self.names[i])
            if m:
                guest = int(m.group(1), 16)
                result = f"sub_{m.group(1)}" + (f" [{self.guest[guest]}]" if guest in self.guest else "")
            elif "CrashPPC:" in self.objects[i]:
                result += " (PPC)"
        self.cache[rva_address] = result
        return result


def thread_names(log_path):
    names = {}
    if not log_path or not Path(log_path).is_file():
        return names
    for line in open(log_path, encoding="utf-8", errors="replace"):
        m = re.search(r"\[thread\] starting guest thread (\d+): entry (0x[0-9A-Fa-f]+)", line)
        if m:
            names[int(m.group(1))] = f"guest {m.group(2)}"
        m = re.search(r"\[thread\] starting runtime thread (\d+) \(([^)]*)\)", line)
        if m:
            names[int(m.group(1))] = m.group(2)
    return names


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("samples")
    parser.add_argument("--log")
    parser.add_argument("--map", default=str(ROOT / "build" / "CrashRecomp.map"))
    parser.add_argument("--functions", default=str(ROOT / "private" / "symbols" / "functions.csv"))
    parser.add_argument("--top", type=int, default=25)
    parser.add_argument("--min-share", type=float, default=0.5, help="threads com menos que isto (%% de um núcleo) ficam de fora")
    parser.add_argument("--out")
    args = parser.parse_args()

    symbols = Symbols(args.map, args.functions)
    base = 0x140000000
    exe = None
    per_thread = collections.defaultdict(lambda: {"cycles": 0, "samples": 0, "self": collections.Counter(),
                                                  "incl": collections.Counter(), "roots": collections.Counter()})
    totals, duration = {}, None
    for line in open(args.samples):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "M":
            exe = int(parts[1], 16)
        elif parts[0] == "T":
            totals[int(parts[1])] = int(parts[2])
        elif parts[0] == "D":
            duration = float(parts[1])
        elif parts[0] == "S":
            tid, weight = int(parts[1]), int(parts[2])
            stack = [int(x, 16) for x in parts[3:]]
            names = []
            for address in stack:
                if exe and exe <= address < exe + 0x4000000:
                    names.append(symbols.name(address - exe + base))
                else:
                    names.append("[system]")
            t = per_thread[tid]
            t["cycles"] += weight
            t["samples"] += 1
            # Self: the first frame inside the exe (system frames = waits, D3D, CRT allocations).
            t["self"][names[0]] += weight
            for name in set(names):
                t["incl"][name] += weight
            root = next((label for key, label in THREAD_ROOTS if any(key in n for n in names)), None)
            if root is None:
                if any("_xstart" in n or n.startswith("main") for n in names):
                    root = "main"
                else:
                    exe_names = [n for n in names if n != "[system]"]
                    root = exe_names[-1] if exe_names else "[system]"
            t["roots"][root] += 1

    log_names = thread_names(args.log)
    ghz = None
    lines = []
    total_cycles = sum(totals.values()) or sum(t["cycles"] for t in per_thread.values())
    lines.append(f"duração {duration:.1f} s, {sum(t['samples'] for t in per_thread.values())} amostras, "
                 f"{len(per_thread)} threads com amostras" if duration else "")
    order = sorted(per_thread.items(), key=lambda kv: -totals.get(kv[0], kv[1]["cycles"]))
    # Approximate cores: cycles / (duration * nominal clock). The 5700X here runs ~4.4-4.6 GHz.
    clock = 4.5e9
    for tid, t in order:
        cycles = totals.get(tid, t["cycles"])
        share = 100.0 * cycles / (duration * clock) if duration else 0
        if share < args.min_share:
            continue
        name = log_names.get(tid) or t["roots"].most_common(1)[0][0]
        if tid in log_names and t["roots"]:
            root = t["roots"].most_common(1)[0][0]
            if root not in name:
                name += f" / {root}"
        lines.append("")
        lines.append(f"=== thread {tid} ({name}): {share:.1f}% de um núcleo a 4,5 GHz, {t['samples']} amostras ===")
        weight = t["cycles"] or 1
        lines.append("  self:")
        for fn, w in t["self"].most_common(args.top):
            lines.append(f"    {100.0 * w / weight:5.1f}%  {fn}")
        lines.append("  inclusivo:")
        for fn, w in t["incl"].most_common(args.top):
            lines.append(f"    {100.0 * w / weight:5.1f}%  {fn}")
    text = "\n".join(lines)
    print(text)
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
