"""The portable build toolchain: everything the installer needs to build the game, in toolchain\\.

The release zip already has it, so a player installs nothing and downloads no compiler:

    toolchain\\llvm-mingw\\   clang, lld, the mingw-w64 headers and import libraries (x86-64 only)
    toolchain\\cmake\\        CMake (bin\\cmake.exe + share\\cmake-*)
    toolchain\\ninja\\        Ninja
    toolchain\\python\\       the official embeddable Python, with numpy and Pillow in Lib\\site-packages

On Linux (the Steam Deck first: a read-only SteamOS with no compiler and no package manager) the
same idea with Linux archives:

    toolchain/zig/      zig: clang + lld + glibc and libc++ headers for any glibc (x86_64-linux-gnu.2.35)
    toolchain/bin/      zig-cc, zig-c++, zig-ar, zig-ranlib: zig with the target, as CMake's compilers
    toolchain/cmake/    CMake (bin/cmake + share/cmake-*)
    toolchain/ninja/    Ninja
    toolchain/python/   python-build-standalone (bin/python3), relocatable

A git clone gets the same folder, from the same pinned archives (checked by SHA-256), with

    python tools/toolchain.py fetch      # download + unpack + trim (~300 MB of downloads)
    python tools/toolchain.py check      # what is there

tools/buildenv.py puts its folders first on PATH for every build ([build] toolchain = "msvc" in
config/local.toml switches back to Visual Studio + clang-cl).
"""
import argparse
import hashlib
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLCHAIN = ROOT / "toolchain"
DOWNLOADS = ROOT / "build" / "downloads" / "toolchain"

WINDOWS = sys.platform == "win32"
LLVM_MINGW = "20261006"
CMAKE = "4.4.4"
NINJA = "1.13.2"
PYTHON = "3.13.17"
PYTHON_TAG = "313"  # the wheels below are for this CPython (cp313-win_amd64)
ZIG = "0.17.0"
LINUX_PYTHON = "3.13.16"
LINUX_PYTHON_BUILD = "20261009"  # python-build-standalone release
# The glibc the Linux build links against: older than every SteamOS 3.x (3.8 has 2.41) and the
# desktop distributions people play on; zig carries the headers and stubs of each version.
ZIG_TARGET = "x86_64-linux-gnu.2.35"

# name: (url, sha256, file name in build\downloads\toolchain)
PINS = {
    "llvm-mingw": (f"https://github.com/mstorsjo/llvm-mingw/releases/download/{LLVM_MINGW}/llvm-mingw-{LLVM_MINGW}-ucrt-x86_64.zip",
                   "317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a"),
    "cmake": (f"https://github.com/Kitware/CMake/releases/download/v{CMAKE}/cmake-{CMAKE}-windows-x86_64.zip",
              "bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536"),
    "ninja": (f"https://github.com/ninja-build/ninja/releases/download/v{NINJA}/ninja-win.zip",
              "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65"),
    "ninja-license": (f"https://raw.githubusercontent.com/ninja-build/ninja/v{NINJA}/COPYING",
                      "eb7e9ab9690124c5c9f42bdc81383d886a3dede26345b6ed15bbad7caf81f7ea"),
    "python": (f"https://www.python.org/ftp/python/{PYTHON}/python-{PYTHON}-embed-amd64.zip",
               "9e35319bffedab8b36ad44d8f377cb4f1733bcec257a0b6d04d94f01486ddb38"),
    # tools/requirements.txt, for the asset export (numpy, Pillow)
    "numpy": ("https://files.pythonhosted.org/packages/90/8c/140a40398a66b4471211be1affdb6ed24c486d581bd28d07b7f2fcb69540/"
              "numpy-2.5.4-cp313-cp313-win_amd64.whl",
              "8b4d2fd2d34e5f8c9235ee787de5631a37a28402b15cb80814df973d2be54129"),
    "pillow": ("https://files.pythonhosted.org/packages/d7/ed/1b16fb66a37e1b9e41370b93b2c614c7da761a5b2a41dc333f7af4639962/"
               "pillow-13.0.0-cp313-cp313-win_amd64.whl",
               "9dda0788a9932776a75d8bcc020076edc308ce2508b51f6739a45e3158b3fc42"),
}

