"""CrashRecomp installer: from your own Crash of the Titans (Xbox 360) dump to a folder ready to play.

    python tools/setup.py --game D:\\Games\\Crash                  # build everything, play from dist\\
    python tools/setup.py --game D:\\Games\\Crash --subtitles      # + subtitles (EN transcript, PT-BR)
    python tools/setup.py --game D:\\Games\\Crash --remaster       # + HD AI Remaster (4.5 GB)
    python tools/setup.py --game D:\\Games\\Crash --extract-assets # + export the game's assets for modding
    python tools/setup.py --game D:\\Games\\Crash --assets-only    # only the asset export (retry it)
    python tools/setup.py --check                                 # only check the prerequisites
    python tools/setup.py --game ... --toolchain msvc             # build with Visual Studio + clang-cl
    python tools/setup.py --game ... --no-dlss                    # without DLSS (no NVIDIA RTX GPU)
    python tools/setup.py --game ... --copy-game --play-dir ..    # the installer's layout (make_dist --installed)

Run it with the Python of the release (toolchain\\python\\python.exe, what CrashSetup.exe uses) or any
Python 3.11+. The single-file installer (CrashSetup-<tag>.exe, tools/make_installer.py) unpacks this
project with everything below already in place, then runs this script: nothing is downloaded but the
optional release assets.

Steps (each one is skipped when its result is already up to date, so running it again is cheap):
 1. prerequisites: the portable toolchain in toolchain\\ (llvm-mingw clang, CMake, Ninja and Python
    with numpy and Pillow; tools/toolchain.py), which the release zip carries: nothing to install.
    With --toolchain msvc (advanced): Visual Studio 2022+ / Build Tools (C++), LLVM clang-cl, CMake,
    Ninja;
 2. the dump: default.xex must be the known version (SHA-256 below); default.rcf, english.rcf,
    sound1.rcf, sound2.rcf and shaders\\ must be there. It is never written to;
 3. XenonRecomp: its source at a pinned commit + patches\\XenonRecomp.patch in tools\\XenonRecomp. The
    release zip has it (tools\\XenonRecomp\\.crashrecomp_source names the commit and the patch); a git
    clone of this repository clones it with git;
 4. third-party SDK files (FFmpeg LGPL, NVIDIA DLSS, AMD FSR), from their official sources, checked
    by SHA-256 (thirdparty\\*\\README.md); already there in the single-file installer. DLSS: a
    clone downloads the SDK (CMake builds CrashDlss.dll from it); the installer instead carries our
    prebuilt CrashDlss.dll + nvngx_dlss.dll, and leaves them out (--no-dlss) on a PC whose GPU
    can't run DLSS;
 5. XenonRecomp turns default.xex into C++ (generated\\ppc, ~60 MB, never shared);
 6. CMake + Ninja build CrashRecomp.exe, CrashLauncher.exe and CrashSetup.exe (build\\);
 7. optional release assets (--subtitles, --remaster): downloaded from the GitHub release (or taken
    from --assets-dir), checked against its SHA256SUMS.txt and unpacked into private\\ / languages\\;
 8. tools\\make_dist.py assembles dist\\ (the folder to play), pointing it at your dump (or copying
    the dump into dist\\game with --copy-game), and CrashRecomp.exe --precompile-shaders fills
    dist\\userdata\\shadercache;
 9. optional: tools\\assets.py all exports the game's sounds, videos, textures and models to
    private\\assets for modding (README, "Extracting and modifying assets"). It needs the Python
    packages of tools\\requirements.txt (numpy, Pillow): the bundled Python has them; another Python
    gets them with pip, and a failure there only fails this step (the game is installed anyway).
    Retry it alone with --assets-only.

Machine-readable progress for the graphical installer (installer/): lines starting with "@@".
"""
import argparse
import hashlib
import importlib.metadata
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import buildenv  # noqa: E402

ROOT = buildenv.ROOT
PRIVATE = ROOT / "private"
GENERATED = ROOT / "generated" / "ppc"
XENON = ROOT / "tools" / "XenonRecomp"
XENON_URL = "https://github.com/hedge-dev/XenonRecomp.git"
XENON_COMMIT = "ddd128bcca99fe8bfbb99bea583c972351fa6ace"
XENON_PATCH = ROOT / "patches" / "XenonRecomp.patch"
# Written by tools/make_release.py into the bundled XenonRecomp source: "<commit> <sha256 of the patch>".
XENON_SOURCE_STAMP = XENON / ".crashrecomp_source"
REQUIREMENTS = ROOT / "tools" / "requirements.txt"

