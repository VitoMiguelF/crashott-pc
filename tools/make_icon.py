"""Draws CrashRecomp's own icon (launcher/crashrecomp.ico: 256, 64, 48, 32, 24 and 16 px), used by
CrashLauncher.exe, CrashSetup.exe and the shortcuts the installer makes. It is drawn here from
shapes (a stylized orange fruit with leaves on a dark round tile), nothing taken from the game:

    python tools/make_icon.py      # needs Pillow (the bundled toolchain\\python has it)
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "launcher" / "crashrecomp.ico"
SIZES = [256, 64, 48, 32, 24, 16]
S = 1024  # drawn at 4x and scaled down


def draw():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # The tile: a dark blue rounded square with a lighter rim.
    d.rounded_rectangle((24, 24, S - 24, S - 24), radius=220, fill=(18, 40, 88, 255))
    d.rounded_rectangle((24, 24, S - 24, S - 24), radius=220, outline=(70, 150, 230, 255), width=28)
    # The fruit: an orange ball with a darker rim and a soft highlight.
    cx, cy, r = S // 2, S // 2 + 70, 330
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(196, 72, 8, 255))
    d.ellipse((cx - r + 26, cy - r + 18, cx + r - 26, cy + r - 34), fill=(244, 128, 24, 255))
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    g = ImageDraw.Draw(glow)
    g.ellipse((cx - 210, cy - 250, cx + 20, cy - 60), fill=(255, 226, 150, 190))
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(28)))
    # A few curved ridges, as on the fruit of the platformers.
    for k in (-1, 0, 1):
        d.arc((cx - r + 70 + 60 * abs(k), cy - r + 40, cx + r - 70 - 60 * abs(k), cy + r - 40), start=200 + 12 * k, end=330 + 12 * k,
              fill=(214, 92, 14, 255), width=14)
    # Two leaves on top.
    for side in (-1, 1):
        base = (cx + side * 20, cy - r + 30)
        tip = (cx + side * 230, cy - r - 170)
        mid = ((base[0] + tip[0]) / 2, (base[1] + tip[1]) / 2)
        nx, ny = tip[1] - base[1], -(tip[0] - base[0])
        n = math.hypot(nx, ny)
        nx, ny = nx / n * 80, ny / n * 80
        d.polygon([base, (mid[0] + nx, mid[1] + ny), tip, (mid[0] - nx, mid[1] - ny)], fill=(46, 160, 52, 255))
        d.line([base, tip], fill=(24, 104, 32, 255), width=12)
    return img


def main():
    big = draw()
    images = [big.resize((s, s), Image.LANCZOS) for s in SIZES]
    images[0].save(OUT, format="ICO", sizes=[(s, s) for s in SIZES], append_images=images[1:])
    print(f"{OUT}: {OUT.stat().st_size} bytes")


if __name__ == "__main__":
    main()
