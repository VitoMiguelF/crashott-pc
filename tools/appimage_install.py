"""The Linux installer inside CrashSetup-<tag>-x86_64.AppImage (tools/make_appimage.py), run by its AppRun
with the bundled Python. The Linux counterpart of CrashSetup.exe, for the Steam Deck's desktop mode first:

 1. asks for the game folder (the player's own dump) and checks it as setup.py does;
 2. asks where to install (default ~/Games/Crash of the Titans Definitive Edition) and whether to add the
    subtitles (a download from the project's release);
 3. copies the project (source, XenonRecomp, toolchain/, the Linux third-party files) from the AppImage
    into <install>/CrashRecomp and runs its tools/setup.py --copy-game --play-dir <install>: the game is
    recompiled and built here, from the player's dump (nothing of the game is in the AppImage);
 4. adds a menu entry (~/.local/share/applications) whose right-click "Add to Steam" puts it in Game Mode.

The dialogs are zenity's or kdialog's (KDE, the Deck's desktop), else the terminal. The log goes to
<install>/setup.log.

    ./CrashSetup-v1.07-x86_64.AppImage                 # the dialogs
    ./CrashSetup-v1.07-x86_64.AppImage --game ~/Crash --dir ~/Games/CrashOTT --yes   # no questions
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

PAYLOAD = Path(__file__).resolve().parents[1]  # the project inside the AppImage (read-only)
sys.path.insert(0, str(PAYLOAD / "tools"))
import setup  # noqa: E402  (GAME_FILES, XEX_SHA256, sha256, STEPS)

TITLE = "Crash of the Titans Definitive Edition"
DEFAULT_DIR = Path.home() / "Games" / TITLE
DESKTOP_ID = "crashrecomp-crashottde.desktop"


class Ui:
    """zenity, kdialog or the terminal."""

    def __init__(self, force_text=False):
        has_display = bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))
        self.kind = "text"
        if has_display and not force_text:
            for tool in ("zenity", "kdialog"):
                if shutil.which(tool):
                    self.kind = tool
                    break
        self.progress_proc = None
        self.qdbus = shutil.which("qdbus6") or shutil.which("qdbus")
        self.kdialog_ref = None

    def _run(self, args):
        return subprocess.run(args, capture_output=True, text=True)

    def info(self, text):
        if self.kind == "zenity":
            self._run(["zenity", "--info", "--title", TITLE, "--width", "460", "--text", text])
        elif self.kind == "kdialog":
            self._run(["kdialog", "--title", TITLE, "--msgbox", text])
        else:
            print(text)

    def error(self, text):
        if self.kind == "zenity":
            self._run(["zenity", "--error", "--title", TITLE, "--width", "460", "--text", text])
        elif self.kind == "kdialog":
            self._run(["kdialog", "--title", TITLE, "--error", text])
        else:
            print("ERRO: " + text, file=sys.stderr)

    def question(self, text):
        if self.kind == "zenity":
            return self._run(["zenity", "--question", "--title", TITLE, "--width", "460", "--text", text]).returncode == 0
        if self.kind == "kdialog":
            return self._run(["kdialog", "--title", TITLE, "--yesno", text]).returncode == 0
        return input(text + " [S/n] ").strip().lower() in ("", "s", "sim", "y", "yes")

    def choose_dir(self, title, start):
        if self.kind == "zenity":
            out = self._run(["zenity", "--file-selection", "--directory", "--title", title, "--filename", f"{start}/"])
        elif self.kind == "kdialog":
            out = self._run(["kdialog", "--title", title, "--getexistingdirectory", str(start)])
        else:
            answer = input(f"{title} [{start}]: ").strip()
            return Path(os.path.expanduser(answer)) if answer else Path(start)
        return Path(out.stdout.strip()) if out.returncode == 0 and out.stdout.strip() else None

    # A progress window: begin, then update(fraction, text), then end.
    def begin(self, text):
        if self.kind == "zenity":
            self.progress_proc = subprocess.Popen(["zenity", "--progress", "--title", TITLE, "--width", "460", "--auto-close",
                                                   "--no-cancel", "--text", text], stdin=subprocess.PIPE, text=True)
        elif self.kind == "kdialog":
            out = self._run(["kdialog", "--title", TITLE, "--progressbar", text, "100"])
            self.kdialog_ref = out.stdout.split() if out.returncode == 0 else None
        else:
            print(text)

    def update(self, fraction, text):
        percent = max(0, min(99, int(fraction * 100)))
        if self.progress_proc and self.progress_proc.poll() is None:
            try:
                self.progress_proc.stdin.write(f"{percent}\n# {text}\n")
                self.progress_proc.stdin.flush()
            except OSError:
                pass
        elif self.kdialog_ref and self.qdbus:
            self._run([self.qdbus, *self.kdialog_ref, "Set", "", "value", str(percent)])
            self._run([self.qdbus, *self.kdialog_ref, "setLabelText", text])
        elif self.kind == "text":
            print(f"[{percent:3}%] {text}", flush=True)

    def end(self):
        if self.progress_proc:
            try:
                self.progress_proc.stdin.write("100\n")
                self.progress_proc.stdin.close()
            except OSError:
                pass
            self.progress_proc.wait()
            self.progress_proc = None
        if self.kdialog_ref and self.qdbus:
            self._run([self.qdbus, *self.kdialog_ref, "close"])
            self.kdialog_ref = None


def check_game(game):
    """None when `game` is the supported dump, else why not."""
    missing = [name for name in setup.GAME_FILES if not (game / name).is_file()]
    if not (game / "shaders").is_dir():
        missing.append("shaders/")
    if missing:
        return f"{game} não parece o Crash of the Titans de Xbox 360: falta {', '.join(missing)}."
    if setup.sha256(game / "default.xex") != setup.XEX_SHA256:
        return "Esse default.xex é de outra versão do jogo, que o CrashRecomp não suporta."
    return None


def copy_project(target, ui):
    """The AppImage's project into target (files that are already there with the same size are kept)."""
    files = [f for f in PAYLOAD.rglob("*") if f.is_file() or f.is_symlink()]
    for i, f in enumerate(files):
        rel = f.relative_to(PAYLOAD)
        dest = target / rel
        if f.is_symlink():
            if dest.is_symlink() or dest.exists():
                dest.unlink()
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.symlink_to(os.readlink(f))
        elif not (dest.is_file() and dest.stat().st_size == f.stat().st_size):
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, dest)
        if i % 200 == 0:
            ui.update(0.15 * i / len(files), f"Copiando os arquivos de instalação ({i}/{len(files)})")


