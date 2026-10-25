"""Assembles dist\\, the folder to play from: the exe and its DLLs together with mods\\, userdata\\,
languages\\, the optional data derived from your game and a game\\ folder for the dump.

    python tools/make_dist.py                         # updates dist\\ from build\\CrashRecomp.exe
    python tools/make_dist.py --game D:\\Jogos\\Crash   # also tells the game where your dump is
    python tools/make_dist.py --no-remaster           # without the HD AI Remaster data
    cmake --build build --target dist                 # the same, after building

dist\\
    CrashLauncher.exe               the launcher: the options with the game's look, then PLAY
    CrashRecomp.exe                 the game (our code + the recompiled code)
    *.dll                           FFmpeg (LGPL), NVIDIA DLSS, AMD FidelityFX (licenses in licencas\\)
    game\\                           your game dump (default.xex, default.rcf...), or [paths] game in
                                    userdata\\settings.toml (--game; the exe also asks on the first run)
    mods\\                           mods (kept as they are)
    userdata\\                       saves, settings.toml, shader cache (kept as they are)
    languages\\                      language packs (PT-BR), copied from the project
    data\\                           optional, derived from YOUR game, never share: subtitles\\english.txt,
                                    symbols\\functions.csv, remaster_full\\ (HD AI Remaster, hard links)
    licencas\\                       third-party licenses
    LEIA.txt

Updated on every run: the exe, the DLLs, languages\\, data\\, licencas\\ and LEIA.txt. Never touched:
game\\, mods\\ and userdata\\ (only created when missing). Fails if dist\\CrashRecomp.exe is running.

--installed (the single-file installer, through setup.py --play-dir): the play folder is the install
folder and holds only what the player uses; the rest goes to its CrashRecomp\\ subfolder, which is
also the project the game was built from (the runtime finds languages\\ and data\\ there, paths.h):

    <install>\\                          "Crash of the Titans Definitive Edition"
        CrashLauncher.exe, CrashOTTDE.exe (the game: build\\CrashRecomp.exe renamed), *.dll
        game\\ (the dump, always copied), userdata\\, mods\\
        CrashRecomp\\                    languages\\, data\\, licencas\\ + the project (toolchain\\, build\\...)
No LEIA.txt files there.

On Linux the same, with CrashRecomp / CrashOTTDE and CrashLauncher (no .exe) and the .so files the build
put next to the exe (FFmpeg, DXC, SDL3; the exes find them with their $ORIGIN rpath) instead of the DLLs.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PRIVATE = ROOT / "private"
WINDOWS = sys.platform == "win32"
EXE = ".exe" if WINDOWS else ""

LICENSES = {
    "CrashRecomp_LICENSE.txt": ROOT / "LICENSE",
    "THIRD_PARTY_NOTICES.md": ROOT / "THIRD_PARTY_NOTICES.md",
    "ffmpeg_LICENSE.txt": ROOT / "thirdparty" / "ffmpeg" / "LICENSE.txt",
    "ffmpeg_README.md": ROOT / "thirdparty" / "ffmpeg" / "README.md",
    "dlss_LICENSE.txt": ROOT / "thirdparty" / "dlss" / "LICENSE.txt",
    "fsr_license.md": ROOT / "thirdparty" / "fsr" / "license.md",
    "dxc_LICENSE-MS.txt": ROOT / "thirdparty" / "dxc" / "bin" / "LICENSE-MS.txt",
    "dxc_LICENSE-LLVM.txt": ROOT / "thirdparty" / "dxc" / "bin" / "LICENSE-LLVM.txt",
    "smaa_LICENSE.txt": ROOT / "thirdparty" / "smaa" / "LICENSE.txt",
    "imgui_LICENSE.txt": ROOT / "thirdparty" / "imgui" / "LICENSE.txt",
    "sdl3_copyright.txt": ROOT / "thirdparty" / "sdl3" / "copyright",
    "freetype_LICENSE.txt": ROOT / "thirdparty" / "freetype" / "LICENSE.TXT",
    "freetype_FTL.txt": ROOT / "thirdparty" / "freetype" / "FTL.TXT",
}
# HD AI Remaster: the mod folders the runtime looks for under data\ (runtime/settings/hd_remaster.cpp),
# and the parts of them it reads.
REMASTERS = ["remaster_full/mods/remaster_full", "remaster_l1/mods_final/remaster_l1_final"]
REMASTER_PARTS = ["rcf", "hd_textures"]

LEIA = """CrashRecomp - Crash of the Titans (Xbox 360) recompilado para Windows
=====================================================================

