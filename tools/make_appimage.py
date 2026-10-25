"""Builds the Linux installer, the counterpart of CrashSetup.exe for the Steam Deck first:
release/<tag>/CrashSetup-<tag>-x86_64.AppImage (not in git).

    python3 tools/make_appimage.py v1.07                  # HEAD
    python3 tools/make_appimage.py test --worktree        # the working tree instead of HEAD (tests)

What goes in is what tools/make_installer.py puts in the Windows exe (no game file, no recompiled code:
the game is built from the player's dump on the player's machine), with the Linux pieces:
- the source at HEAD needed to build (make_installer.INSTALL), tools/appimage_install.py (the installer
  itself) and tools/XenonRecomp at setup.py's pinned commit + patch;
- toolchain/ of Linux (python3 tools/toolchain.py fetch: zig, CMake, Ninja, Python), which also runs the
  installer;
- the third-party files setup.py fetches on Linux (FFmpeg .so files and headers, libdxcompiler.so,
  FreeType's headers, SDL3 from the Steam Runtime): thirdparty/ as `python3 tools/setup.py` left it, so
  the install works offline but for the optional subtitles.

AppDir/AppRun starts the graphical installer, usr/bin/CrashSetup (launcher/installer.inc built for Linux:
the same screens as CrashSetup.exe, with the game's look; our code only), with usr/lib/libSDL3.so.0
(tools/linux_sdl3.py's portable build). It reads payload/.crashrecomp_payload, the index written here
("tag", "file <component> <size> <path>", "link <component> <path>\t<target>"). With --text, or with no
display, AppRun runs tools/appimage_install.py with the bundled Python instead (dialogs or the
terminal). appimagetool 1.9.1 (it carries mksquashfs; run extracted, so FUSE isn't needed here) packs it
with the type2 runtime 20251108, both pinned by SHA-256 and kept in build/downloads/appimage.

Build first (the release machine): python3 tools/linux_sdl3.py, then python3 tools/setup.py (the
toolchain, thirdparty/, build/CrashSetup).
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_installer  # noqa: E402  (collect, read, INSTALL)
import toolchain  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DOWNLOADS = ROOT / "build" / "downloads" / "appimage"
APPIMAGETOOL = ("https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage",
                "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0")
RUNTIME = ("https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64",
           "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d")
# The Linux third-party files (make_installer's THIRDPARTY on Windows): (component, path in the project, source).
LINUX_THIRDPARTY = [
    ("base", "thirdparty/ffmpeg/lib", ROOT / "thirdparty" / "ffmpeg" / "lib"),
    ("base", "thirdparty/ffmpeg/include", ROOT / "thirdparty" / "ffmpeg" / "include"),
    ("base", "thirdparty/dxc/bin", ROOT / "thirdparty" / "dxc" / "bin"),
    ("base", "thirdparty/freetype", ROOT / "thirdparty" / "freetype"),
    ("base", "thirdparty/sdl3", ROOT / "thirdparty" / "sdl3"),
    # tools/linux_dlss.py's libCrashDlss.so + NVIDIA's libnvidia-ngx-dlss.so.*: installed only where the
    # installer's probe says DLSS runs (an NVIDIA RTX GPU with NVIDIA's driver), as on Windows.
    ("dlss", "thirdparty/dlss/bin", ROOT / "thirdparty" / "dlss" / "bin"),
]
# The Windows-only parts of the payload a Linux install has no use for.
# (launcher/ stays: setup.py builds CrashLauncher from it.)
SKIP_PREFIXES = ("thirdparty/fsr/", "thirdparty/imgui/backends/imgui_impl_win32",
                 "thirdparty/imgui/backends/imgui_impl_dx")

APPRUN = """#!/bin/sh
# CrashSetup (Linux): the graphical installer, or with --text (or no display) tools/appimage_install.py
# with the Python carried in the AppImage.
HERE="$(dirname "$(readlink -f "$0")")"
export APPDIR="${APPDIR:-$HERE}"
if [ "$1" = "--text" ] || { [ -z "$DISPLAY" ] && [ -z "$WAYLAND_DISPLAY" ]; }; then
    export PYTHONDONTWRITEBYTECODE=1
    unset PYTHONHOME PYTHONPATH
    exec "$HERE/payload/toolchain/python/bin/python3" -I "$HERE/payload/tools/appimage_install.py" "$@"
