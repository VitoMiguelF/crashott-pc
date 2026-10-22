"""CrashRecomp installer: from your own Crash of the Titans (Xbox 360) dump to a folder ready to play.

    python tools/setup.py --game D:\\Games\\Crash                  # build everything, play from dist\\
    python tools/setup.py --game D:\\Games\\Crash --subtitles      # + subtitles (EN transcript, PT-BR)
    python tools/setup.py --game D:\\Games\\Crash --remaster       # + HD AI Remaster (4.5 GB)
    python tools/setup.py --game D:\\Games\\Crash --extract-assets # + export the game's assets for modding
    python tools/setup.py --check                                 # only check the prerequisites

Steps (each one is skipped when its result is already up to date, so running it again is cheap):
 1. prerequisites: Visual Studio 2022+ / Build Tools (C++), LLVM clang-cl, CMake, Ninja, Git;
 2. the dump: default.xex must be the known version (SHA-256 below); default.rcf, english.rcf,
    sound1.rcf, sound2.rcf and shaders\\ must be there. It is never written to;
 3. XenonRecomp: cloned at a pinned commit into tools\\XenonRecomp, + patches\\XenonRecomp.patch;
 4. third-party SDK files (FFmpeg LGPL, NVIDIA DLSS, AMD FSR), from their official sources, checked
    by SHA-256 (thirdparty\\*\\README.md);
 5. XenonRecomp turns default.xex into C++ (generated\\ppc, ~60 MB, never shared);
 6. CMake + Ninja build CrashRecomp.exe, CrashLauncher.exe and CrashSetup.exe (build\\);
 7. optional release assets (--subtitles, --remaster): downloaded from the GitHub release (or taken
    from --assets-dir), checked against its SHA256SUMS.txt and unpacked into private\\ / languages\\;
 8. tools\\make_dist.py assembles dist\\ (the folder to play), pointing it at your dump (or copying
    the dump into dist\\game with --copy-game);
 9. optional: tools\\assets.py all exports the game's sounds, videos, textures and models to
    private\\assets for modding (README, "Extracting and modifying assets").

Machine-readable progress for the graphical installer (installer/): lines starting with "@@".
"""
import argparse
import hashlib
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

# Crash of the Titans (Xbox 360, NTSC/PAL disc release, title 565507E4): the only default.xex the
# addresses in config/Crash.toml are for.
XEX_SHA256 = "e76824b242c6c628738c373bdf052d456945d8e343eb934100617fd423e86373"
GAME_FILES = ["default.xex", "default.rcf", "english.rcf", "sound1.rcf", "sound2.rcf"]

# The release that holds the data assets (subtitles, HD AI Remaster) and their SHA256SUMS.txt.
# They haven't changed since v1.0: later releases (v1.01) only publish the source + installer zip,
# and setup keeps downloading the data from v1.0. Bump it only when the data assets are rebuilt.
RELEASE_TAG = "v1.0"
RELEASE_URL = f"https://github.com/VitoMiguelF/crashott-pc/releases/download/{RELEASE_TAG}/"
RELEASE_SUMS = "SHA256SUMS.txt"
SUBTITLES_ASSET = "CrashRecomp-v1.0-legendas.zip"
REMASTER_PREFIX = "CrashRecomp-v1.0-HD-AI-Remaster.part"