# The Linux toolchain (name: (url, sha256)); the file keeps the URL's name in build/downloads/toolchain.
LINUX_PINS = {
    "zig": (f"https://ziglang.org/download/{ZIG}/zig-x86_64-linux-{ZIG}.tar.xz",
            "1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026"),
    "cmake": (f"https://github.com/Kitware/CMake/releases/download/v{CMAKE}/cmake-{CMAKE}-linux-x86_64.tar.gz",
              "e5bb807f7728cb60cd8b27ebc97a2edb469b68655f21e844a600c3575b76f5bb"),
    "ninja": (f"https://github.com/ninja-build/ninja/releases/download/v{NINJA}/ninja-linux.zip",
              "5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6"),
    "ninja-license": PINS["ninja-license"],
    "python": (f"https://github.com/astral-sh/python-build-standalone/releases/download/{LINUX_PYTHON_BUILD}/"
               f"cpython-{LINUX_PYTHON}%2B{LINUX_PYTHON_BUILD}-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz",
               "24801ea31c7d406d559a17e2226325c723d50c56900b9c0da7b62adaae2affc9"),
}
# zig's tools as the commands CMake and the build scripts call (toolchain/bin): the target goes in
# every compile and link, so the result needs only the glibc of ZIG_TARGET.
ZIG_WRAPPERS = {"zig-cc": f"cc -target {ZIG_TARGET}", "zig-c++": f"c++ -target {ZIG_TARGET}",
                "zig-ar": "ar", "zig-ranlib": "ranlib"}

# What the release's toolchain keeps of llvm-mingw: only the x86-64 Windows target. The other
# targets, lldb, clangd / clang-tidy and the sanitizer runtimes are removed (~740 -> ~330 MB).
LLVM_REMOVE_DIRS = ["aarch64-w64-mingw32", "armv7-w64-mingw32", "i686-w64-mingw32", "arm64ec-w64-mingw32",
                    "python", "share", "busybox", "lib/libear", "lib/libscanbuild"]
LLVM_REMOVE_BIN_PREFIXES = ("aarch64-", "armv7-", "i686-", "arm64ec-", "lldb", "liblldb", "libpython", "clangd",
                            "clang-tidy", "libclang_rt.", "libomp", "analyze-build", "intercept-build", "scan-build",
                            "scan-view")
LLVM_KEEP_RUNTIME = ("libclang_rt.builtins-x86_64.a",)  # lib\clang\<ver>\lib\windows

# The files each part must have (also what `check` reports).
WINDOWS_REQUIRED = {
    "llvm-mingw": ["llvm-mingw/bin/clang.exe", "llvm-mingw/bin/clang++.exe", "llvm-mingw/bin/ld.lld.exe",
                   "llvm-mingw/x86_64-w64-mingw32/lib/libd3d12.a"],
    "cmake": ["cmake/bin/cmake.exe"],
    "ninja": ["ninja/ninja.exe"],
    "python": ["python/python.exe", f"python/python{PYTHON_TAG}._pth"],
    "numpy": ["python/Lib/site-packages/numpy/__init__.py"],
    "pillow": ["python/Lib/site-packages/PIL/__init__.py"],
}
LINUX_REQUIRED = {
    "zig": ["zig/zig", "bin/zig-cc", "bin/zig-c++", "bin/zig-ar", "bin/zig-ranlib"],
    "cmake": ["cmake/bin/cmake"],
    "ninja": ["ninja/ninja"],
    "python": ["python/bin/python3"],
}
REQUIRED = WINDOWS_REQUIRED if WINDOWS else LINUX_REQUIRED


def bin_dirs(root=TOOLCHAIN):
    """The folders that go first on PATH, in order."""
    if not WINDOWS:
        return [root / "bin", root / "zig", root / "cmake" / "bin", root / "ninja", root / "python" / "bin"]
    return [root / "llvm-mingw" / "bin", root / "cmake" / "bin", root / "ninja", root / "python"]


def python_exe(root=TOOLCHAIN):
    return root / "python" / ("python.exe" if WINDOWS else "bin/python3")


# What building needs (numpy and Pillow are only for the asset export: the installer adds them
# only with ASSETS FOR MODDING).
BUILD_PARTS = ("llvm-mingw", "cmake", "ninja", "python") if WINDOWS else ("zig", "cmake", "ninja", "python")