fi
exec "$HERE/usr/bin/CrashSetup" "$@"
"""

DESKTOP = """[Desktop Entry]
Type=Application
Name=Crash of the Titans Definitive Edition (instalador)
Comment=Instala o CrashRecomp a partir da sua cópia do jogo
Exec=AppRun
Icon=crashrecomp
Terminal=false
Categories=Game;
"""


def download(url, digest):
    import hashlib
    import urllib.request
    dest = DOWNLOADS / Path(url).name
    if dest.is_file() and hashlib.sha256(dest.read_bytes()).hexdigest() == digest:
        return dest
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    print(f"baixando {url}", flush=True)
    request = urllib.request.Request(url, headers={"User-Agent": "CrashRecomp-appimage"})
    with urllib.request.urlopen(request, timeout=120) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != digest:
        raise SystemExit(f"{dest.name}: o arquivo baixado não confere (SHA-256)")
    dest.write_bytes(data)
    dest.chmod(0o755)
    return dest


def icon_png():
    """The 256 px PNG inside launcher/crashrecomp.ico (its entries are PNGs: tools/make_icon.py)."""
    data = (ROOT / "launcher" / "crashrecomp.ico").read_bytes()
    count = struct.unpack_from("<H", data, 4)[0]
    best = None
    for i in range(count):
        width, _, _, _, _, _, size, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
        width = width or 256
        if best is None or width > best[0]:
            best = (width, data[offset:offset + size])
    if not best or not best[1].startswith(b"\x89PNG"):
        raise SystemExit("launcher/crashrecomp.ico sem um PNG dentro")
    return best[1]


def write_payload(appdir, entries, tag):
    """The files under AppDir/payload and their index (.crashrecomp_payload, read by installer_payload.inc)."""
    payload = appdir / "payload"
    entries = list(entries) + [("base", ".crashrecomp_version", (tag + "\n").encode("ascii")),
                               ("base", "crashrecomp.png", icon_png())]
    index = [f"tag {tag}"]
    for component, path, data in entries:
        if path.startswith(SKIP_PREFIXES):
            continue
        dest = payload / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(data, bytes):
            dest.write_bytes(data)
        elif Path(data).is_symlink():
            os.symlink(os.readlink(data), dest)
            index.append(f"link {component} {path}\t{os.readlink(data)}")
            continue
        else:
            shutil.copy2(data, dest)
        index.append(f"file {component} {dest.stat().st_size} {path}")
    (payload / ".crashrecomp_payload").write_text("\n".join(index) + "\n", encoding="utf-8")
    return len(index) - 1


def size_of(folder):
    return sum(f.stat().st_size for f in folder.rglob("*") if f.is_file() and not f.is_symlink())


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if sys.platform == "win32":
        raise SystemExit("o AppImage é montado no Linux (o toolchain/ e o thirdparty/ de Linux)")
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("tag")
    p.add_argument("--out", help="o AppImage de saída (padrão: release/<tag>/CrashSetup-<tag>-x86_64.AppImage)")
    p.add_argument("--worktree", action="store_true", help="leva a árvore de trabalho (arquivos não ignorados) em vez do HEAD: testes")
    args = p.parse_args()
    out = Path(args.out) if args.out else ROOT / "release" / args.tag / f"CrashSetup-{args.tag}-x86_64.AppImage"
    start = time.monotonic()

    entries = make_installer.collect(args.worktree, LINUX_THIRDPARTY)
    if not any(e[1] == "tools/appimage_install.py" for e in entries):
        # make_installer.INSTALL lists what the Windows install needs; the Linux installer script too.
        source = ROOT / "tools" / "appimage_install.py"
        entries.append(("base", "tools/appimage_install.py", source if args.worktree else
                        subprocess.run(["git", "show", "HEAD:tools/appimage_install.py"], cwd=ROOT, capture_output=True,
                                       check=True).stdout))
    appdir = ROOT / "build" / "appimage" / "AppDir"
    if appdir.exists():
        shutil.rmtree(appdir)
    appdir.mkdir(parents=True)
    count = write_payload(appdir, entries, args.tag)
    # The graphical installer (our code, built by setup.py's CMake run) and the portable SDL3 it runs with.
    setup_exe, sdl = ROOT / "build" / "CrashSetup", ROOT / "thirdparty" / "sdl3" / "lib" / "libSDL3.so.0"
    if not setup_exe.is_file():
        raise SystemExit("build/CrashSetup não existe: rode python3 tools/setup.py (ou cmake --build build --target CrashSetup)")
    if not (ROOT / "thirdparty" / "sdl3" / ".portable").is_file():
        raise SystemExit("thirdparty/sdl3 não é o SDL3 portátil: rode python3 tools/linux_sdl3.py")
    (appdir / "usr" / "bin").mkdir(parents=True)
    (appdir / "usr" / "lib").mkdir(parents=True)
    shutil.copy2(setup_exe, appdir / "usr" / "bin" / "CrashSetup")
    shutil.copy2(sdl, appdir / "usr" / "lib" / "libSDL3.so.0")
    (appdir / "AppRun").write_text(APPRUN, encoding="ascii")
    (appdir / "AppRun").chmod(0o755)
    (appdir / "crashrecomp.desktop").write_text(DESKTOP, encoding="utf-8")
    (appdir / "crashrecomp.png").write_bytes(icon_png())
    os.symlink("crashrecomp.png", appdir / ".DirIcon")
    print(f"AppDir: {count} arquivos, {size_of(appdir) / 1048576:.0f} MB", flush=True)

    tool = download(*APPIMAGETOOL)
    runtime = download(*RUNTIME)
    out.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, APPIMAGE_EXTRACT_AND_RUN="1", ARCH="x86_64", LC_ALL="C.UTF-8")

    def arg(path):
        # GLib's option parser refuses non-ASCII paths ("Área de trabalho") under some locales: relative ones.
        path = Path(path).resolve()
        return os.path.relpath(path, ROOT) if path.is_relative_to(ROOT) else str(path)
    result = subprocess.run([str(tool), "--no-appstream", "--runtime-file", arg(runtime), "--comp", "zstd",
                             arg(appdir), arg(out)], env=env, capture_output=True, text=True, cwd=ROOT)
    if result.returncode != 0:
        print(result.stdout[-3000:] + result.stderr[-3000:])
        raise SystemExit("o appimagetool falhou")
    print(f"{out}: {out.stat().st_size / 1048576:.0f} MB em {time.monotonic() - start:.0f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
