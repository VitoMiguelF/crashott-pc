r"""Puts the screenshots of two run folders side by side (pairs in order), downscaled, in one PNG.

    python tools\perf\side_by_side.py <run a> <run b> <out.png> [--width 640] [--only N]
"""

import argparse
from pathlib import Path

from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument("a")
parser.add_argument("b")
parser.add_argument("out")
parser.add_argument("--width", type=int, default=640)
parser.add_argument("--only", type=int, help="only the Nth pair (0-based)")
args = parser.parse_args()
pairs = list(zip(sorted(Path(args.a).glob("screenshot_*.png")), sorted(Path(args.b).glob("screenshot_*.png"))))
if args.only is not None:
    pairs = [pairs[args.only]]
rows = []
for x, y in pairs:
    ia, ib = Image.open(x).convert("RGB"), Image.open(y).convert("RGB")
    h = int(ia.height * args.width / ia.width)
    rows.append((ia.resize((args.width, h)), ib.resize((args.width, h))))
out = Image.new("RGB", (args.width * 2, sum(r[0].height for r in rows)))
top = 0
for ia, ib in rows:
    out.paste(ia, (0, top))
    out.paste(ib, (args.width, top))
    top += ia.height
out.save(args.out)