DLSS = "https://github.com/NVIDIA/DLSS/raw/v310.9.16/"
FSR = "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/raw/v2.3.0/Kits/FidelityFX/"
# (url, destination, sha256)
DOWNLOADS = [
    (DLSS + "include/nvsdk_ngx.h", "thirdparty/dlss/include/nvsdk_ngx.h", "dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848"),
    (DLSS + "include/nvsdk_ngx_defs.h", "thirdparty/dlss/include/nvsdk_ngx_defs.h", "ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e"),
    (DLSS + "include/nvsdk_ngx_helpers.h", "thirdparty/dlss/include/nvsdk_ngx_helpers.h", "5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471"),
    (DLSS + "include/nvsdk_ngx_helpers_cuda.h", "thirdparty/dlss/include/nvsdk_ngx_helpers_cuda.h", "f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042"),
    (DLSS + "include/nvsdk_ngx_helpers_d3d.h", "thirdparty/dlss/include/nvsdk_ngx_helpers_d3d.h", "ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba"),
    (DLSS + "include/nvsdk_ngx_params.h", "thirdparty/dlss/include/nvsdk_ngx_params.h", "943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc"),
    (DLSS + "lib/Windows_x86_64/x64/nvsdk_ngx_s.lib", "thirdparty/dlss/lib/nvsdk_ngx_s.lib", "7b9a5239c1145af5f645ab472aa066f9ea79db36ac819562312aae69d65f3765"),
    (DLSS + "lib/Windows_x86_64/rel/nvngx_dlss.dll", "thirdparty/dlss/bin/nvngx_dlss.dll", "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e"),
    (FSR + "api/include/ffx_api.h", "thirdparty/fsr/Kits/FidelityFX/api/include/ffx_api.h", "e1a9d1b559eaf75f4cb401f1c43a66e2f3dc9621abfeb58b9c6fac56443177fd"),
    (FSR + "api/include/ffx_api_types.h", "thirdparty/fsr/Kits/FidelityFX/api/include/ffx_api_types.h", "cc0adc0ffc804dd7cd03c39c15581b575e416383177ffe95060201d52cc0fc49"),
    (FSR + "api/include/ffx_api_loader.h", "thirdparty/fsr/Kits/FidelityFX/api/include/ffx_api_loader.h", "18d1901dbbc881e8131b91c65ad44bb3b4a1de6c31435d5b75236f16727ba80e"),
    (FSR + "api/include/dx12/ffx_api_dx12.h", "thirdparty/fsr/Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h", "c9afbc3c4c673723e9e5369a666f0ff54560543b5a93537f8ff453bf1c3bf530"),
    (FSR + "upscalers/include/ffx_upscale.h", "thirdparty/fsr/Kits/FidelityFX/upscalers/include/ffx_upscale.h", "65be36d5653eb71c10379e0204f7bb7cd6a8aeec96e6f8cffc5f59ffeb1f89bf"),
    (FSR + "signedbin/amd_fidelityfx_loader_dx12.dll", "thirdparty/fsr/bin/amd_fidelityfx_loader_dx12.dll", "e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa"),
    (FSR + "signedbin/amd_fidelityfx_upscaler_dx12.dll", "thirdparty/fsr/bin/amd_fidelityfx_upscaler_dx12.dll", "d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46"),
]
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
    """The environment of vcvars64.bat + the tool folders (computed once)."""
    global _ENV
    if _ENV is None:
        lines = buildenv.cmd_prelude() + ["set"]
        script = ROOT / "build" / "setup_env.cmd"
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_text("@echo off\r\n" + "\r\n".join(lines) + "\r\n", encoding="ascii")
        out = subprocess.run(["cmd", "/d", "/c", str(script)], capture_output=True, text=True, errors="replace")
        if out.returncode != 0:
            fail("vcvars64.bat falhou: confira a instalação do Visual Studio (C++)")
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

def check_prerequisites():
    """Returns {name: path or None}; the setup needs all of them."""
    found = {
        "Visual Studio (C++)": buildenv.vcvars(),
        "clang-cl (LLVM)": buildenv.which("clang-cl"),
        "CMake": buildenv.which("cmake"),
        "Ninja": buildenv.which("ninja"),
        "Git": buildenv.which("git"),
    }
    for name, path in found.items():
        say(f"@@prereq {'ok' if path else 'missing'} {name} | {path or ''}")
    if sys.version_info < (3, 11):
        say("@@prereq missing Python 3.11+ |")
        found["Python 3.11+"] = None
    else:
        say(f"@@prereq ok Python | {sys.executable}")
    return found


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
    git = buildenv.which("git")
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


def fetch_thirdparty():
    total = len(DOWNLOADS) + 1
    for i, (url, dest, digest) in enumerate(DOWNLOADS):
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
    progress(1.0)
    say("FFmpeg, DLSS e FSR ok")


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
    if not (build / "build.ninja").is_file():
        run_all(["cmake", "-S", XENON, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                 "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"], env=env)
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


def compile_project(jobs=None):
    env = build_env()
    build = ROOT / "build"
    if not (build / "build.ninja").is_file():
        run_all(["cmake", "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                 "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"], env=env)
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


def assemble(game, copy_game):
    game = Path(game).resolve()
    dist_game = (ROOT / "dist" / "game").resolve()
    args = [sys.executable, ROOT / "tools" / "make_dist.py"]
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
    p.add_argument("--check", action="store_true", help="só confere os pré-requisitos")
    args = p.parse_args()

    step(0)
    found = check_prerequisites()
    missing = [name for name, path in found.items() if not path]
    if missing:
        fail("falta instalar: " + ", ".join(missing) + " (README, \"Requisitos\")")
    if args.check:
        say("@@done")
        return 0

    game = Path(args.game) if args.game else buildenv.game_folder()
    if game is None:
        fail("diga onde está o jogo: --game <pasta>")
    step(1, str(game))
    check_game(game, args.allow_other_xex)
    if not args.copy_game and game.resolve() != (ROOT / "dist" / "game").resolve():
        buildenv.write_local(game=game)

    step(2)
    setup_xenonrecomp()
    step(3)
    fetch_thirdparty()
    step(4, "default.xex -> C++")
    recompile()
    step(5, "pode levar de 5 a 20 minutos")
    compile_project(args.jobs)
    step(6)
    install_extras(args.subtitles, args.remaster, args.assets_dir)
    step(7)
    assemble(game, args.copy_game)
    step(8)
    if args.extract_assets:
        extract_assets(game)
    else:
        say("exportação de assets não pedida (--extract-assets)")
    say(f"@@finished {ROOT / 'dist'}")
    say("@@done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
