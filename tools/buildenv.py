"""Machine-specific settings of the tools, found at run time instead of written in the repository.

- config/local.toml (not in git; written by setup.py) overrides anything here:

      [paths]
      game = "D:/Games/Crash"          # your dump (default.xex, default.rcf...)
      [build]
      vcvars = "C:/.../vcvars64.bat"   # only if vswhere can't find Visual Studio
      path_prepend = ["C:/LLVM/bin"]   # extra folders for clang-cl / cmake / ninja

- the game folder otherwise comes from dist/userdata/settings.toml ([paths] game, the one the player's
  exe uses) or dist/game when it holds a dump;
- vcvars64.bat from vswhere (Visual Studio 2022 or newer, or the Build Tools, with the C++ workload);
- clang-cl, cmake and ninja from PATH plus their usual install folders.
"""
import os
import shutil
import subprocess
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / "config" / "local.toml"
# Usual install folders, added after the ones from local.toml (all of them only if they exist).
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


def write_local(game=None, vcvars=None):
    """Writes config/local.toml with the given values (keeps the others)."""
    data = local()
    if game is not None:
        data.setdefault("paths", {})["game"] = Path(game).as_posix()
    if vcvars is not None:
        data.setdefault("build", {})["vcvars"] = Path(vcvars).as_posix()
    lines = ["# Machine-specific settings of the tools (tools/buildenv.py). Not in git."]
    for section, values in data.items():
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


def path_prepend():
    """Folders put in front of PATH for the builds (local.toml first, then the usual install folders)."""
    folders = list(local().get("build", {}).get("path_prepend", [])) + DEFAULT_PATH_PREPEND
    out = []
    for folder in folders:
        p = Path(os.path.expandvars(folder))
        if p.is_dir() and p not in out:
            out.append(p)
    return out


def vcvars():
    """vcvars64.bat: config/local.toml, else the newest Visual Studio with the C++ tools (vswhere). None if not found."""
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


def cmd_prelude():
    """The first lines of a .cmd script that builds: vcvars64 + PATH. Raises SystemExit without Visual Studio."""
    bat = vcvars()
    if bat is None:
        raise SystemExit("Visual Studio (C++) não encontrado: instale o Visual Studio 2022 ou as Build Tools com "
                         "\"Desenvolvimento para desktop com C++\", ou ponha [build] vcvars em config/local.toml")
    prepend = ";".join(str(p) for p in path_prepend())
    return [f'call "{bat}" >nul || exit /b 1', f'set "PATH={prepend};%PATH%"']


def which(name):
    """A tool from PATH or the usual install folders."""
    found = shutil.which(name)
    if found:
        return Path(found)
    for folder in path_prepend():
        for ext in ("", ".exe"):
            if (folder / f"{name}{ext}").is_file():
                return folder / f"{name}{ext}"
    return None