# Crash of the Titans (Xbox 360, NTSC/PAL disc release, title 565507E4): the only default.xex the
# addresses in config/Crash.toml are for.
XEX_SHA256 = "e76824b242c6c628738c373bdf052d456945d8e343eb934100617fd423e86373"
GAME_FILES = ["default.xex", "default.rcf", "english.rcf", "sound1.rcf", "sound2.rcf"]

# The release that holds the data assets (subtitles, HD AI Remaster) and their SHA256SUMS.txt.
# They haven't changed since v1.0: later releases only publish the source + installer zip (v1.01 to v1.04)
# or the single-file installer (v1.05 on), and setup keeps downloading the data from v1.0. Bump it only when the data assets are rebuilt.
RELEASE_TAG = "v1.0"
RELEASE_URL = f"https://github.com/VitoMiguelF/crashott-pc/releases/download/{RELEASE_TAG}/"
RELEASE_SUMS = "SHA256SUMS.txt"
SUBTITLES_ASSET = "CrashRecomp-v1.0-legendas.zip"
REMASTER_PREFIX = "CrashRecomp-v1.0-HD-AI-Remaster.part"

DLSS = "https://github.com/NVIDIA/DLSS/raw/v310.9.16/"
# FSR 3.1 for Vulkan: FidelityFX SDK 1.1.4 (the SDK 2.x only ships DX12 DLLs).
FSR = "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/raw/v1.1.4/"
# (url, destination, sha256)
DOWNLOADS = [
    (DLSS + "include/nvsdk_ngx.h", "thirdparty/dlss/include/nvsdk_ngx.h", "dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848"),
    (DLSS + "include/nvsdk_ngx_defs.h", "thirdparty/dlss/include/nvsdk_ngx_defs.h", "ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e"),
    (DLSS + "include/nvsdk_ngx_params.h", "thirdparty/dlss/include/nvsdk_ngx_params.h", "943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc"),
    (DLSS + "include/nvsdk_ngx_vk.h", "thirdparty/dlss/include/nvsdk_ngx_vk.h", "2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d"),
    (DLSS + "include/nvsdk_ngx_defs_vk.h", "thirdparty/dlss/include/nvsdk_ngx_defs_vk.h", "0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031"),
    (DLSS + "include/nvsdk_ngx_helpers_vk.h", "thirdparty/dlss/include/nvsdk_ngx_helpers_vk.h", "c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623"),
    (DLSS + "lib/Windows_x86_64/x64/nvsdk_ngx_s.lib", "thirdparty/dlss/lib/nvsdk_ngx_s.lib", "7b9a5239c1145af5f645ab472aa066f9ea79db36ac819562312aae69d65f3765"),
    (DLSS + "lib/Windows_x86_64/rel/nvngx_dlss.dll", "thirdparty/dlss/bin/nvngx_dlss.dll", "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e"),
    (FSR + "ffx-api/include/ffx_api/ffx_api.h", "thirdparty/fsr/include/ffx_api/ffx_api.h", "408b4203545e8299ed188fbb1d9ddfe0932b73ea6e8b14fe10edcf1f45771561"),
    (FSR + "ffx-api/include/ffx_api/ffx_api_types.h", "thirdparty/fsr/include/ffx_api/ffx_api_types.h", "dee2782813d22047c068e0fb27d1347324f8ce79d280a4672848b3b294cb12fe"),
    (FSR + "ffx-api/include/ffx_api/ffx_upscale.h", "thirdparty/fsr/include/ffx_api/ffx_upscale.h", "1155bcdcea9c10e6a95507bc9422ca062a853ac84250f0b09a637a66836b3ac1"),
    (FSR + "ffx-api/include/ffx_api/vk/ffx_api_vk.h", "thirdparty/fsr/include/ffx_api/vk/ffx_api_vk.h", "a50a8d5ff5689a10a6ce378f2df5569fd880c89ec46e8356aaf35eb40512edd9"),
    (FSR + "PrebuiltSignedDLL/amd_fidelityfx_vk.dll", "thirdparty/fsr/bin/amd_fidelityfx_vk.dll", "a1624cc4238fef046f30c4d80ce3f47be63fc5f5373f49e3ee9edb9960f54c78"),
]
# HLSL -> SPIR-V for the Vulkan renderer: dxcompiler.dll of Microsoft's official DXC release (signed;
# the Windows SDK's copy has no SPIR-V backend), with its licenses.
DXC_ZIP = ("https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip",
           "ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1")
