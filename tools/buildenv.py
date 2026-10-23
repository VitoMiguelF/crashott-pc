"""Machine-specific settings of the tools, found at run time instead of written in the repository.

- The toolchain: by default the portable one in toolchain\\ (llvm-mingw clang + lld, CMake, Ninja and
  an embeddable Python with numpy and Pillow; tools/toolchain.py). The release zip carries it, a clone
  fetches it with `python tools/toolchain.py fetch`, and nothing is installed on the system.
  `[build] toolchain = "msvc"` in config/local.toml (or the CRASH_TOOLCHAIN=msvc environment variable)
  builds with Visual Studio / the Build Tools + LLVM clang-cl instead.
- config/local.toml (not in git; written by setup.py) overrides anything here:

      [paths]
      game = "D:/Games/Crash"          # your dump (default.xex, default.rcf...)
      [build]
      toolchain = "msvc"               # only to build with Visual Studio + clang-cl
      vcvars = "C:/.../vcvars64.bat"   # (msvc) only if vswhere can't find Visual Studio
      path_prepend = ["C:/LLVM/bin"]   # extra folders for clang-cl / cmake / ninja

- the game folder otherwise comes from dist/userdata/settings.toml ([paths] game, the one the player's
  exe uses) or dist/game when it holds a dump;
- (msvc) vcvars64.bat from vswhere (Visual Studio 2022 or newer, or the Build Tools, with the C++
  workload); clang-cl, cmake and ninja from PATH plus their usual install folders.

A build folder remembers which toolchain configured it (.crashrecomp_toolchain): switching toolchains
reconfigures it from scratch (configure_build).
"""
import os
import shutil
import subprocess
import tomllib
from pathlib import Path

import toolchain as portable

ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / "config" / "local.toml"
TOOLCHAINS = ("bundled", "msvc")
# (msvc) Usual install folders, added after the ones from local.toml (all of them only if they exist).
DEFAULT_PATH_PREPEND = [
    "%ProgramFiles%/LLVM/bin",
    "%ProgramFiles%/CMake/bin",
    "%LOCALAPPDATA%/Microsoft/WinGet/Links",
]


def local():
    """config/local.toml as a dict ({} when missing)."""
    try:
        return tomllib.loads(LOCAL.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError):
        return {}


def write_local(game=None, vcvars=None, toolchain=None):
    """Writes config/local.toml with the given values (keeps the others; toolchain "bundled" removes the key)."""
    data = local()
    if game is not None:
        data.setdefault("paths", {})["game"] = Path(game).as_posix()
    if vcvars is not None:
        data.setdefault("build", {})["vcvars"] = Path(vcvars).as_posix()
    if toolchain == "bundled":
        data.get("build", {}).pop("toolchain", None)
    elif toolchain is not None:
        data.setdefault("build", {})["toolchain"] = toolchain
    lines = ["# Machine-specific settings of the tools (tools/buildenv.py). Not in git."]
    for section, values in data.items():
        if not values:
            continue
        lines.append(f"\n[{section}]")
        for key, value in values.items():
            if isinstance(value, list):
                lines.append(f"{key} = [{', '.join(repr(str(v)).replace(chr(39), chr(34)) for v in value)}]")
            elif isinstance(value, bool):
                lines.append(f"{key} = {str(value).lower()}")
            else:
                lines.append(f'{key} = "{value}"')
    LOCAL.write_text("\n".join(lines) + "\n", encoding="utf-8")


def is_game_folder(folder):
    return folder is not None and (Path(folder) / "default.xex").is_file() and (Path(folder) / "default.rcf").is_file()


def game_folder():
    """The user's dump: config/local.toml, dist/userdata/settings.toml [paths] game, dist/game. None if unknown."""
    game = local().get("paths", {}).get("game")
    if game:
        return Path(game)
    try:
        settings = tomllib.loads((ROOT / "dist" / "userdata" / "settings.toml").read_text(encoding="utf-8"))
        game = settings.get("paths", {}).get("game")
        if game and is_game_folder(game):
            return Path(game)
    except (OSError, tomllib.TOMLDecodeError):
        pass
    if is_game_folder(ROOT / "dist" / "game"):
        return ROOT / "dist" / "game"
    return None


def toolchain_kind():
    """"bundled" (toolchain\\, the default) or "msvc": CRASH_TOOLCHAIN, else [build] toolchain."""
    kind = os.environ.get("CRASH_TOOLCHAIN") or local().get("build", {}).get("toolchain") or "bundled"
    if kind not in TOOLCHAINS:
        raise SystemExit(f"toolchain desconhecida: {kind} (use {' ou '.join(TOOLCHAINS)})")
    return kind


