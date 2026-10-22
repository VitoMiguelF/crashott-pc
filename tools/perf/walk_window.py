"""FPS of the gameplay part of a desempenho_vila run: by default the first 20 s after the village
becomes playable (frontend node 638 in the log, + 2 s), where every version is at the same place;
--fim: the last 15 s (the walk drifts a little with the frame rate, since the game moves Crash by
real time, so the end of the walk can look at a different part of the village in a slower build).

    python tools/perf/walk_window.py [--fim] <nome> [<nome> ...]   (build/desempenho/<nome>.json)
"""
import csv
import json
import re
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def gameplay_start(run):
    start = None
    for line in open(run / "run.log", encoding="utf-8", errors="replace"):
        m = re.search(r"frontend node 638 \(input clock ([\d.]+) s\)", line)
        if m:
            start = float(m.group(1))
    return start


def walk(name, end_part=False, seconds=None):
    result = json.load(open(ROOT / "build" / "desempenho" / f"{name}.json", encoding="utf-8"))
    run = Path(result["run_dir"])
    rows = [(float(r["ms"]), float(r["input_s"])) for r in csv.DictReader(open(run / "frametimes.csv"))]
    end = rows[-1][1]
    start = gameplay_start(run)
    if end_part or start is None:
        lo, hi = end - 1.0 - (seconds or 15.0), end - 1.0
    else:
        lo, hi = start + 2.0, start + 2.0 + (seconds or 20.0)
    ms = [m for m, t in rows if lo <= t <= hi]
    ordered = sorted(ms)
    return {
        "fps": round(len(ms) / (sum(ms) / 1000), 1),
        "ms_avg": round(statistics.mean(ms), 2),
        "ms_p99": round(ordered[int(len(ordered) * 0.99) - 1], 1),
        "fps_1pct_low": round(1000 / statistics.mean(ordered[-max(1, len(ordered) // 100):]), 1),
        "frames": len(ms),
    }


if __name__ == "__main__":
    end_part = "--fim" in sys.argv
    for name in [a for a in sys.argv[1:] if a != "--fim"]:
        w = walk(name, end_part)
        print(f"{name:16s} {w['fps']:6.1f} fps  {w['ms_avg']:6.2f} ms  p99 {w['ms_p99']:5.1f} ms  1% low {w['fps_1pct_low']:5.1f} fps  ({w['frames']} quadros)")