DXC_FILES = {"bin/x64/dxcompiler.dll": "dxcompiler.dll", "LICENSE-MS.txt": "LICENSE-MS.txt", "LICENSE-LLVM.txt": "LICENSE-LLVM.txt"}
FFMPEG_ZIP = ("https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-30-13-08/"
              "ffmpeg-n8.1.3-9-g29e619e767-win64-lgpl-shared-8.1.zip",
              "3e47bda1607740550141e37c0e49d1e5182b34699f15adfd137ee266d346811a")
FFMPEG_DLLS = ["avcodec-62.dll", "avutil-60.dll", "swresample-6.dll"]

STEPS = ["Pré-requisitos", "Jogo", "XenonRecomp", "Bibliotecas", "Recompilação", "Compilação",
         "Extras", "Pasta do jogo", "Assets"]


# --- output -----------------------------------------------------------------------------------

def say(text):
    print(text, flush=True)


def step(index, detail=""):
    say(f"@@step {index + 1} {len(STEPS)} {STEPS[index]}" + (f" | {detail}" if detail else ""))


def progress(fraction):
    say(f"@@progress {max(0.0, min(1.0, fraction)):.3f}")


def fail(message):
    say(f"@@error {message}")
    raise SystemExit(1)


def sha256(path, chunk=1 << 22):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while block := f.read(chunk):
            h.update(block)
    return h.hexdigest()


