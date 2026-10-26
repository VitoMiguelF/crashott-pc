"""Builds the GitHub release assets into release\\<tag>\\ (not in git):

    python tools/make_release.py v1.05                       # everything
    python tools/make_release.py v1.05 --only installer      # only the installer (a code-only version)

- CrashSetup-<tag>.exe: the single-file installer (tools/make_installer.py): CrashSetup.exe with the
  source, XenonRecomp, the toolchain and the third-party DLLs inside, so the install is offline. The
  way players install since v1.05.
- CrashRecomp-<tag>.zip (--only source; the way of v1.0 to v1.04): the source at HEAD (git archive)
  + CrashSetup.exe (the installer, our code only) at the top, + everything the installer needs to
  build, so a player installs nothing:
  toolchain\\ (llvm-mingw, CMake, Ninja, the embeddable Python with numpy and Pillow; fetch it with
  `python tools/toolchain.py fetch`) and tools\\XenonRecomp\\ (its source at setup.py's pinned commit
  with patches/XenonRecomp.patch applied, without .git: no Git needed). No game file, no recompiled
  code: the installer builds everything from the user's dump.
- CrashRecomp-<tag>-legendas.zip: the subtitles (English transcript of the game's voices and the
  Brazilian Portuguese translation), paths relative to the project (tools/setup.py --subtitles).
- CrashRecomp-<tag>-HD-AI-Remaster.partN.zip: the HD AI Remaster mod (hd_textures + rcf), split in
  parts under GitHub's 2 GB limit per file; each part is a normal zip (tools/setup.py --remaster).
- SHA256SUMS.txt: the checksums setup.py verifies.
"""
import argparse
import hashlib
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
PART_LIMIT = 1900 * 1024 * 1024  # bytes of input per remaster part (GitHub: 2 GiB per asset)
REMASTER = ROOT / "private" / "remaster_full" / "mods" / "remaster_full"
REMASTER_PARTS = ["hd_textures", "rcf"]  # what the runtime reads (make_dist.REMASTER_PARTS)
SUBTITLES = [ROOT / "private" / "subtitles" / "english.txt", ROOT / "languages" / "pt-br" / "subtitles.txt"]

REMASTER_README = """HD AI Remaster - CrashRecomp (fan-made, optional)
==================================================

EN: AI-upscaled textures (4x-UltraSharp, up to 2048 px) and Catmull-Clark subdivided models for the
whole game. Experimental: tested in game on L1_E1, L3_E2 and L5_E3 (about 30 fps, no crash); report
problems on the repository. Install with the installer (HD AI REMASTER = ON) or
`python tools\\setup.py --remaster`, then turn on HD AI REMASTER in the launcher or in SETTINGS >
DEFINITIVE EDITION. It needs the game: these files only replace parts of your own default.rcf.

PT-BR: texturas ampliadas por IA (4x-UltraSharp, até 2048 px) e modelos suavizados (Catmull-Clark)
no jogo todo. Experimental: testado no jogo em L1_E1, L3_E2 e L5_E3 (cerca de 30 fps, sem crash);
avise de problemas no repositório. Instale pelo instalador (HD AI REMASTER = LIGADO) ou com
`python tools\\setup.py --remaster` e ligue HD AI REMASTER no launcher ou em SETTINGS > DEFINITIVE
EDITION. Precisa do jogo: estes arquivos só substituem partes do seu próprio default.rcf.
"""


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while block := f.read(1 << 22):
            h.update(block)
    return h.hexdigest()


def xenon_source(prefix):
    """[(file, name in the zip)] of tools/XenonRecomp: the files git tracks there and in its submodules,
    as they are in the working tree (the pinned commit + patches/XenonRecomp.patch, checked)."""
    xenon = ROOT / "tools" / "XenonRecomp"
    sys.path.insert(0, str(ROOT / "tools"))
    import setup  # XENON_COMMIT, XENON_PATCH
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=xenon, capture_output=True, text=True).stdout.strip()
    if head != setup.XENON_COMMIT:
        raise SystemExit(f"tools/XenonRecomp está em {head[:10]}, não em {setup.XENON_COMMIT[:10]}: rode tools/setup.py")
    if subprocess.run(["git", "apply", "--reverse", "--check", "--ignore-whitespace", str(setup.XENON_PATCH)],
                      cwd=xenon, capture_output=True).returncode != 0:
        raise SystemExit("tools/XenonRecomp não tem exatamente o patches/XenonRecomp.patch aplicado")
    names = subprocess.run(["git", "ls-files", "--recurse-submodules", "-z"], cwd=xenon, capture_output=True,
                           text=True, check=True).stdout.split("\0")
    return [(xenon / n, f"{prefix}/tools/XenonRecomp/{n}") for n in names if n and (xenon / n).is_file()]