def missing(root=TOOLCHAIN, parts=None):
    """The parts of REQUIRED (or of `parts`) that are not there."""
    return [name for name, files in REQUIRED.items()
            if (parts is None or name in parts) and not all((root / f).is_file() for f in files)]


def present(root=TOOLCHAIN):
    return not missing(root)


# --- fetch ------------------------------------------------------------------------------------

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while block := f.read(1 << 22):
            h.update(block)
    return h.hexdigest()


def download(name):
    url, digest = (PINS if WINDOWS else LINUX_PINS)[name]
    dest = DOWNLOADS / (Path(url).name.replace("%2B", "+") if name != "ninja" else
                        f"ninja-{NINJA}-{'win' if WINDOWS else 'linux'}.zip")
    if name == "ninja-license":
        dest = DOWNLOADS / f"ninja-COPYING-v{NINJA}.txt"
    if dest.is_file() and sha256(dest) == digest:
        return dest
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    print(f"baixando {url}", flush=True)
    request = urllib.request.Request(url, headers={"User-Agent": "CrashRecomp-toolchain"})
    with urllib.request.urlopen(request, timeout=120) as response, open(part, "wb") as out:
        shutil.copyfileobj(response, out, 1 << 20)
    if sha256(part) != digest:
        part.unlink()
        raise SystemExit(f"{dest.name}: o arquivo baixado não confere (SHA-256)")
    part.replace(dest)
    return dest


def extract(archive, dest, strip_top=False):
    """Unpacks a zip into dest (a new folder), dropping its single top folder when strip_top."""
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            name = info.filename
            if strip_top:
                name = name.split("/", 1)[1] if "/" in name else ""
            if not name or info.is_dir():
                continue
            target = (dest / name).resolve()
            if dest.resolve() not in target.parents:
                raise SystemExit(f"{archive.name}: caminho inválido {info.filename}")
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, open(target, "wb") as out:
                shutil.copyfileobj(src, out)


def extract_tar(archive, dest, strip_top=False):
    """Unpacks a .tar.gz / .tar.xz into dest (a new folder), keeping the executable bits."""
    import tarfile
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    with tarfile.open(archive) as t:
        members = []
        for member in t.getmembers():
            if strip_top:
                member.name = member.name.split("/", 1)[1] if "/" in member.name else ""
            if not member.name:
                continue
            target = (dest / member.name).resolve()
            if dest.resolve() not in target.parents:
                raise SystemExit(f"{archive.name}: caminho inválido {member.name}")
            members.append(member)
        t.extractall(dest, members=members, filter="tar" if hasattr(tarfile, "tar_filter") else None)


def write_zig_wrappers(root):
    """toolchain/bin/zig-cc etc.: sh scripts that find zig next to them, so the folder can move."""
    folder = root / "bin"
    folder.mkdir(parents=True, exist_ok=True)
    for name, command in ZIG_WRAPPERS.items():
        path = folder / name
        path.write_text(f'#!/bin/sh\nexec "$(dirname "$0")/../zig/zig" {command} "$@"\n', encoding="ascii")
        path.chmod(0o755)


def trim_cmake_linux(folder):
    for f in (folder / "bin").iterdir():
        if f.name != "cmake":
            f.unlink()
    for rel in ["man", "share/aclocal", "share/bash-completion", "share/emacs", "share/vim", "share/applications",
                "share/icons", "share/mime"]:
        if (folder / rel).is_dir():
            shutil.rmtree(folder / rel)
    for html in (folder / "doc").glob("*/html"):
        shutil.rmtree(html)


def fetch_linux(root=TOOLCHAIN):
    root.mkdir(exist_ok=True)
    work = root / "_unpack"
    extract_tar(download("zig"), work / "zig", strip_top=True)
    for rel in ["doc"]:
        if (work / "zig" / rel).is_dir():
            shutil.rmtree(work / "zig" / rel)
    extract_tar(download("cmake"), work / "cmake", strip_top=True)
    trim_cmake_linux(work / "cmake")
    extract(download("ninja"), work / "ninja")
    (work / "ninja" / "ninja").chmod(0o755)
    shutil.copy2(download("ninja-license"), work / "ninja" / "COPYING.txt")
    extract_tar(download("python"), work / "python", strip_top=True)
    for part in ["zig", "cmake", "ninja", "python"]:
        if (root / part).exists():
            shutil.rmtree(root / part)
        (work / part).rename(root / part)
    shutil.rmtree(work)
    write_zig_wrappers(root)
    (root / "VERSIONS.txt").write_text(
        f"zig {ZIG} (x86_64-linux, target {ZIG_TARGET})\ncmake {CMAKE}\nninja {NINJA}\n"
        f"python {LINUX_PYTHON} (python-build-standalone {LINUX_PYTHON_BUILD})\n", encoding="ascii")