def run(args, cwd=ROOT, env=None, log_prefix=""):
    """Runs a command, streaming its output; fails the setup on a non-zero exit."""
    say(f"> {' '.join(str(a) for a in args)}")
    args = [str(a) for a in args]
    if env is not None and not Path(args[0]).is_absolute():
        # CreateProcess searches this process's PATH, not the child's: resolve with the build PATH.
        args[0] = shutil.which(args[0], path=env.get("PATH") or env.get("Path")) or args[0]
    proc = subprocess.Popen(args, cwd=cwd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    for line in proc.stdout:
        line = line.rstrip()
        if line:
            say(log_prefix + line)
            yield line
    if proc.wait() != 0:
        fail(f"o comando falhou (código {proc.returncode}): {Path(str(args[0])).name}")


def run_all(args, **kw):
    for _ in run(args, **kw):
        pass


_ENV = None


def build_env():
    """The build environment: the toolchain's folders first on PATH (+ vcvars64.bat for msvc), computed once."""
    global _ENV
    if _ENV is None:
        lines = buildenv.cmd_prelude() + ["set"]
        script = ROOT / "build" / "setup_env.cmd"
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_text("@echo off\r\n" + "\r\n".join(lines) + "\r\n", encoding="ascii")
        out = subprocess.run(["cmd", "/d", "/c", str(script)], capture_output=True, text=True, errors="replace")
        if out.returncode != 0:
            fail("vcvars64.bat falhou: confira a instalação do Visual Studio (C++)" if buildenv.toolchain_kind() == "msvc"
                 else "não consegui preparar o ambiente de compilação: " + (out.stdout + out.stderr).strip()[-300:])
        _ENV = dict(line.split("=", 1) for line in out.stdout.splitlines() if "=" in line and not line.startswith("="))
    return _ENV


def download(url, dest, expected=None, label=None):
    """Downloads url to dest (via a .part file), checking the SHA-256 when given."""
    dest = Path(dest)
    if expected and dest.is_file() and sha256(dest) == expected:
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    say(f"baixando {label or dest.name} ...")
    request = urllib.request.Request(url, headers={"User-Agent": "CrashRecomp-setup"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response, open(part, "wb") as out:
            total = int(response.headers.get("Content-Length") or 0)
            done, last = 0, 0.0
            while block := response.read(1 << 20):
                out.write(block)
                done += len(block)
                if total and time.monotonic() - last > 0.5:
                    last = time.monotonic()
                    say(f"@@download {label or dest.name} {done} {total}")
    except OSError as error:
        part.unlink(missing_ok=True)
        fail(f"não consegui baixar {url}: {error}")
    if expected and sha256(part) != expected:
        part.unlink(missing_ok=True)
        fail(f"{dest.name}: o arquivo baixado não confere (SHA-256)")
    part.replace(dest)
    return dest


# --- steps ------------------------------------------------------------------------------------

def xenon_source_bundled():
    """The release's XenonRecomp source (no .git): the pinned commit with the current patch applied."""
    if (XENON / ".git").exists() or not XENON_SOURCE_STAMP.is_file():
        return False
    return XENON_SOURCE_STAMP.read_text(encoding="ascii").split() == [XENON_COMMIT, sha256(XENON_PATCH)]


def check_prerequisites():
    """Returns {name: path or None}; the setup needs all of them."""
    if buildenv.toolchain_kind() == "bundled":
        tc = buildenv.portable
        gaps = tc.missing()
        parts = {"Toolchain: llvm-mingw (clang)": ("llvm-mingw", tc.TOOLCHAIN / "llvm-mingw" / "bin" / "clang++.exe"),
                 "Toolchain: CMake": ("cmake", tc.TOOLCHAIN / "cmake" / "bin" / "cmake.exe"),
                 "Toolchain: Ninja": ("ninja", tc.TOOLCHAIN / "ninja" / "ninja.exe")}
        found = {name: None if part in gaps else path for name, (part, path) in parts.items()}
    else:
        found = {
            "Visual Studio (C++)": buildenv.vcvars(),
            "clang-cl (LLVM)": buildenv.which("clang-cl"),
            "CMake": buildenv.which("cmake"),
            "Ninja": buildenv.which("ninja"),
        }
    if not xenon_source_bundled():
        found["Git"] = buildenv.which("git")  # only a clone of the repository needs it, for XenonRecomp
    for name, path in found.items():
        say(f"@@prereq {'ok' if path else 'missing'} {name} | {path or ''}")
    if sys.version_info < (3, 11):
        say("@@prereq missing Python 3.11+ |")
        found["Python 3.11+"] = None
    else:
        say(f"@@prereq ok Python | {sys.executable}")
    return found


# --- Python packages (only the asset export needs them) ----------------------------------------

def python_requirements():
    """[(name, minimum version)] of tools/requirements.txt."""
    reqs = []
    for line in REQUIREMENTS.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            name, _, minimum = line.partition(">=")
            reqs.append((name.strip(), minimum.strip()))
    return reqs


def version_tuple(text):
    parts = []
    for piece in text.split("."):
        digits = piece[:len(piece) - len(piece.lstrip("0123456789"))]
        if not digits:
            break
        parts.append(int(digits))
    return tuple(parts)


def check_python_packages(report=True):
    """The tools/requirements.txt packages missing (or too old) in this Python, as "name>=min".
    report: "@@pypkg ok|missing <name>>=<min> | <installed version>" lines for the installer."""
    importlib.invalidate_caches()
    missing = []
    for name, minimum in python_requirements():
        try:
            have = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            have = None
        ok = have is not None and version_tuple(have) >= version_tuple(minimum)
        if report:
            say(f"@@pypkg {'ok' if ok else 'missing'} {name}>={minimum} | {have or ''}")
        if not ok:
            missing.append(f"{name}>={minimum}")
    return missing


def pip_command_text():
    """What to run by hand when the automatic install fails (the same Python as setup.py)."""
    return f'"{sys.executable}" -m pip install -r "{REQUIREMENTS}"'


def packages_ready_in_new_process():
    """A new Python sees packages pip put in a user site folder that didn't exist when this one started."""
    return subprocess.run([sys.executable, str(Path(__file__).resolve()), "--check-packages"],
                          capture_output=True).returncode == 0


def install_python_packages():
    """pip install -r tools/requirements.txt into this Python. Returns None when everything is
    there, else why it failed (it never stops the setup: only the asset export needs them)."""
    missing = check_python_packages(report=False)
    if not missing or packages_ready_in_new_process():
        return None
    say("instalando os pacotes Python da exportação de assets: " + ", ".join(missing))
    has_pip = subprocess.run([sys.executable, "-m", "pip", "--version"], capture_output=True).returncode == 0
    if not has_pip:
        say("este Python não tem o pip; tentando python -m ensurepip")
        subprocess.run([sys.executable, "-m", "ensurepip", "--upgrade"], capture_output=True)
        has_pip = subprocess.run([sys.executable, "-m", "pip", "--version"], capture_output=True).returncode == 0
        if not has_pip:
            return "este Python não tem o pip"
    args = [sys.executable, "-m", "pip", "install", "-r", str(REQUIREMENTS), "--disable-pip-version-check", "--no-input"]
    say(f"> {pip_command_text()}")
    proc = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace")
    for line in proc.stdout:
        if line.strip():
            say("  " + line.rstrip())
    if proc.wait() != 0:
        return "o pip não conseguiu instalar (sem internet?)"
    if not packages_ready_in_new_process():
        return "o pip terminou, mas os pacotes continuam faltando"
    say("pacotes Python ok: " + ", ".join(missing))
    return None


def require_python_packages():
    """Before the asset export: install the packages, or fail this step with the command to run by hand."""
    problem = install_python_packages()
    if problem:
        missing = ", ".join(name.split(">=")[0] for name in check_python_packages(report=False))
        say("Para instalar à mão, rode num prompt e depois tente de novo (setup.py --assets-only):")
        say("  " + pip_command_text())
        say(f"@@hint {pip_command_text()}")
        fail(f"faltam pacotes Python para exportar os assets ({missing}): {problem}. "
             "O jogo já está instalado; instale-os com o comando abaixo e tente de novo.")


def check_game(game, allow_other_xex=False):
    game = Path(game)
    missing = [name for name in GAME_FILES if not (game / name).is_file()]
    if not (game / "shaders").is_dir():
        missing.append("shaders\\")
    if missing:
        fail(f"{game} não parece o Crash of the Titans de Xbox 360: falta {', '.join(missing)}")
    digest = sha256(game / "default.xex")
    if digest != XEX_SHA256:
        message = f"default.xex diferente do esperado (SHA-256 {digest[:16]}...): esta versão não é suportada"
        if not allow_other_xex:
            fail(message)
        say("aviso: " + message)
    PRIVATE.mkdir(exist_ok=True)
    target = PRIVATE / "default.xex"
    if not target.is_file() or sha256(target) != digest:
        shutil.copy2(game / "default.xex", target)
    say(f"jogo ok: {game}")


def setup_xenonrecomp():
    if xenon_source_bundled():
        say("XenonRecomp ok (código do zip: " + XENON_COMMIT[:10] + " + patches/XenonRecomp.patch)")
        return
    git = buildenv.which("git")
    if git is None:
        fail(f"{XENON} não tem o código do XenonRecomp e o Git não foi encontrado: use o zip da release "
             "ou instale o Git")
    if not (XENON / ".git").exists():
        if XENON.exists():
            fail(f"{XENON} existe mas não é um clone do XenonRecomp: apague a pasta e rode de novo")
        run_all([git, "-c", "core.autocrlf=false", "clone", XENON_URL, XENON])
    head = subprocess.run([git, "rev-parse", "HEAD"], cwd=XENON, capture_output=True, text=True).stdout.strip()
    if head != XENON_COMMIT:
        status = subprocess.run([git, "status", "--porcelain", "--untracked-files=no"], cwd=XENON, capture_output=True, text=True).stdout
        if status.strip():
            fail(f"{XENON} tem outro commit com mudanças locais: apague a pasta e rode de novo")
        run_all([git, "-c", "core.autocrlf=false", "fetch", "origin"], cwd=XENON)
        run_all([git, "-c", "advice.detachedHead=false", "checkout", XENON_COMMIT], cwd=XENON)
    run_all([git, "-c", "core.autocrlf=false", "submodule", "update", "--init", "--recursive"], cwd=XENON)
    applied = subprocess.run([git, "apply", "--reverse", "--check", "--ignore-whitespace", XENON_PATCH], cwd=XENON,
                             capture_output=True).returncode == 0
    if not applied:
        run_all([git, "apply", "--ignore-whitespace", XENON_PATCH], cwd=XENON)
    say("XenonRecomp ok (" + XENON_COMMIT[:10] + " + patches/XenonRecomp.patch)")


def fetch_thirdparty(dlss=True):
    """dlss=False (--no-dlss): none of the DLSS files. With the installer's prebuilt CrashDlss.dll
    (thirdparty/dlss/bin) the SDK's headers and library aren't needed: only nvngx_dlss.dll."""
    prebuilt = (ROOT / "thirdparty" / "dlss" / "bin" / "CrashDlss.dll").is_file()
    wanted = [d for d in DOWNLOADS if not d[1].startswith("thirdparty/dlss/") or
              (dlss and (not prebuilt or d[1].endswith("nvngx_dlss.dll")))]
    if not dlss:
        say("DLSS fora (--no-dlss): sem placa NVIDIA RTX, o jogo usa FSR ou nenhum upscaler")
    elif prebuilt:
        say("DLSS: CrashDlss.dll pré-compilado do instalador (o SDK não é necessário)")
    total = len(wanted) + 1
    for i, (url, dest, digest) in enumerate(wanted):
        download(url, ROOT / dest, digest)
        progress((i + 1) / total)
    dlls = [ROOT / "thirdparty" / "ffmpeg" / "bin" / name for name in FFMPEG_DLLS]
    headers = ROOT / "thirdparty" / "ffmpeg" / "include" / "libavcodec" / "avcodec.h"
    if not (all(d.is_file() for d in dlls) and headers.is_file()):
        url, digest = FFMPEG_ZIP
        archive = download(url, ROOT / "build" / "downloads" / Path(url).name, digest, "FFmpeg 8.1 (LGPL)")
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                parts = info.filename.split("/", 1)
                if len(parts) < 2 or info.is_dir():
                    continue
                rel = parts[1]
                if rel.startswith("include/") or rel in (f"bin/{d}" for d in FFMPEG_DLLS):
                    target = ROOT / "thirdparty" / "ffmpeg" / rel
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with z.open(info) as src, open(target, "wb") as out:
                        shutil.copyfileobj(src, out)
    dxc = ROOT / "thirdparty" / "dxc" / "bin"
    if not all((dxc / name).is_file() for name in DXC_FILES.values()):
        url, digest = DXC_ZIP
        archive = download(url, ROOT / "build" / "downloads" / Path(url).name, digest, "DirectX Shader Compiler (SPIR-V)")
        dxc.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive) as z:
            for source, name in DXC_FILES.items():
                with z.open(source) as src, open(dxc / name, "wb") as out:
                    shutil.copyfileobj(src, out)
    progress(1.0)
    say("FFmpeg, DXC, FSR" + (" e DLSS" if dlss else "") + " ok")


def recompile():
    """XenonRecomp: default.xex -> generated/ppc (skipped when the inputs didn't change)."""
    inputs = hashlib.sha256()
    for f in (PRIVATE / "default.xex", ROOT / "config" / "Crash.toml", ROOT / "config" / "Crash_switch_tables.toml", XENON_PATCH):
        inputs.update(sha256(f).encode())
    inputs.update(XENON_COMMIT.encode())
    stamp = GENERATED / ".setup_stamp"
    if stamp.is_file() and stamp.read_text().strip() == inputs.hexdigest() and any(GENERATED.glob("ppc_recomp.*.cpp")):
        say("código recompilado já atualizado (generated/ppc)")
        return
    env = build_env()
    build = XENON / "build"
    # llvm-mingw: XenonRecomp's fmt uses malloc without <cstdlib> under libc++; -static: no toolchain DLLs.
    extra = ["-DCMAKE_CXX_FLAGS=-include cstdlib", "-DCMAKE_EXE_LINKER_FLAGS=-static"] \
        if buildenv.toolchain_kind() == "bundled" else []
    configure = buildenv.configure_build(XENON, build, extra)
    if configure:
        run_all(configure, env=env)
    run_all(["cmake", "--build", build, "--target", "XenonRecomp"], env=env)
    if GENERATED.exists():
        shutil.rmtree(GENERATED)
    GENERATED.mkdir(parents=True)
    recomp = build / "XenonRecomp" / "XenonRecomp.exe"
    for line in run([recomp, ROOT / "config" / "Crash.toml", XENON / "XenonUtils" / "ppc_context.h"], env=env):
        if line.startswith("[phase] analysed"):
            progress(0.3)
    stamp.write_text(inputs.hexdigest())
    say(f"recompilado: {len(list(GENERATED.glob('*.cpp')))} arquivos em generated/ppc")


def dlss_files():
    """Which DLSS files CMake would use now: the SDK, the prebuilt DLL, nvngx_dlss.dll."""
    dlss = ROOT / "thirdparty" / "dlss"
    return " ".join(name for name, path in (("sdk", dlss / "lib" / "nvsdk_ngx_s.lib"), ("prebuilt", dlss / "bin" / "CrashDlss.dll"),
                                              ("nvngx", dlss / "bin" / "nvngx_dlss.dll")) if path.is_file()) or "none"


def compile_project(jobs=None):
    env = build_env()
    build = ROOT / "build"
    configure = buildenv.configure_build(ROOT, build)
    if configure:
        run_all(configure, env=env)
    # CMake looks for the DLSS files when it configures: configure again when they changed (a new
    # GPU, a reinstall with or without DLSS), or the build would copy a DLL that is gone.
    stamp = build / ".crashrecomp_dlss"
    if stamp.is_file() and stamp.read_text().strip() != dlss_files() and not configure:
        run_all(["cmake", "-S", ROOT, "-B", build], env=env)
    stamp.write_text(dlss_files() + "\n")
    args = ["cmake", "--build", build] + (["--parallel", str(jobs)] if jobs else [])
    for line in run(args, env=env):
        # Ninja: "[123/456] Building CXX ..."
        if line.startswith("[") and "/" in line[:16]:
            try:
                done, total = line[1:line.index("]")].split("/")
                progress(int(done) / int(total))
            except ValueError:
                pass


def release_sums(assets_dir):
    """{name: sha256} of the release (SHA256SUMS.txt from --assets-dir or the GitHub release)."""
    local = Path(assets_dir) / RELEASE_SUMS if assets_dir else None
    path = local if local and local.is_file() else download(RELEASE_URL + RELEASE_SUMS, ROOT / "build" / "downloads" / RELEASE_SUMS)
    sums = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            digest, name = line.split(maxsplit=1)
            sums[name.lstrip("*").strip()] = digest.lower()
    return sums


def fetch_asset(name, sums, assets_dir):
    """A release asset: from --assets-dir when it has it (checked), else downloaded."""
    if name not in sums:
        fail(f"{name} não está no {RELEASE_SUMS} da release")
    if assets_dir and (Path(assets_dir) / name).is_file():
        path = Path(assets_dir) / name
        if sha256(path) != sums[name]:
            fail(f"{path} não confere com o {RELEASE_SUMS}")
        return path
    return download(RELEASE_URL + name, ROOT / "build" / "downloads" / name, sums[name])


def unpack(archive):
    """Unpacks a release zip into the project (its paths are relative to the project root)."""
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            target = (ROOT / info.filename).resolve()
            if ROOT.resolve() not in target.parents:
                fail(f"{archive.name}: caminho inválido {info.filename}")
            if info.is_dir():
                continue
            if target.is_file() and target.stat().st_size == info.file_size:
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, open(target, "wb") as out:
                shutil.copyfileobj(src, out)


def install_extras(subtitles, remaster, assets_dir):
    if not (subtitles or remaster):
        say("sem extras (legendas e HD AI Remaster são opcionais)")
        return
    sums = release_sums(assets_dir)
    names = ([SUBTITLES_ASSET] if subtitles else []) + \
            (sorted(n for n in sums if n.startswith(REMASTER_PREFIX)) if remaster else [])
    for i, name in enumerate(names):
        unpack(fetch_asset(name, sums, assets_dir))
        progress((i + 1) / len(names))
        say(f"instalado: {name}")


def play_folder(play_dir):
    """Where the game is played from: dist\\ of the project, or --play-dir (the installer's layout)."""
    return Path(play_dir).resolve() if play_dir else ROOT / "dist"


def game_exe(play_dir):
    return play_folder(play_dir) / ("CrashOTTDE.exe" if play_dir else "CrashRecomp.exe")


def assemble(game, copy_game, play_dir=None):
    game = Path(game).resolve()
    dist_game = (play_folder(play_dir) / "game").resolve()
    args = [sys.executable, ROOT / "tools" / "make_dist.py"]
    if play_dir:
        args += ["--out", play_folder(play_dir), "--installed"]
    if copy_game and game != dist_game:
        files = [f for f in game.rglob("*") if f.is_file()]
        for i, f in enumerate(files):
            target = dist_game / f.relative_to(game)
            if not (target.is_file() and target.stat().st_size == f.stat().st_size):
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(f, target)
            progress((i + 1) / len(files))
        (dist_game / "LEIA.txt").unlink(missing_ok=True)
    elif game != dist_game:
        args += ["--game", game]
    run_all(args)
    precompile_shaders(game if not copy_game else dist_game, play_dir)


def precompile_shaders(game, play_dir=None):
    """Translates and compiles the game's shaders into dist\\userdata\\shadercache (both variants, every
    core), so the first run doesn't compile them while playing (runtime/gpu/shader_warmup.h). Optional: a
    failure only costs that first-run compile time."""
    exe = game_exe(play_dir)
    if not exe.is_file():
        return
    say(f"> {exe.name} --precompile-shaders")
    result = subprocess.run([str(exe), "--game", str(game), "--precompile-shaders"], cwd=exe.parent, capture_output=True, text=True,
                            errors="replace")
    summary = [line for line in result.stdout.splitlines() + result.stderr.splitlines() if "--precompile-shaders" in line]
    if result.returncode == 0:
        say(summary[-1] if summary else "shaders pré-compilados")
    else:
        say(f"aviso: a pré-compilação dos shaders falhou (código {result.returncode}); o jogo compila no primeiro uso")


def extract_assets(game):
    run_all([sys.executable, ROOT / "tools" / "assets.py", "all", "--game", game])


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--game", help="a pasta do seu jogo (default.xex, default.rcf...); padrão: a já configurada")
    p.add_argument("--copy-game", action="store_true", help="copia o jogo para dist\\game (senão, dist aponta para a pasta dele)")
    p.add_argument("--subtitles", action="store_true", help="instala as legendas (transcrição em inglês + PT-BR)")
    p.add_argument("--remaster", action="store_true", help="instala o HD AI Remaster (texturas e modelos, ~4,5 GB)")
    p.add_argument("--assets-dir", help="pasta com os zips da release já baixados (senão, baixa do GitHub)")
    p.add_argument("--extract-assets", action="store_true", help="exporta sons, vídeos, texturas e modelos para private\\assets (modding)")
    p.add_argument("--jobs", type=int, help="processos de compilação em paralelo (padrão: todos os núcleos)")
    p.add_argument("--allow-other-xex", action="store_true", help="continua com um default.xex desconhecido (não suportado)")
    p.add_argument("--assets-only", action="store_true", help="só a exportação de assets (tentar de novo depois de uma falha nela)")
    p.add_argument("--check", action="store_true", help="só confere os pré-requisitos")
    p.add_argument("--toolchain", choices=buildenv.TOOLCHAINS,
                   help="bundled = a pasta toolchain\\ (padrão); msvc = Visual Studio + clang-cl instalados "
                        "(avançado; fica salvo em config/local.toml)")
    p.add_argument("--check-packages", action="store_true", help="só confere os pacotes Python (tools/requirements.txt)")
    p.add_argument("--no-dlss", action="store_true", help="sem DLSS (PC sem placa NVIDIA RTX): não baixa nem instala os arquivos dele")
    p.add_argument("--play-dir", help="a pasta do jogo no layout do instalador (exe, game\\, userdata\\; o projeto fica nela em CrashRecomp\\)")
    args = p.parse_args()
    if args.check_packages:
        return 1 if check_python_packages() else 0
    args.extract_assets |= args.assets_only
    if args.toolchain:
        buildenv.write_local(toolchain=args.toolchain)
        os.environ.pop("CRASH_TOOLCHAIN", None)
    say(f"@@toolchain {buildenv.toolchain_kind()}")

    step(0)
    found = check_prerequisites()
    missing_packages = check_python_packages()
    missing = [name for name, path in found.items() if not path]
    if missing:
        if any(m.startswith("Toolchain") for m in missing):
            fail("falta a pasta toolchain\\ completa (" + ", ".join(missing) + "): ela vem no zip da release; "
                 "num clone do repositório, rode: python tools\\toolchain.py fetch")
        fail("falta instalar: " + ", ".join(missing) + " (README, \"Requisitos\")")
    if args.check:
        say("@@done")
        return 0
    if args.extract_assets and missing_packages:
        # Now, so a problem shows up before the long build; it only fails the asset step (9).
        problem = install_python_packages()
        if problem:
            say(f"aviso: {problem}; o jogo vai ser instalado, mas a exportação de assets vai falhar "
                f"sem eles. Para instalar à mão: {pip_command_text()}")

    game = Path(args.game) if args.game else buildenv.game_folder()
    if game is None:
        fail("diga onde está o jogo: --game <pasta>")
    step(1, str(game))
    check_game(game, args.allow_other_xex)
    if not args.copy_game and game.resolve() != (play_folder(args.play_dir) / "game").resolve():
        buildenv.write_local(game=game)

    if not args.assets_only:
        step(2)
        setup_xenonrecomp()
        step(3)
        fetch_thirdparty(dlss=not args.no_dlss)
        step(4, "default.xex -> C++")
        recompile()
        step(5, "pode levar alguns minutos")
        compile_project(args.jobs)
        step(6)
        install_extras(args.subtitles, args.remaster, args.assets_dir)
        step(7)
        assemble(game, args.copy_game, args.play_dir)
    if game_exe(args.play_dir).is_file():
        say(f"@@game-ready {play_folder(args.play_dir)}")
    step(8)
    if args.extract_assets:
        require_python_packages()
        extract_assets(game)
    else:
        say("exportação de assets não pedida (--extract-assets)")
    say(f"@@finished {play_folder(args.play_dir)}")
    say("@@done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
