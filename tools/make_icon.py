"""CrashRecomp's icon: launcher/crashrecomp.png (256 px, the source) and launcher/crashrecomp.ico made
from it (256, 64, 48, 32, 24 and 16 px), the resource of CrashRecomp.exe, CrashLauncher.exe and
CrashSetup.exe (launcher/launcher.rc) and the icon of the shortcuts the installer makes. The PNG is
also the Linux one (tools/make_appimage.py: the AppImage and the installed desktop entries).

    python tools/make_icon.py      # after changing crashrecomp.png; needs Pillow (the bundled toolchain\\python has it)
"""
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "launcher" / "crashrecomp.png"
OUT = ROOT / "launcher" / "crashrecomp.ico"
SIZES = [256, 64, 48, 32, 24, 16]


def main():
    big = Image.open(SOURCE).convert("RGBA")
    images = [big.resize((s, s), Image.LANCZOS) for s in SIZES]
    images[0].save(OUT, format="ICO", sizes=[(s, s) for s in SIZES], append_images=images[1:])
    print(f"{OUT}: {OUT.stat().st_size} bytes")


if __name__ == "__main__":
    main()