def zip_files(target, files, arcname, compress=zipfile.ZIP_DEFLATED, extra=None):
    with zipfile.ZipFile(target, "w", compress, compresslevel=6) as z:
        for f in files:
            z.write(f, arcname(f))
        for name, text in (extra or {}).items():
            z.writestr(name, text)
    print(f"{target.name}: {target.stat().st_size / 1048576:.1f} MB", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("tag")
    p.add_argument("--only", choices=["installer", "source", "subtitles", "remaster"], action="append")
    args = p.parse_args()
    out = ROOT / "release" / args.tag
    out.mkdir(parents=True, exist_ok=True)
    only = set(args.only or ["installer", "subtitles", "remaster"])
    prefix = f"CrashRecomp-{args.tag}"

    if "installer" in only:
        subprocess.run([sys.executable, str(ROOT / "tools" / "make_installer.py"), args.tag, "--out", str(out / f"CrashSetup-{args.tag}.exe")],
                       check=True)

    if "source" in only:
        status = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True).stdout
        if status.strip():
            print("aviso: há mudanças não commitadas; o zip usa o HEAD", flush=True)
        source = out / f"{prefix}.zip"
        subprocess.run(["git", "archive", "--format=zip", f"--prefix={prefix}/", "-o", str(source), "HEAD"], cwd=ROOT, check=True)
        setup = ROOT / "build" / "CrashSetup.exe"
        if not setup.is_file():
            raise SystemExit("build\\CrashSetup.exe não existe: cmake --build build --target CrashSetup")
        if toolchain.missing():
            raise SystemExit("toolchain\\ incompleta: python tools\\toolchain.py fetch")
        xenon = xenon_source(prefix)
        with zipfile.ZipFile(source, "a", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            z.write(setup, f"{prefix}/CrashSetup.exe")
            # The stamp setup.py checks: the commit and the SHA-256 of the patch exactly as this zip has it.
            patch = z.read(f"{prefix}/patches/XenonRecomp.patch")
            for f, name in xenon:
                z.write(f, name)
            commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT / "tools" / "XenonRecomp",
                                    capture_output=True, text=True).stdout.strip()
            z.writestr(f"{prefix}/tools/XenonRecomp/.crashrecomp_source",
                       f"{commit} {hashlib.sha256(patch).hexdigest()}\n")
            for f in sorted(toolchain.TOOLCHAIN.rglob("*")):
                if f.is_file() and "_unpack" not in f.parts:
                    z.write(f, f"{prefix}/toolchain/{f.relative_to(toolchain.TOOLCHAIN).as_posix()}")
        print(f"{source.name}: {source.stat().st_size / 1048576:.1f} MB", flush=True)

    if "subtitles" in only:
        missing = [str(f) for f in SUBTITLES if not f.is_file()]
        if missing:
            raise SystemExit("faltam: " + ", ".join(missing))
        zip_files(out / f"{prefix}-legendas.zip", SUBTITLES, lambda f: f.relative_to(ROOT).as_posix())

    if "remaster" in only:
        for old in out.glob(f"{prefix}-HD-AI-Remaster.part*.zip"):
            old.unlink()
        files = sorted(f for part in REMASTER_PARTS for f in (REMASTER / part).rglob("*") if f.is_file())
        parts, current, size = [], [], 0
        for f in files:
            n = f.stat().st_size
            if current and size + n > PART_LIMIT:
                parts.append(current)
                current, size = [], 0
            current.append(f)
            size += n
        if current:
            parts.append(current)
        for i, group in enumerate(parts, 1):
            zip_files(out / f"{prefix}-HD-AI-Remaster.part{i}.zip", group, lambda f: f.relative_to(ROOT).as_posix(),
                      extra={"private/remaster_full/LEIA-README.txt": REMASTER_README} if i == 1 else None)

    sums = out / "SHA256SUMS.txt"
    # The Linux installer (CrashSetup-<tag>-x86_64.AppImage, tools/make_appimage.py): listed when it is
    # here (built on a Linux machine and copied); for a release, .github/workflows/appimage.yml builds it
    # on the tag and adds its line to the release's SHA256SUMS.txt itself.
    lines = [f"{sha256(f)}  {f.name}" for f in sorted([*out.glob("*.zip"), *out.glob("*.exe"), *out.glob("*.AppImage")])]
    sums.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(sums.read_text(encoding="utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