def run_setup(project, install, game, subtitles, ui, log_path):
    """tools/setup.py of the installed project, its @@ lines turned into the progress window."""
    python = project / "toolchain" / "python" / "bin" / "python3"
    args = [str(python), "-I", str(project / "tools" / "setup.py"), "--game", str(game), "--copy-game",
            "--play-dir", str(install)] + (["--subtitles"] if subtitles else [])
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    for key in ("PYTHONHOME", "PYTHONPATH"):
        env.pop(key, None)
    step, steps, step_text, error = 0, len(setup.STEPS), "", None
    with open(log_path, "a", encoding="utf-8") as log:
        log.write("> " + " ".join(args) + "\n")
        proc = subprocess.Popen(args, cwd=project, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                encoding="utf-8", errors="replace")
        for line in proc.stdout:
            log.write(line)
            log.flush()
            line = line.rstrip("\n")
            if m := re.match(r"@@step (\d+) (\d+) (.*)", line):
                step, steps, step_text = int(m.group(1)) - 1, int(m.group(2)), m.group(3)
                ui.update(0.15 + 0.85 * step / steps, f"{step + 1}/{steps}: {step_text}")
            elif m := re.match(r"@@progress ([\d.]+)", line):
                ui.update(0.15 + 0.85 * (step + float(m.group(1))) / steps, f"{step + 1}/{steps}: {step_text}")
            elif line.startswith("@@error "):
                error = line[len("@@error "):]
            elif ui.kind == "text" and not line.startswith("@@"):
                print("   " + line)
        code = proc.wait()
    return None if code == 0 else (error or f"o setup.py saiu com o código {code}")


