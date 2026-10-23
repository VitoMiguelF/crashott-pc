r"""Compares the screenshots of two run folders (tools/run_game.py) pixel by pixel.

    python tools\perf\compare_runs.py build\vulkan\baseline\<run> build\vulkan\vulkan\<run> [--diff out_dir]

Screenshots are paired in order (their frame numbers can differ between runs by a frame or two).
For each pair: mean absolute difference per channel (0-255), the share of pixels that differ by
more than 8, and the largest difference. --diff writes amplified difference images.
"""

import argparse
import sys
from pathlib import Path

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit("needs numpy and Pillow (python -m pip install -r tools\\requirements.txt)")


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--diff", help="folder for the difference images")
    args = parser.parse_args()
    shots_a = sorted(Path(args.a).glob("screenshot_*.png"))
    shots_b = sorted(Path(args.b).glob("screenshot_*.png"))
    if not shots_a or not shots_b:
        sys.exit("no screenshots in one of the folders")
    if len(shots_a) != len(shots_b):
        print(f"warning: {len(shots_a)} vs {len(shots_b)} screenshots; comparing the first {min(len(shots_a), len(shots_b))}")
    if args.diff:
        Path(args.diff).mkdir(parents=True, exist_ok=True)
    for a, b in zip(shots_a, shots_b):
        image_a, image_b = load(a), load(b)
        if image_a.shape != image_b.shape:
            print(f"{a.name} vs {b.name}: sizes differ {image_a.shape} vs {image_b.shape}")
            continue
        difference = np.abs(image_a - image_b)
        mean = difference.mean()
        changed = (difference.max(axis=2) > 8).mean() * 100.0
        print(f"{a.name} vs {b.name}: mean {mean:.2f}, {changed:.2f}% pixels > 8, max {int(difference.max())}")
        if args.diff:
            amplified = np.clip(difference * 4, 0, 255).astype(np.uint8)
            Image.fromarray(amplified).save(Path(args.diff) / f"diff_{a.stem}_{b.stem}.png")


if __name__ == "__main__":
    main()
