"""One build entry point for Windows and Linux. It only runs the scripts that already do the work
(tools/toolchain.py, tools/setup.py, tools/make_installer.py, tools/make_appimage.py, tools/run_game.py).

    python tools/build.py check                    # what is there and what is missing
    python tools/build.py game <game folder>       # compiles the game into dist/ (your own copy)
    python tools/build.py package [TAG]           # the installer: release/<TAG>/ (no game file needed)
    python tools/build.py test                     # boot test on dist/ (after game)

Windows: the installer is CrashSetup-<TAG>.exe (tools/make_installer.py). Linux: CrashSetup-<TAG>-x86_64.AppImage
(tools/make_appimage.py). Both pack the working tree, uncommitted changes included, as --worktree does.
macOS is not supported yet (no toolchain, no runtime port, no package format).
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
WINDOWS = sys.platform == "win32"
LINUX = sys.platform.startswith("linux")
sys.path.insert(0, str(TOOLS))
import toolchain  # noqa: E402  (the bundled toolchain of this platform, REQUIRED per OS)


def run(*args):
    print("> " + " ".join(str(a) for a in args), flush=True)
    result = subprocess.run([sys.executable, *map(str, args)], cwd=ROOT)
    if result.returncode != 0:
        raise SystemExit(f"falhou (código {result.returncode}): {' '.join(map(str, args))}")


def require_supported():
    if sys.platform == "darwin":
        raise SystemExit("macOS ainda não é suportado: não há toolchain, port do runtime nem pacote para ele.")
    if not (WINDOWS or LINUX):
        raise SystemExit(f"plataforma não suportada: {sys.platform}")


def ensure_toolchain():
    if toolchain.missing():
        run(TOOLS / "toolchain.py", "fetch")


def check():
    ok = True
    missing = toolchain.missing()
    ok = ok and not missing
    print(("ok      " if not missing else "falta   ") + "toolchain/: " +
          ("completa" if not missing else ", ".join(missing) + "  ->  python tools/build.py check (fetch: tools/toolchain.py fetch)"))
    installer = ROOT / "build" / ("CrashSetup.exe" if WINDOWS else "CrashSetup")
    print(("ok      " if installer.is_file() else "falta   ") + f"{installer.relative_to(ROOT)} (o instalador): package constrói")
    if LINUX:
        portable = (ROOT / "thirdparty" / "sdl3" / ".portable").is_file()
        print(("ok      " if portable else "opcional ") + "SDL3 portátil (AppImage de release): tools/linux_sdl3.py, precisa de clang e lld")
        for tool in ("clang", "ld.lld"):
            print(("ok      " if shutil.which(tool) else "opcional ") + f"{tool} (só para o SDL3 portátil e a DLSS)")
    return ok


def package(tag):
    require_supported()
    ensure_toolchain()
    run(TOOLS / "setup.py", "--installer-only")
    if WINDOWS:
        run(TOOLS / "make_installer.py", tag, "--worktree")
        print(f"pronto: release\\{tag}\\CrashSetup-{tag}.exe")
        return
    make_args = [TOOLS / "make_appimage.py", tag, "--worktree"]
    if not (ROOT / "thirdparty" / "sdl3" / ".portable").is_file():
        make_args.append("--steam-sdl3")  # SDL3 from the Steam Runtime; the release uses the portable build
    run(*make_args)
    print(f"pronto: release/{tag}/CrashSetup-{tag}-x86_64.AppImage")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("check", help="o que existe e o que falta")
    game = sub.add_parser("game", help="compila o jogo em dist/ com a sua cópia")
    game.add_argument("folder", help="a pasta do jogo (default.xex, .rcf, shaders/)")
    pack = sub.add_parser("package", help="gera o instalador (sem o jogo)")
    pack.add_argument("tag", nargs="?", default="local")
    sub.add_parser("test", help="teste de boot em dist/ (depois de game)")
    args = p.parse_args()

    if args.command == "check":
        sys.exit(0 if check() else 1)
    require_supported()
    if args.command == "game":
        ensure_toolchain()
        run(TOOLS / "setup.py", "--game", args.folder)
    elif args.command == "package":
        package(args.tag)
    elif args.command == "test":
        run(TOOLS / "run_game.py", "boot", "--dist", "--no-build")


if __name__ == "__main__":
    main()
