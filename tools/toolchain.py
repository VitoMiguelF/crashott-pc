"""The portable build toolchain: everything the installer needs to build the game, in toolchain\\.

The release zip already has it, so a player installs nothing and downloads no compiler:

    toolchain\\llvm-mingw\\   clang, lld, the mingw-w64 headers and import libraries (x86-64 only)
    toolchain\\cmake\\        CMake (bin\\cmake.exe + share\\cmake-*)
    toolchain\\ninja\\        Ninja
    toolchain\\python\\       the official embeddable Python, with numpy and Pillow in Lib\\site-packages

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

LLVM_MINGW = "20261006"
CMAKE = "4.4.4"
NINJA = "1.13.2"
PYTHON = "3.13.17"
PYTHON_TAG = "313"  # the wheels below are for this CPython (cp313-win_amd64)

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

# What the release's toolchain keeps of llvm-mingw: only the x86-64 Windows target. The other
# targets, lldb, clangd / clang-tidy and the sanitizer runtimes are removed (~740 -> ~330 MB).
LLVM_REMOVE_DIRS = ["aarch64-w64-mingw32", "armv7-w64-mingw32", "i686-w64-mingw32", "arm64ec-w64-mingw32",
                    "python", "share", "busybox", "lib/libear", "lib/libscanbuild"]
LLVM_REMOVE_BIN_PREFIXES = ("aarch64-", "armv7-", "i686-", "arm64ec-", "lldb", "liblldb", "libpython", "clangd",
                            "clang-tidy", "libclang_rt.", "libomp", "analyze-build", "intercept-build", "scan-build",
                            "scan-view")
LLVM_KEEP_RUNTIME = ("libclang_rt.builtins-x86_64.a",)  # lib\clang\<ver>\lib\windows

# The files each part must have (also what `check` reports).
REQUIRED = {
    "llvm-mingw": ["llvm-mingw/bin/clang.exe", "llvm-mingw/bin/clang++.exe", "llvm-mingw/bin/ld.lld.exe",
                   "llvm-mingw/x86_64-w64-mingw32/lib/libd3d12.a"],
    "cmake": ["cmake/bin/cmake.exe"],
    "ninja": ["ninja/ninja.exe"],
    "python": ["python/python.exe", f"python/python{PYTHON_TAG}._pth"],
    "numpy": ["python/Lib/site-packages/numpy/__init__.py"],
    "pillow": ["python/Lib/site-packages/PIL/__init__.py"],
}


def bin_dirs(root=TOOLCHAIN):
    """The folders that go first on PATH, in order."""
    return [root / "llvm-mingw" / "bin", root / "cmake" / "bin", root / "ninja", root / "python"]


def python_exe(root=TOOLCHAIN):
    return root / "python" / "python.exe"


# What building needs (numpy and Pillow are only for the asset export: the installer adds them
# only with ASSETS FOR MODDING).
BUILD_PARTS = ("llvm-mingw", "cmake", "ninja", "python")


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
    url, digest = PINS[name]
    dest = DOWNLOADS / (Path(url).name if name != "ninja" else f"ninja-{NINJA}-win.zip")
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
    for part in ["llvm-mingw", "cmake", "ninja", "python"]:
        folder = TOOLCHAIN / part
        size = f"{size_of(folder) / 1048576:.0f} MB" if folder.is_dir() else "-"
        print(f"{part:11} {size:>8}  {'falta' if part in gaps else 'ok'}")
    for part in ["numpy", "pillow"]:
        print(f"{part:11} {'':>8}  {'falta' if part in gaps else 'ok'}")
    return 1 if gaps else 0


if __name__ == "__main__":
    sys.exit(main())