Para jogar: CrashLauncher.exe (as opções, com a cara do jogo, e JOGAR) ou CrashRecomp.exe direto.

Pastas
  game\\       o seu jogo (default.xex, default.rcf, english.rcf, sound1.rcf...). Se o jogo
              estiver em outra pasta, ela fica em userdata\\settings.toml ([paths] game); na
              primeira vez sem jogo o CrashRecomp pergunta onde ele está.
  mods\\       mods (um por pasta; veja o README do projeto, "Mods").
  userdata\\   seus saves, settings.toml (as opções da tela SETTINGS) e o cache de shaders.
  languages\\  pacotes de idioma (Português do Brasil).
  data\\       opcional, gerado a partir do SEU jogo: texto das legendas, nomes de funções para
              relatórios de erro, HD AI Remaster. Sem esta pasta o jogo roda igual, sem esses extras.
  licencas\\   licenças do FFmpeg, DLSS, FidelityFX, SMAA e Dear ImGui.

Compartilhar
  Esta pasta é a sua instalação: não a publique. O CrashRecomp.exe contém o código do jogo
  recompilado e game\\ é o próprio jogo. Para outra pessoa jogar, mande o link do projeto
  (https://github.com/VitoMiguelF/crashott-pc): o instalador monta tudo a partir da cópia dela.
  As legendas e o HD AI Remaster de data\\ vêm das releases do projeto; mods feitos a partir
  dos arquivos do jogo são conteúdo derivado.

Opções de linha de comando (normalmente desnecessárias): --game, --mods, --userdata,
--languages, --data <pasta>. Atalhos no jogo: F1 overlay, F12 captura, Alt+Enter tela cheia.
"""

GAME_LEIA = """Coloque aqui os arquivos do seu Crash of the Titans (Xbox 360): default.xex, default.rcf,
english.rcf, sound1.rcf, sound2.rcf, shaders\\... (a pasta inteira do jogo).

Se o jogo ficar em outra pasta, não precisa copiar: o CrashRecomp usa [paths] game de
userdata\\settings.toml (ou pergunta a pasta na primeira vez). Os arquivos do jogo nunca são alterados.
"""

MODS_LEIA = """Mods do CrashRecomp: uma pasta por mod (mods\\<nome>\\...). load_order.txt (opcional) define a ordem.
Veja o README do projeto: "Mods de entradas de .rcf", "Extraindo e modificando assets" e
"Texturas HD sem regravar pacotes". Mods feitos a partir dos arquivos do jogo são só para o seu uso.
"""


LEIA_LINUX = """CrashRecomp - Crash of the Titans (Xbox 360) recompilado para Linux
===================================================================

Para jogar: ./CrashLauncher (as opções, com a cara do jogo, e JOGAR) ou ./CrashRecomp direto.
No Steam Deck, adicione o CrashLauncher à Steam como jogo não-Steam.

Pastas
  game/       o seu jogo (default.xex, default.rcf, english.rcf, sound1.rcf...). Se o jogo
              estiver em outra pasta, ela fica em userdata/settings.toml ([paths] game).
  mods/       mods (um por pasta; veja o README do projeto, "Mods").
  userdata/   seus saves, settings.toml (as opções da tela SETTINGS) e o cache de shaders.
  languages/  pacotes de idioma (Português do Brasil).
  data/       opcional, gerado a partir do SEU jogo (legendas, nomes de funções, HD AI Remaster).
  licencas/   licenças do FFmpeg, DXC, SDL, FreeType, SMAA e Dear ImGui.

Compartilhar
  Esta pasta é a sua instalação: não a publique. O CrashRecomp contém o código do jogo
  recompilado e game/ é o próprio jogo. Para outra pessoa jogar, mande o link do projeto
  (https://github.com/VitoMiguelF/crashott-pc): o instalador monta tudo a partir da cópia dela.

Atalhos no jogo: F1 overlay, F12 captura, Alt+Enter tela cheia.
"""

# The game's exe in the installed layout (--installed).
INSTALLED_EXE = "CrashOTTDE" + EXE


def running(exe):
    """Whether a process is running from `exe` (PowerShell Get-Process gives the image paths; /proc on Linux)."""
    if not WINDOWS:
        target = str(exe.resolve())
        for proc in Path("/proc").glob("[0-9]*"):
            try:
                if os.readlink(proc / "exe") == target:
                    return True
            except OSError:
                pass
        return False
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command",
                              "Get-Process CrashRecomp, CrashOTTDE -ErrorAction SilentlyContinue | ForEach-Object { $_.Path }"],
                             capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.TimeoutExpired):
        return False
    target = os.path.normcase(str(exe.resolve()))
    return any(os.path.normcase(line.strip()) == target for line in out.splitlines() if line.strip())


def copy_if_newer(source, target):
    if target.exists() and target.stat().st_size == source.stat().st_size and target.stat().st_mtime >= source.stat().st_mtime:
        return False
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    return True


def mirror(source, target, link=False):
    """Makes `target` hold exactly the files of `source` (hard links when `link`, else copies)."""
    count = 0
    wanted = set()
    for file in source.rglob("*"):
        if not file.is_file():
            continue
        rel = file.relative_to(source)
        wanted.add(rel)
        dest = target / rel
        if link:
            if dest.exists() and os.path.samefile(dest, file):
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            if dest.exists():
                dest.unlink()
            try:
                os.link(file, dest)
            except OSError:
                shutil.copy2(file, dest)
            count += 1
        elif copy_if_newer(file, dest):
            count += 1
    if target.is_dir():
        for file in sorted(target.rglob("*"), reverse=True):
            rel = file.relative_to(target)
            if file.is_file() and rel not in wanted:
                file.unlink()
            elif file.is_dir() and not any(file.iterdir()):
                file.rmdir()
    return count


def set_game_folder(userdata, game):
    """Writes [paths] game into userdata\\settings.toml (the runtime keeps it when it rewrites the file)."""
    settings = userdata / "settings.toml"
    line = f'game = "{game.resolve().as_posix()}"   # pasta do jogo (default.xex, default.rcf); sem esta linha: a pasta game\\ ao lado do exe\n'
    text = settings.read_bytes().decode("utf-8") if settings.exists() else ""
    lines = text.splitlines(keepends=True)
    out, section, done = [], "", False
    for raw in lines:
        stripped = raw.strip()
        if stripped.startswith("["):
            if section == "paths" and not done:
                out.append(line)
                done = True
            section = stripped[1:stripped.find("]")].strip().lower()
        elif section == "paths" and stripped.split("=", 1)[0].strip().lower() == "game":
            if not done:
                out.append(line)
                done = True
            continue
        out.append(raw)
    if section == "paths" and not done:
        out.append(line)
        done = True
    if not done:
        # A new [paths] table at the top, after the header comments.
        head = 0
        while head < len(out) and (out[head].startswith("#") or not out[head].strip()):
            head += 1
        out[head:head] = ["[paths]\n", line, "\n"]
    settings.write_bytes("".join(out).encode("utf-8"))  # LF, like the runtime writes it


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="Monta a pasta dist\\ (o jogo pronto para jogar) a partir do build.")
    parser.add_argument("--exe", default=str(ROOT / "build" / ("CrashRecomp" + EXE)), help="o CrashRecomp.exe compilado (padrão: build\\CrashRecomp.exe)")
    parser.add_argument("--out", default=str(ROOT / "dist"), help="pasta de saída (padrão: dist)")
    parser.add_argument("--game", help="pasta do seu jogo: grava [paths] game no userdata\\settings.toml de dist")
    parser.add_argument("--no-remaster", action="store_true", help="não põe o HD AI Remaster em data\\")
    parser.add_argument("--no-data", action="store_true", help="sem data\\ (legendas, símbolos, remaster)")
    parser.add_argument("--installed", action="store_true",
                        help="o layout do instalador: só o que o jogador usa em --out, o resto em --out\\CrashRecomp")
    args = parser.parse_args()

    exe = Path(args.exe).resolve()
    out = Path(args.out).resolve()
    support = out / "CrashRecomp" if args.installed else out  # languages\, data\, licencas\
    game_exe = out / (INSTALLED_EXE if args.installed else "CrashRecomp" + EXE)
    if not exe.is_file():
        raise SystemExit(f"não achei {exe}: compile antes (cmake --build build)")
    if running(game_exe):
        raise SystemExit(f"{game_exe} está aberto: feche o jogo e rode de novo")
    out.mkdir(parents=True, exist_ok=True)

    launcher = exe.parent / "CrashLauncher.exe"
    if WINDOWS:
        files = [exe, *([launcher] if launcher.is_file() else []), *sorted(exe.parent.glob("*.dll"))]
        if not launcher.is_file():
            print(f"aviso: {launcher} não existe (cmake --build build --target CrashLauncher)")
    else:
        launcher = exe.parent / "CrashLauncher"
        files = [exe, *([launcher] if launcher.is_file() else []), *sorted(f for f in exe.parent.glob("*.so*") if f.is_file())]
        if not launcher.is_file():
            print(f"aviso: {launcher} não existe (cmake --build build --target CrashLauncher)")
    changed = [f.name for f in files if copy_if_newer(f, game_exe if f == exe else out / f.name)]
    print(f"exe e DLLs: {', '.join(changed) if changed else 'já atualizados'}")
    if (support / "languages").resolve() != (ROOT / "languages").resolve():
        print(f"languages\\: {mirror(ROOT / 'languages', support / 'languages')} arquivo(s) atualizados")

    for folder, text in (("game", GAME_LEIA), ("mods", MODS_LEIA)):
        (out / folder).mkdir(exist_ok=True)
        if not args.installed and not any((out / folder).iterdir()):
            (out / folder / "LEIA.txt").write_text(text, encoding="utf-8")
    (out / "userdata").mkdir(exist_ok=True)

    data = support / "data"
    if args.no_data:
        print("data\\: não incluída (--no-data)")
    else:
        files = {"subtitles/english.txt": PRIVATE / "subtitles" / "english.txt",
                 "symbols/functions.csv": PRIVATE / "symbols" / "functions.csv"}
        for rel, source in files.items():
            if source.is_file():
                copy_if_newer(source, data / rel)
            else:
                print(f"data\\{rel}: {source} não existe (opcional)")
        # Only the remaster the runtime would pick (the whole game before level 1).
        remaster = None if args.no_remaster else next((r for r in REMASTERS if (PRIVATE / r / "rcf").is_dir() or (PRIVATE / r / "hd_textures").is_dir()), None)
        for r in REMASTERS:
            top = data / r.split("/")[0]
            if r != remaster and top.exists():
                shutil.rmtree(top)
        if remaster:
            linked = sum(mirror(PRIVATE / r / part, data / remaster / part, link=True) for r in [remaster] for part in REMASTER_PARTS if (PRIVATE / r / part).is_dir())
            print(f"data\\{remaster.split('/')[0]}: HD AI Remaster ({linked} arquivo(s) ligados por hard link, sem cópia)")
        print("data\\: legendas, símbolos" + (", HD AI Remaster" if remaster else ""))

    licenses = support / "licencas"
    for name, source in LICENSES.items():
        if source.is_file():
            copy_if_newer(source, licenses / name)
    if not args.installed:
        (out / "LEIA.txt").write_text(LEIA if WINDOWS else LEIA_LINUX, encoding="utf-8")

    if args.game:
        game = Path(args.game)
        if not ((game / "default.xex").is_file() and (game / "default.rcf").is_file()):
            raise SystemExit(f"{game} não tem default.xex e default.rcf")
        set_game_folder(out / "userdata", game)
        print(f"jogo: {game} (userdata\\settings.toml, [paths] game)")
    print(f"pronto: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