def path_prepend(kind=None):
    """Folders put in front of PATH for the builds: the bundled toolchain's (+ local.toml's
    path_prepend), or for msvc local.toml's path_prepend + the usual install folders."""
    kind = kind or toolchain_kind()
    folders = list(local().get("build", {}).get("path_prepend", []))
    folders = portable.bin_dirs() + folders if kind == "bundled" else folders + DEFAULT_PATH_PREPEND
    out = []
    for folder in folders:
        p = Path(os.path.expandvars(str(folder)))
        if p.is_dir() and p not in out:
            out.append(p)
    return out


def vcvars():
    """(msvc) vcvars64.bat: config/local.toml, else the newest Visual Studio with the C++ tools (vswhere). None if not found."""
    configured = local().get("build", {}).get("vcvars")
    if configured and Path(configured).is_file():
        return Path(configured)
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.is_file():
        return None
    try:
        out = subprocess.run([str(vswhere), "-latest", "-products", "*", "-prerelease",
                              "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                              "-property", "installationPath"], capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    for line in out.splitlines():
        candidate = Path(line.strip()) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
        if candidate.is_file():
            return candidate
    return None


def cmd_prelude(kind=None):
    """The first lines of a .cmd script that builds: PATH (+ vcvars64 for msvc). Raises SystemExit
    when the toolchain is missing."""
    kind = kind or toolchain_kind()
    prepend = ";".join(str(p) for p in path_prepend(kind))
    if kind == "bundled":
        gaps = portable.missing()
        if gaps:
            raise SystemExit(f"toolchain\\ incompleta (falta {', '.join(gaps)}): use o zip da release ou rode "
                             "python tools\\toolchain.py fetch")
        return [f'set "PATH={prepend};%PATH%"']
    bat = vcvars()
    if bat is None:
        raise SystemExit("Visual Studio (C++) não encontrado: instale o Visual Studio 2022 ou as Build Tools com "
                         "\"Desenvolvimento para desktop com C++\", ou ponha [build] vcvars em config/local.toml")
    return [f'call "{bat}" >nul || exit /b 1', f'set "PATH={prepend};%PATH%"']


def compilers(kind=None):
    """(C compiler, C++ compiler) for CMake."""
    kind = kind or toolchain_kind()
    if kind == "bundled":
        bin_dir = portable.TOOLCHAIN / "llvm-mingw" / "bin"
        return (bin_dir / "clang.exe").as_posix(), (bin_dir / "clang++.exe").as_posix()
    return "clang-cl", "clang-cl"


def configure_build(src, build, extra=(), kind=None):
    """The cmake configure command for src -> build with this toolchain, or None when build is already
    configured by it. A folder configured by the other toolchain is reset first (CMakeCache.txt and
    CMakeFiles\\ go; the objects are rebuilt)."""
    kind = kind or toolchain_kind()
    build = Path(build)
    stamp = build / ".crashrecomp_toolchain"
    configured = stamp.read_text().strip() if stamp.is_file() else ("msvc" if (build / "CMakeCache.txt").is_file() else None)
    if configured == kind and (build / "build.ninja").is_file():
        return None
    if configured != kind:
        (build / "CMakeCache.txt").unlink(missing_ok=True)
        shutil.rmtree(build / "CMakeFiles", ignore_errors=True)
        (build / "build.ninja").unlink(missing_ok=True)
    build.mkdir(parents=True, exist_ok=True)
    stamp.write_text(kind + "\n")
    cc, cxx = compilers(kind)
    return ["cmake", "-S", str(src), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}", *extra]


def build_lines(src, build, target=None, extra=(), kind=None):
    """.cmd lines (after cmd_prelude) that configure src -> build when needed, then build it."""
    configure = configure_build(src, build, extra, kind)
    lines = []
    if configure:
        lines.append(" ".join(f'"{a}"' if any(c in a for c in " ;") else a for a in configure) + " || exit /b 1")
    lines.append(f'cmake --build "{build}"' + (f" --target {target}" if target else ""))
    return lines


def which(name, kind=None):
    """A tool: the bundled toolchain's first (when it is the one in use), then PATH and the usual install folders."""
    kind = kind or toolchain_kind()
    folders = path_prepend(kind)
    candidates = [shutil.which(name)] if kind == "msvc" else []
    for folder in folders:
        candidates += [str(folder / f"{name}{ext}") for ext in ("", ".exe")]
    if kind == "bundled":
        candidates.append(shutil.which(name))
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    return None