def write_desktop_entry(install, project):
    """The menu entry (Exec quoted: the path has spaces). Returns its path."""
    folder = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share") / "applications"
    folder.mkdir(parents=True, exist_ok=True)
    exe = install / "CrashOTTDE"
    icon = project / "crashrecomp.png"

    def quote(path):
        return '"' + str(path).replace("\\", "\\\\").replace('"', '\\"').replace("`", "\\`").replace("$", "\\$") + '"'
    entry = folder / DESKTOP_ID
    entry.write_text("[Desktop Entry]\nType=Application\n"
                     f"Name={TITLE}\nComment=Crash of the Titans (Xbox 360) recompilado para Linux (CrashRecomp)\n"
                     f"Exec={quote(exe)}\nPath={install}\nIcon={icon}\nTerminal=false\nCategories=Game;\n",
                     encoding="utf-8")
    entry.chmod(0o755)
    return entry


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--game", help="a pasta do seu jogo (senão, pergunta)")
    p.add_argument("--dir", help=f"onde instalar (padrão: {DEFAULT_DIR})")
    p.add_argument("--subtitles", action="store_true", help="instala as legendas sem perguntar")
    p.add_argument("--yes", action="store_true", help="sem perguntas: as respostas padrão")
    p.add_argument("--text", action="store_true", help="no terminal, sem janelas")
    args = p.parse_args()
    ui = Ui(force_text=args.text)

    game = Path(args.game).expanduser() if args.game else None
    while game is None or (problem := check_game(game)):
        if game is not None:
            ui.error(problem)
            if args.game:
                return 1
        game = ui.choose_dir("Escolha a pasta do seu Crash of the Titans (default.xex, default.rcf...)", Path.home())
        if game is None:
            return 1

    install = Path(args.dir).expanduser() if args.dir else DEFAULT_DIR
    if not args.dir and not args.yes and not ui.question(f"Instalar em:\n{install}\n\n(Não = escolher outra pasta)"):
        parent = ui.choose_dir("Escolha onde criar a pasta do jogo", Path.home())
        if parent is None:
            return 1
        install = parent / TITLE
    subtitles = args.subtitles or (not args.yes and ui.question(
        "Instalar as legendas (inglês e português)?\n\nSão baixadas da página do projeto (precisa de internet)."))

    install = install.resolve()
    project = install / "CrashRecomp"
    project.mkdir(parents=True, exist_ok=True)
    log_path = install / "setup.log"
    ui.begin("Preparando a instalação...")
    copy_project(project, ui)
    tag = (PAYLOAD / ".crashrecomp_version").read_text().strip() if (PAYLOAD / ".crashrecomp_version").is_file() else ""
    if tag:
        (install / ".crashrecomp_version").write_text(tag + "\n")
    error = run_setup(project, install, game, subtitles, ui, log_path)
    ui.end()
    if error:
        ui.error(f"A instalação falhou: {error}\n\nO registro completo está em:\n{log_path}")
        return 1
    entry = write_desktop_entry(install, project)
    message = (f"Pronto! O jogo foi instalado em:\n{install}\n\n"
               f"Ele está no menu de aplicativos (Jogos) como \"{TITLE}\".\n"
               "No Steam Deck: no modo desktop, clique com o botão direito nele no menu e escolha "
               "\"Adicionar à Steam\" para jogar no modo de jogo.")
    if args.yes:
        print(message)
        print(f"menu: {entry}")
        return 0
    if ui.question(message + "\n\nAbrir o jogo agora?"):
        subprocess.Popen([str(install / "CrashOTTDE")], cwd=install, start_new_session=True,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return 0


if __name__ == "__main__":
    sys.exit(main())