def trim_llvm_mingw(folder):
    for rel in LLVM_REMOVE_DIRS:
        path = folder / rel
        if path.is_dir():
            shutil.rmtree(path)
    for f in (folder / "bin").iterdir():
        if f.name.startswith(LLVM_REMOVE_BIN_PREFIXES):
            f.unlink()
    for runtime in folder.glob("lib/clang/*/lib/windows/*"):
        if runtime.name not in LLVM_KEEP_RUNTIME:
            runtime.unlink()


def trim_cmake(folder):
    for f in (folder / "bin").iterdir():
        if f.name.lower() != "cmake.exe":
            f.unlink()
    for rel in ["man", "share/aclocal", "share/bash-completion", "share/emacs", "share/vim"]:
        if (folder / rel).is_dir():
            shutil.rmtree(folder / rel)
    # doc\cmake-<ver>: keep the licenses (Copyright.txt and those of the libraries in cmake.exe), not the manual
    for html in (folder / "doc").glob("*/html"):
        shutil.rmtree(html)
    for manual in (folder / "doc").glob("*/*.qch"):
        manual.unlink()


def setup_python(folder, wheels):
    """The embeddable Python + the wheels unpacked into Lib\\site-packages (a wheel is a zip that
    installs by unpacking). python<ver>._pth lists the import paths: no user site, no system Python,
    and not the script's folder either, so it lists ..\\..\\tools (the scripts import their neighbours)."""
    site = folder / "Lib" / "site-packages"
    site.mkdir(parents=True, exist_ok=True)
    for wheel in wheels:
        with zipfile.ZipFile(wheel) as z:
            z.extractall(site)
    (folder / f"python{PYTHON_TAG}._pth").write_text(
        f"python{PYTHON_TAG}.zip\n.\nLib\\site-packages\n..\\..\\tools\n", encoding="ascii")


def fetch(root=TOOLCHAIN):
    if not WINDOWS:
        return fetch_linux(root)
    root.mkdir(exist_ok=True)
    work = root / "_unpack"
    extract(download("llvm-mingw"), work / "llvm-mingw", strip_top=True)
    trim_llvm_mingw(work / "llvm-mingw")
    extract(download("cmake"), work / "cmake", strip_top=True)
    trim_cmake(work / "cmake")
    extract(download("ninja"), work / "ninja")
    shutil.copy2(download("ninja-license"), work / "ninja" / "COPYING.txt")
    extract(download("python"), work / "python")
    setup_python(work / "python", [download("numpy"), download("pillow")])
    for part in ["llvm-mingw", "cmake", "ninja", "python"]:
        if (root / part).exists():
            shutil.rmtree(root / part)
        (work / part).rename(root / part)
    shutil.rmtree(work)
    (root / "VERSIONS.txt").write_text(
        f"llvm-mingw {LLVM_MINGW} (ucrt, x86_64)\ncmake {CMAKE}\nninja {NINJA}\npython {PYTHON} (embeddable, amd64)\n"
        + "".join(f"{name} {Path(PINS[name][0]).name.split('-')[1]}\n" for name in ("numpy", "pillow")),
        encoding="ascii")


def size_of(folder):
    return sum(f.stat().st_size for f in folder.rglob("*") if f.is_file())


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command", choices=["fetch", "check"])
    args = p.parse_args()
    if args.command == "fetch":
        fetch()
    gaps = missing()
    for part in BUILD_PARTS:
        folder = TOOLCHAIN / part
        size = f"{size_of(folder) / 1048576:.0f} MB" if folder.is_dir() else "-"
        print(f"{part:11} {size:>8}  {'falta' if part in gaps else 'ok'}")
    for part in [p for p in REQUIRED if p not in BUILD_PARTS]:
        print(f"{part:11} {'':>8}  {'falta' if part in gaps else 'ok'}")
    return 1 if gaps else 0


if __name__ == "__main__":
    sys.exit(main())
