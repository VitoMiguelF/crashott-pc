"""Graphs for build/desempenho: FPS per version (bars) and frame time over the walk (lines), as SVG.

    python tools/perf/report.py <out.svg> --bars nome1=Rótulo1,nome2=Rótulo2,... [--lines nome1,nome2]
"""
import argparse
import csv
import json
from pathlib import Path

from walk_window import walk

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build" / "desempenho"
COLORS = ["#8a8f98", "#2f6fdf", "#16a37f", "#e08a1e", "#b5487a", "#5b4fc4"]


def bars(entries, width=760, row=34):
    rows = [(label, walk(name)) for name, label in entries]
    top = max(w["fps"] for _, w in rows) * 1.12
    height = 40 + row * len(rows) + 30
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" font-family="Segoe UI, sans-serif" font-size="13">',
           '<rect width="100%" height="100%" fill="#ffffff"/>',
           '<text x="12" y="22" font-size="15" font-weight="600" fill="#222">FPS na caminhada da vila (1080p, últimos 15 s do replay; 5700X + RTX 5080)</text>']
    x0, x1 = 230, width - 70
    for i, (label, w) in enumerate(rows):
        y = 40 + i * row
        length = (x1 - x0) * w["fps"] / top
        out.append(f'<text x="{x0 - 8}" y="{y + 19}" text-anchor="end" fill="#333">{label}</text>')
        out.append(f'<rect x="{x0}" y="{y + 4}" width="{length:.1f}" height="{row - 10}" rx="3" fill="{COLORS[min(i, len(COLORS) - 1)] if i else COLORS[0]}"/>')
        out.append(f'<text x="{x0 + length + 6:.1f}" y="{y + 19}" fill="#222">{w["fps"]:.1f} fps · 1% low {w["fps_1pct_low"]:.0f}</text>')
    out.append('</svg>')
    return "\n".join(out)


def lines(names, labels, width=760, height=300):
    series = []
    for name in names:
        result = json.load(open(OUT / f"{name}.json", encoding="utf-8"))
        run = Path(result["run_dir"])
        rows = [(float(r["ms"]), float(r["input_s"])) for r in csv.DictReader(open(run / "frametimes.csv"))]
        end = rows[-1][1]
        series.append([(t - (end - 16.0), ms) for ms, t in rows if end - 16.0 <= t <= end - 1.0])
    ymax = 50.0
    x0, y0, x1, y1 = 50, 30, width - 20, height - 35
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" font-family="Segoe UI, sans-serif" font-size="12">',
           '<rect width="100%" height="100%" fill="#ffffff"/>',
           '<text x="12" y="20" font-size="15" font-weight="600" fill="#222">Frame time na caminhada (ms)</text>']
    for ms in (0, 10, 16.7, 25, 33.3, 50):
        y = y1 - (y1 - y0) * ms / ymax
        out.append(f'<line x1="{x0}" y1="{y:.1f}" x2="{x1}" y2="{y:.1f}" stroke="#e3e5e8"/>')
        out.append(f'<text x="{x0 - 6}" y="{y + 4:.1f}" text-anchor="end" fill="#666">{ms:g}</text>')
    for i, points in enumerate(series):
        color = COLORS[1 + i] if i else COLORS[0]
        path = " ".join(f"{x0 + (x1 - x0) * t / 15.0:.1f},{y1 - (y1 - y0) * min(ms, ymax) / ymax:.1f}" for t, ms in points)
        out.append(f'<polyline fill="none" stroke="{color}" stroke-width="1.2" points="{path}"/>')
        out.append(f'<text x="{x0 + 10 + 200 * i}" y="{height - 12}" fill="{color}" font-weight="600">— {labels[i]}</text>')
    out.append('</svg>')
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out")
    parser.add_argument("--bars")
    parser.add_argument("--lines")
    args = parser.parse_args()
    if args.bars:
        entries = [item.split("=", 1) for item in args.bars.split(",")]
        Path(args.out).write_text(bars(entries), encoding="utf-8")
    elif args.lines:
        entries = [item.split("=", 1) for item in args.lines.split(",")]
        Path(args.out).write_text(lines([e[0] for e in entries], [e[1] for e in entries]), encoding="utf-8")


if __name__ == "__main__":
    main()
