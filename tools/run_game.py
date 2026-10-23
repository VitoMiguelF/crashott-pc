"""Runs CrashRecomp.exe with a profile from config/run.toml and checks the result.

    python tools/run_game.py <profile> [--set key=value ...] [--no-build]
    python tools/run_game.py --all          # every profile with test = true
    python tools/run_game.py --list

Each run gets its own folder under [paths].runs with run.log, the screenshots,
audio.wav (if enabled), build.log, frametimes.csv (one line per frame), marks.txt
(frames marked with F2), replay.txt (the profile's `replay`, for the runtime) and
result.json. Exit code: 0 if every check passed, 1 otherwise.

Restarts: the game is started with CRASH_RESTART=exit, so a restart from the settings
screen ends the process with code 4 instead of relaunching it out of the runner's sight;
the runner then starts it again itself (same arguments plus --restarted, same userdata)
and keeps tracking it: run.log goes on, the screenshots and frame times of launch N go to
reinicio_N/. The profile's restart_input / restart_env / restart_timeout drive the new
launch (by default: no input, the same timeout); max_restarts (default 3) caps the loop.

Profiles recorded while playing (F5, or the `gravar` profile) are written by the
runtime to config/recorded/<name>.toml and are listed and run like the others.

Boot and events (runtime/settings/direct_boot.h): `boot = "menu" | "level:L1_E2" |
"load:<slot>[:continue|hub|L1_E2]"` (CRASH_BOOT) goes past the intros and the menus through the
game's own code, `skip_intros = true` (CRASH_SKIP_INTROS) only skips the logos and the title,
`skip_cinematics = true` (CRASH_SKIP_CINEMATICS) ends every in-game cutscene as soon as it plays.
Dev features (runtime/dev/dev_tools.h, docs/dev_features.md): `dev_lua = "<lua>"`,
`dev_lua_file = "path"`, `dev_lua_at = "gameplay_ready+2"`, `dev_invincible = true`,
`dev_teleport = [x, y, z]`, `dev_mojo = 500`, `dev_unlock_all = true`, `dev_shadow = true`,
`dev_gamelink = true`; each block logs the event dev_lua_<n>. The developer console
(runtime/dev/dev_console.h): `dev_console = [["@gameplay_ready+1", "!toggle"], [..., "god on"]]`
types each entry into the window at its time (a command + Enter, or !toggle / !esc / !tab / !up /
!down / !enter / !type:<text>) and turns the console on; events console_<n> and console_cmd. The
runtime logs milestones as "[test] event <name> (...)": title, main_menu, save_slots, load_complete,
hub, level_start, level_ready, cinematic, skip_prompt (the "hold to skip" prompt of a cutscene or
an intro video appears), cinematic_skipped, video_skipped, cinematic_done, gameplay_ready, pause,
game_over, boot_done, boot_failed. In `input`, an
entry ["@<event>"] (or ["@<event>#n"], the n-th one) makes the following times relative to that
event; ["@start"] goes back to the input clock. [t, "SHOT"] takes a screenshot. In `replay`,
[t, "EVENT", "<event>"] waits for the event and re-aligns the timeline on it. `expect.events`
lists events that must happen (`expect.events_absent` ones that must not; boot_failed always is).
The summary prints when each event happened.

Userdata: every test (userdata_mode = "copy", the default) runs on a fresh copy of the fixed test
base, tests/userdata_base ([paths].test_userdata); only `jogar` ("shared") uses the player's
userdata (dist/userdata). --dist runs dist/CrashRecomp.exe (tools/make_dist.py) with only
--userdata, so the exe finds the game, mods, languages and data by itself, as when the player
double-clicks it.
"""
import argparse
import datetime
import csv
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_dist import set_game_folder  # noqa: E402
import buildenv  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "config" / "run.toml"
# Profiles recorded with F5 (runtime/kernel/input_record.cpp), one file each.
RECORDED = ROOT / "config" / "recorded"

# Process exit codes set by runtime/main.cpp and runtime/diagnostics.cpp.
EXIT_NAMES = {0: "ok", 1: "fatal", 2: "timeout", 3: "crash", 4: "reinicio"}
# Exit code of a restart from the settings screen under CRASH_RESTART=exit (runtime/settings/settings.h).
RESTART_EXIT_CODE = 4
DIGITAL = {"A", "B", "X", "Y", "START", "BACK", "UP", "DOWN", "LEFT", "RIGHT", "LB", "RB", "L3", "R3"}
BUTTONS = DIGITAL | {
    # Analog sticks at full deflection (script only): LS_* walk, RS_* turn the camera.
    "LS_LEFT", "LS_RIGHT", "LS_UP", "LS_DOWN", "RS_LEFT", "RS_RIGHT", "RS_UP", "RS_DOWN"}
STICKS = {"LS", "RS"}      # [t, "LS", x, y] (-1..1)
ACTIONS = {"SHOT"}         # [t, "SHOT"]: a screenshot
MOUSE = {"MOUSE", "CLICK", "RCLICK", "LDOWN", "LUP"}  # [t, "CLICK", x, y]: the cursor at (x, y) of the 1280x720 image (+ a click;
# LDOWN / LUP press / release the left button there, for drags)
WHEEL = {"WHEEL"}          # [t, "WHEEL", n]: n notches of the mouse wheel (+ = up)
TRIGGERS = {"LT", "RT"}    # [t, "LT", v] (0..1)
# The replay's clock: game reads of the controller, 30 per second of gameplay.
TICKS_PER_SECOND = 30
# Seconds allowed past the runtime's own watchdog before the runner kills the process.
KILL_GRACE = 90
# A frame longer than this counts as a hitch (same threshold as the runtime's pacing log).
HITCH_MS = 50.0
REPLAY_FILE = "replay.txt"


def load_config(path):
    with open(path, "rb") as f:
        config = tomllib.load(f)
    # config/recorded/*.toml: [profiles.X] tables written by the runtime (F5). A name already in
    # run.toml keeps the run.toml one.
    profiles = config.setdefault("profiles", {})
    for file in sorted(RECORDED.glob("*.toml")) if RECORDED.is_dir() else []:
        try:
            with open(file, "rb") as f:
                recorded = tomllib.load(f).get("profiles", {})
        except (OSError, tomllib.TOMLDecodeError) as error:
            print(f"aviso: {file} ignorado ({error})")
            continue
        for name, profile in recorded.items():
            if name in profiles:
                print(f"aviso: o perfil {name} de {file.name} já existe em run.toml; usando o de run.toml")
                continue
            profile["_file"] = str(file)
            profiles[name] = profile
    # The dump is machine-specific: config/local.toml or the player's settings.toml (tools/buildenv.py).
    paths = config.setdefault("paths", {})
    if not paths.get("game"):
        game = buildenv.game_folder()
        if game is None:
            raise SystemExit("pasta do jogo desconhecida: rode python tools\\setup.py --game <pasta> ou ponha "
                             "[paths] game em config/local.toml")
        paths["game"] = str(game)
    return config


def resolve(path):
    p = Path(path)
    return p if p.is_absolute() else ROOT / p


def parse_override(text):
    key, sep, value = text.partition("=")
    if not sep:
        raise SystemExit(f"--set espera chave=valor, recebeu {text!r}")
    try:
        parsed = tomllib.loads(f"v = {value}")["v"]
    except tomllib.TOMLDecodeError:
        parsed = value  # bare words are strings
    return key.strip(), parsed


def profile_settings(config, name, overrides):
    profiles = config.get("profiles", {})
    if name not in profiles:
        raise SystemExit(f"perfil desconhecido: {name} (use --list)")
    settings = dict(config.get("defaults", {}))
    settings.update(profiles[name])
    settings.update(overrides)
    return settings


# The runtime's milestones (runtime/settings/direct_boot.cpp).
EVENT_LINE = re.compile(r"^\[test\] event (\w+) \(frame (\d+), input clock (-?[\d.]+) s, tick (-?\d+)\)(?:: (.*))?$", re.MULTILINE)


def anchor_name(entry):
    """["@gameplay_ready"] / ["@level_ready#2"] / ["@start"] -> "gameplay_ready" / "level_ready#2" / ""
    (None: not an anchor)."""
    if isinstance(entry, list) and len(entry) == 1 and isinstance(entry[0], str) and entry[0].startswith("@"):
        name = entry[0][1:].strip()
        if not re.fullmatch(r"\w+(#\d+)?", name):
            raise SystemExit(f'âncora inválida em input: {entry!r} (use ["@evento"] ou ["@evento#n"])')
        return "" if name == "start" else name
    return None


def input_script(presses):
    """`input` (wall-clock seconds) -> CRASH_INPUT_SCRIPT ("ms:NAME[:ms_held]", comma-separated);
    after an ["@event"] entry the times are seconds after that event ("event+ms:NAME")."""
    parts = []
    usage = ('(use [segundos, "BOTÃO"], [segundos, "BOTÃO", segundos_apertado], '
             '[segundos, "LS"/"RS", x, y, (segundos)], [segundos, "LT"/"RT", v, (segundos)], [segundos, "SHOT"] '
             'ou ["@evento"])')
    anchor = ""
    for entry in presses:
        name = anchor_name(entry)
        if name is not None:
            anchor = name
            continue
        if not (isinstance(entry, list) and len(entry) >= 2):
            raise SystemExit(f"entrada inválida em input: {entry!r} {usage}")
        seconds, button = entry[0], str(entry[1]).upper()
        # P2_ / P3_ / P4_ + a button, stick or trigger: a virtual second (third, fourth) controller
        # (local co-op), e.g. [1, "P2_START"], [2, "P2_LS", 1, 0, 1.5].
        player = ""
        if re.fullmatch(r"P[2-4]_.+", button) and button[3:] in BUTTONS | STICKS | TRIGGERS:
            player, button = button[:3], button[3:]
        values = 2 if button in STICKS or button in MOUSE else 1 if button in TRIGGERS or button in WHEEL else 0
        # KEY_<name>: a keyboard key or mouse button (settings/controls.h names: KEY_W, KEY_SPACE,
        # KEY_MOUSE1...), through the [controls] bindings like the real keyboard.
        # DEVICE_XBOX / DEVICE_PLAYSTATION / DEVICE_KEYBOARD: that device counts as the last one
        # used (the button icons follow it with controls.prompts = "auto").
        if button not in BUTTONS | ACTIONS and not values and not (button.startswith(("KEY_", "DEVICE_")) and len(button) > 4):
            raise SystemExit(f"botão desconhecido em input: {button} "
                             f"(válidos: {' '.join(sorted(BUTTONS | STICKS | TRIGGERS))} e KEY_<tecla>)")
        if len(entry) not in (2 + values, 3 + values):
            raise SystemExit(f"entrada inválida em input: {entry!r} {usage}")
        name = player + button
        if values:
            name += "=" + "/".join(f"{float(v):g}" for v in entry[2:2 + values])
        ms = round(float(seconds) * 1000)
        part = f"{anchor}+{ms}:{name}" if anchor else f"{ms}:{name}"
        if len(entry) == 3 + values:
            part += f":{round(float(entry[-1]) * 1000)}"
        parts.append(part)
    return ",".join(parts)


def ticks(seconds):
    return round(float(seconds) * TICKS_PER_SECOND)


def replay_lines(entries):
    """`replay` (game seconds) -> the runtime's CRASH_REPLAY file, one "tick KIND args" per line
    (see runtime/kernel/input_record.cpp)."""
    usage = ('[t, "BOTÃO", (segundos)], [t, "LS"/"RS", x, y], [t, "LT"/"RT", v], [t, "SYNC", "0xOFFSET#n"], '
             '[t, "WAIT", "texto do log"], [t, "EVENT", "evento"], [t, "SHOT"] ou [t, "END"]')
    lines = []
    for entry in entries:
        if not (isinstance(entry, list) and len(entry) >= 2 and isinstance(entry[1], str)):
            raise SystemExit(f"entrada inválida em replay: {entry!r} (use {usage})")
        tick, kind, args = ticks(entry[0]), entry[1].upper(), entry[2:]
        try:
            if kind in DIGITAL and len(args) <= 1:
                hold = ticks(args[0]) if args else ticks(0.2)
                lines.append(f"{tick} BTN {kind} {max(hold, 1)}")
            elif kind in STICKS and len(args) == 2:
                x, y = (max(-1.0, min(1.0, float(v))) for v in args)
                lines.append(f"{tick} {kind} {x:.3f} {y:.3f}")
            elif kind in TRIGGERS and len(args) == 1:
                lines.append(f"{tick} {kind} {max(0.0, min(1.0, float(args[0]))):.3f}")
            elif kind == "SYNC" and len(args) == 1:
                offset, sep, count = str(args[0]).partition("#")
                lines.append(f"{tick} SYNC {int(offset, 16):X} {int(count) if sep else 1}")
            elif kind == "WAIT" and len(args) == 1 and str(args[0]).strip():
                lines.append(f"{tick} WAIT {str(args[0]).strip()}")
            elif kind == "EVENT" and len(args) == 1 and re.fullmatch(r"\w+", str(args[0])):
                lines.append(f"{tick} EVENT {args[0]}")
            elif kind in ("SHOT", "END") and not args:
                lines.append(f"{tick} {kind}")
            else:
                raise ValueError
        except (TypeError, ValueError):
            raise SystemExit(f"entrada inválida em replay: {entry!r} (use {usage})")
    return lines


def replay_shots(settings):
    return sum(1 for e in settings.get("replay", []) if isinstance(e, list) and len(e) >= 2 and str(e[1]).upper() == "SHOT")


def input_shots(entries, events, seconds):
    """SHOT entries of an `input` the run reached: after an event that happened (with time left in
    the run after it), or on the input clock before the end."""
    times, anchor, wanted = {}, "", 0
    for name, clock in events:
        times.setdefault(name, []).append(clock)
    for entry in entries:
        name = anchor_name(entry)
        if name is not None:
            anchor = name
            continue
        if not (isinstance(entry, list) and len(entry) == 2 and str(entry[1]).upper() == "SHOT"):
            continue
        event, _, nth = anchor.partition("#")
        if not anchor:
            wanted += float(entry[0]) < seconds - 3
        else:
            seen, n = times.get(event, []), int(nth or 1)
            wanted += len(seen) >= n and seen[n - 1] + float(entry[0]) < seconds - 3
    return wanted


def parse_events(text):
    """[(name, input clock s, frame, tick, detail)] from the "[test] event" lines of a run.log."""
    return [(m.group(1), float(m.group(3)), int(m.group(2)), int(m.group(4)), m.group(5) or "") for m in EVENT_LINE.finditer(text)]


def build(config, run_dir):
    b = config.get("build", {})
    script = run_dir / "build.cmd"
    script.write_text(
        "@echo off\r\n" + "".join(line + "\r\n" for line in buildenv.cmd_prelude()) +
        f'cmake --build "{resolve(b.get("build_dir", "build"))}" --target {b.get("target", "CrashRecomp")}\r\n',
        encoding="ascii")
    log_path = run_dir / "build.log"
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        result = subprocess.run(["cmd", "/d", "/c", str(script)], stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
    if result.returncode != 0:
        tail = log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-25:]
        print("\n".join(tail))
        print(f"build falhou (código {result.returncode}); log completo em {log_path}")
        return False
    return True


def prepare_userdata(config, settings, run_dir):
    """The userdata folder of a run. "copy" (the default, every test): a fresh copy of the fixed
    test base ([paths].test_userdata: saves in content/ and settings.toml) in the run folder, with
    the shader cache of the tests ([paths].test_shadercache), so runs never change the base nor the
    player's userdata. "shared" (only `jogar`): the player's own userdata ([paths].userdata)."""
    paths = config["paths"]
    player = resolve(paths["userdata"]).resolve()
    mode = settings.get("userdata_mode", "copy")
    if mode == "shared":
        return player
    if mode != "copy":
        raise SystemExit(f"userdata_mode inválido: {mode} (use \"copy\" ou \"shared\")")
    copy = run_dir / "userdata"
    copy.mkdir()
    # userdata_from: the saves (content\) and settings.toml from another folder, e.g. the user
    # data a recording started with (relative to the profile's file; run.toml: to the project).
    source = resolve(paths.get("test_userdata", "tests/userdata_base"))
    if settings.get("userdata_from"):
        base = Path(settings["_file"]).parent if settings.get("_file") else ROOT
        source = Path(settings["userdata_from"])
        source = source if source.is_absolute() else base / source
    if not source.is_dir():
        raise SystemExit(f"userdata de teste não existe: {source}")
    if source.resolve() == player:
        raise SystemExit(f"{source} é a userdata do jogador: os testes usam uma cópia da base de teste "
                         f"(paths.test_userdata), nunca a userdata de verdade")
    if (source / "settings.toml").is_file():
        shutil.copy2(source / "settings.toml", copy / "settings.toml")
    if (source / "content").is_dir():
        shutil.copytree(source / "content", copy / "content")
    # The tests' own shader cache (only saves compile time); merged back after the run.
    cache = test_shadercache(config)
    if cache.is_dir():
        shutil.copytree(cache, copy / "shadercache")
    return copy


def test_shadercache(config):
    return resolve(config["paths"].get("test_shadercache", "tests/shadercache"))


def merge_shadercache(config, userdata):
    """Shaders compiled during a copy-mode run go into the tests' cache (new files only), so the
    next runs do not compile them again at boot (that slows the boot and desyncs input scripts)."""
    source, cache = userdata / "shadercache", test_shadercache(config)
    if not source.is_dir():
        return 0
    cache.mkdir(parents=True, exist_ok=True)
    added = 0
    for file in source.iterdir():
        if file.is_file() and not (cache / file.name).exists():
            shutil.copy2(file, cache / file.name)
            added += 1
    return added


OVERLAY_MODES = {"off": "off", "desligado": "off", "fps": "fps", "frametime": "frametime",
                 "fps_frametime": "frametime", "full": "full", "completo": "full"}


def environment(settings, run_dir, profile=None):
    # Start from the user's environment minus any CRASH_* left over from manual runs.
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("CRASH_")}
    if settings.get("timeout", 0):
        env["CRASH_TIMEOUT"] = str(settings["timeout"])
    if not settings.get("audio", True):
        env["CRASH_NOAUDIO"] = "1"
    # The runtime treats these as on whenever they are set, whatever the value.
    if not settings.get("render", True):
        env["CRASH_NO_RENDER"] = "1"
    if settings.get("d3d12_debug") or settings.get("gpu_debug"):
        env["CRASH_GPU_DEBUG"] = "1"  # D3D12 debug layer / Vulkan validation layers
    if settings.get("renderer"):
        env["CRASH_RENDERER"] = str(settings["renderer"])  # d3d12 | vulkan (gpu/renderer.h)
    if settings.get("log_all_io"):
        env["CRASH_LOG_ALL_IO"] = "1"
    if not settings.get("symbols", True):
        env["CRASH_SYMBOLS"] = "0"
    if settings.get("input"):
        env["CRASH_INPUT_SCRIPT"] = input_script(settings["input"])
    # Past the intros and the menus through the game's code (runtime/settings/direct_boot.h).
    if settings.get("boot"):
        env["CRASH_BOOT"] = str(settings["boot"])
    if settings.get("skip_intros"):
        env["CRASH_SKIP_INTROS"] = "1"
    if settings.get("skip_cinematics"):
        env["CRASH_SKIP_CINEMATICS"] = "1"
    # Developer leftovers (runtime/dev/dev_tools.h, docs/dev_features.md): Lua in the game's state,
    # invincibility and the dev.* shortcuts.
    if settings.get("dev_lua"):
        env["CRASH_DEV_LUA"] = str(settings["dev_lua"])
    if settings.get("dev_lua_file"):
        env["CRASH_DEV_LUA_FILE"] = str(resolve(settings["dev_lua_file"]))
    if settings.get("dev_lua_at"):
        env["CRASH_DEV_LUA_AT"] = str(settings["dev_lua_at"])
    if settings.get("dev_invincible"):
        env["CRASH_DEV_INVINCIBLE"] = "1"
    if settings.get("dev_teleport"):
        env["CRASH_DEV_TELEPORT"] = ",".join(str(float(v)) for v in settings["dev_teleport"])
    if settings.get("dev_mojo"):
        env["CRASH_DEV_MOJO"] = str(int(settings["dev_mojo"]))
    if settings.get("dev_unlock_all"):
        env["CRASH_DEV_UNLOCK_ALL"] = "1"
    if settings.get("dev_shadow"):
        env["CRASH_DEV_SHADOW"] = "1"
    if settings.get("dev_gamelink"):
        env["CRASH_DEV_GAMELINK"] = "1"
    if settings.get("dev_console") is not None:
        env["CRASH_DEV_CONSOLE"] = "1"
        script = run_dir / "console_script.txt"
        script.write_text("".join(f"{str(t).lstrip('@')}|{text}\n" for t, text in settings["dev_console"]), encoding="utf-8")
        env["CRASH_DEV_CONSOLE_SCRIPT"] = str(script)
    if settings.get("replay"):
        replay = run_dir / REPLAY_FILE
        replay.write_text("# replay de " + str(profile) + " (tick = segundos do jogo x 30)\n" +
                          "\n".join(replay_lines(settings["replay"])) + "\n", encoding="utf-8")
        env["CRASH_REPLAY"] = str(replay)
    if settings.get("screenshots"):
        env["CRASH_SCREENSHOTS"] = ",".join(str(int(n)) for n in settings["screenshots"])
    env["CRASH_SCREENSHOT_DIR"] = str(run_dir)
    env["CRASH_FRAME_STATS"] = str(run_dir / "frametimes.csv")
    # overlay: true = completo (o de antes), false = o que estiver no settings.toml, ou o modo:
    # "off" | "fps" | "frametime" | "full" (também "desligado" e "completo").
    overlay = settings.get("overlay")
    if overlay is True:
        env["CRASH_OVERLAY"] = "full"
    elif isinstance(overlay, str):
        mode = OVERLAY_MODES.get(overlay.lower())
        if not mode:
            raise SystemExit(f"overlay = {overlay!r}: use true/false ou {', '.join(sorted(OVERLAY_MODES))}")
        env["CRASH_OVERLAY"] = mode
    if profile:
        env["CRASH_PROFILE"] = profile  # o overlay completo mostra o perfil e o último botão
    if settings.get("screenshot_overlay"):
        env["CRASH_SCREENSHOT_OVERLAY"] = "1"
    # F5 records in any run; record_input = true starts recording at boot (the gravar profile).
    env["CRASH_RECORD_DIR"] = str(RECORDED)
    if settings.get("record_input"):
        env["CRASH_RECORD_INPUT"] = "1"
    if settings.get("audio_dump"):
        env["CRASH_AUDIO_DUMP"] = str(run_dir / "audio.wav")
        env["CRASH_AUDIO_DUMP_SECONDS"] = str(settings.get("audio_dump_seconds", 60))
    if settings.get("log_imports"):
        env["CRASH_LOG_IMPORTS"] = ",".join(settings["log_imports"])
    for key, value in settings.get("env", {}).items():
        env[key] = str(value)
    # A restart from the settings screen exits with code 4 and the runner starts the game again
    # (run_profile), instead of the game relaunching itself out of the runner's sight.
    env.setdefault("CRASH_RESTART", "exit")
    return env


def restart_environment(env, settings, run_dir, launch):
    """The environment of launch number `launch` (1 = the first restart): the per-run overrides of
    the first launch (CRASH_SETTINGS is already in its settings.toml; input, replay and recording
    would start over) are dropped, and screenshots / frame times go to reinicio_<launch>/."""
    env = {k: v for k, v in env.items()
           if k not in ("CRASH_SETTINGS", "CRASH_INPUT_SCRIPT", "CRASH_REPLAY", "CRASH_RECORD_INPUT",
                        "CRASH_SCREENSHOTS", "CRASH_SCREENSHOT_TIMES", "CRASH_AUDIO_DUMP", "CRASH_BOOT")}
    folder = run_dir / f"reinicio_{launch}"
    folder.mkdir(exist_ok=True)
    env["CRASH_SCREENSHOT_DIR"] = str(folder)
    env["CRASH_FRAME_STATS"] = str(folder / "frametimes.csv")
    if settings.get("restart_input"):
        env["CRASH_INPUT_SCRIPT"] = input_script(settings["restart_input"])
    if settings.get("restart_boot"):
        env["CRASH_BOOT"] = str(settings["restart_boot"])
    timeout = settings.get("restart_timeout", settings.get("timeout", 0))
    if timeout:
        env["CRASH_TIMEOUT"] = str(timeout)
    else:
        env.pop("CRASH_TIMEOUT", None)
    for key, value in settings.get("restart_env", {}).items():
        env[key] = str(value)
    return env, timeout


def analyse_log(log_path):
    text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
    frames = max((int(n) for n in re.findall(r"VdSwap #(\d+)", text)), default=0)
    problems = [line for line in text.splitlines() if line.startswith(("[fatal]", "[crash]"))]
    pacing = re.findall(r"\[gpu\] present pacing: .*", text)
    return text, frames, problems, (pacing[-1] if pacing else None)


def frame_stats(path, from_frame=0):
    """Frame-time statistics from the runtime's frametimes.csv (frames from `from_frame` on)."""
    times = []
    if path.exists():
        with open(path, newline="", encoding="utf-8", errors="replace") as f:
            for row in csv.DictReader(f):
                try:
                    if int(row["frame"]) >= from_frame:
                        times.append(float(row["ms"]))
                except (KeyError, TypeError, ValueError):
                    pass  # the last line can be cut short when the process is terminated
    if not times:
        return None
    ordered = sorted(times)
    count = len(ordered)
    total_ms = sum(ordered)
    worst = ordered[-max(1, count // 100):]
    return {
        "from_frame": from_frame,
        "frames": count,
        "avg_fps": round(count * 1000.0 / total_ms, 2) if total_ms else 0.0,
        "avg_ms": round(total_ms / count, 2),
        "max_ms": round(ordered[-1], 2),
        "p99_ms": round(ordered[min(count - 1, math.ceil(count * 0.99) - 1)], 2),
        "low_1pct_fps": round(1000.0 * len(worst) / sum(worst), 2),
        "hitches": sum(1 for ms in ordered if ms > HITCH_MS),
    }


def describe_perf(perf):
    return (f"{perf['avg_fps']:.1f} fps médio, 1% low {perf['low_1pct_fps']:.1f} fps | frame {perf['avg_ms']:.1f} ms médio, "
            f"p99 {perf['p99_ms']:.1f} ms, max {perf['max_ms']:.1f} ms | {perf['hitches']} acima de {HITCH_MS:.0f} ms "
            f"({perf['frames']} quadros a partir do {perf['from_frame']})")


def check(settings, exit_name, text, frames, screenshots, perf, restarts=0, seconds=0.0):
    expect = settings.get("expect", {})
    results = []
    events = parse_events(text)
    seen = [name for name, *_ in events]
    for wanted in expect.get("events", []):
        event, _, nth = str(wanted).partition("#")
        count = seen.count(event)
        results.append((f"evento {wanted}", count >= int(nth or 1), f"{count}x"))
    absent = list(expect.get("events_absent", []))
    if (settings.get("boot") or settings.get("restart_boot")) and "boot_failed" not in absent:
        absent.append("boot_failed")
    for unwanted in absent:
        detail = next((d for name, _, _, _, d in events if name == unwanted), None)
        results.append((f"sem evento {unwanted}", detail is None, "ausente" if detail is None else (detail or "presente")))
    if "restarts" in expect:
        results.append((f"reinícios = {expect['restarts']}", restarts == expect["restarts"], str(restarts)))
    allowed = expect.get("exit")
    if allowed:
        results.append((f"saída em {allowed}", exit_name in allowed, exit_name))
    if "min_frames" in expect:
        results.append((f"quadros >= {expect['min_frames']}", frames >= expect["min_frames"], f"{frames} quadros"))
    for pattern in expect.get("log_absent", []):
        found = re.search(pattern, text, re.MULTILINE)
        results.append((f"log sem /{pattern}/", found is None, found.group(0) if found else "ausente"))
    for pattern in expect.get("log_contains", []):
        found = re.search(pattern, text, re.MULTILINE)
        results.append((f"log com /{pattern}/", found is not None, "presente" if found else "ausente"))
    perf_checks = [("min_fps", "fps médio >= {}", lambda p, v: p["avg_fps"] >= v, "{avg_fps:.1f} fps"),
                   ("max_frame_ms_p99", "frame p99 <= {} ms", lambda p, v: p["p99_ms"] <= v, "{p99_ms:.1f} ms"),
                   ("max_hitches", "quadros > 50 ms <= {}", lambda p, v: p["hitches"] <= v, "{hitches}")]
    for key, label, test, detail in perf_checks:
        if key in expect:
            if perf:
                results.append((label.format(expect[key]), test(perf, expect[key]), detail.format(**perf)))
            else:
                results.append((label.format(expect[key]), False, "sem frametimes.csv"))
    # Only screenshots for frames the run actually reached are expected; a replay's SHOT entries
    # only if it finished.
    wanted = len([n for n in settings.get("screenshots", []) if n <= frames])
    if replay_shots(settings) and re.search(r"\[input\] replay finished", text):
        wanted += replay_shots(settings)
    wanted += input_shots(settings.get("input", []), [(n, c) for n, c, *_ in events], seconds)
    if wanted:
        results.append((f"{wanted} captura(s)", len(screenshots) >= wanted, f"{len(screenshots)} salva(s)"))
    return results


def run_profile(config, name, overrides, do_build, dist=False):
    settings = profile_settings(config, name, overrides)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = resolve(config["paths"]["runs"]) / f"{stamp}_{name}"
    # Two runs of a profile started in the same second (parallel tests) get their own folders.
    for n in range(2, 100):
        try:
            run_dir.mkdir(parents=True)
            break
        except FileExistsError:
            run_dir = resolve(config["paths"]["runs"]) / f"{stamp}_{name}_{n}"
    print(f"== {name}: {settings.get('description', '')}")
    print(f"   pasta: {run_dir}")

    if do_build and not build(config, run_dir):
        return {"profile": name, "passed": False, "exit": "build", "run_dir": str(run_dir)}

    exe = resolve(config["paths"]["exe"])
    if not exe.exists():
        raise SystemExit(f"executável não encontrado: {exe}")
    userdata = prepare_userdata(config, settings, run_dir)
    recorded_before = {p: p.stat().st_mtime for p in RECORDED.glob("*.toml")} if RECORDED.is_dir() else {}
    paths = config["paths"]
    if dist:
        # As the player runs it: only the userdata copy, whose settings.toml says where the game is.
        args = [str(exe)]
        if userdata != resolve(paths["userdata"]).resolve():
            set_game_folder(userdata, resolve(paths["game"]))
            args += ["--userdata", str(userdata)]
        if settings.get("mods_dir"):
            args += ["--mods", str(resolve(settings["mods_dir"]))]
    else:
        args = [str(exe),
                "--game", str(resolve(paths["game"])),
                "--mods", str(resolve(settings.get("mods_dir") or paths["mods"])),
                "--userdata", str(userdata),
                "--languages", str(resolve(paths.get("languages", "languages"))),
                "--data", str(resolve(paths.get("data", "private")))]
    env = environment(settings, run_dir, name)
    (run_dir / "settings.json").write_text(json.dumps(
        {"profile": name, "settings": {k: v for k, v in settings.items() if not k.startswith("_")}, "args": args,
         "env": {k: v for k, v in env.items() if k.startswith("CRASH_")}}, indent=2, ensure_ascii=False),
        encoding="utf-8")

    timeout = settings.get("timeout", 0)
    log_path = run_dir / "run.log"
    started = datetime.datetime.now()
    restarts = 0
    launch_args, launch_env = args, env
    with open(log_path, "wb") as log:
        while True:
            process = subprocess.Popen(launch_args, stdout=log, stderr=subprocess.STDOUT, cwd=exe.parent if dist else ROOT, env=launch_env)
            # Same identity as the window title (runtime/gpu/window.cpp): who started this instance.
            session = os.environ.get("CLAUDE_CODE_HOST_SESSION_ID", "").removeprefix("local_")[:8]
            print(f"   jogo: PID {process.pid}" + (f", sessão {session}" if session else "")
                  + (f", até {timeout} s" if timeout else ", sem limite de tempo"), flush=True)
            try:
                code = process.wait(timeout=timeout + KILL_GRACE if timeout else None)
            except subprocess.TimeoutExpired:
                process.kill()
                code = process.wait()
                print(f"   o watchdog do runtime não encerrou o jogo; processo morto após {timeout + KILL_GRACE} s")
            except KeyboardInterrupt:
                process.kill()
                process.wait()
                print("   interrompido (Ctrl+C)")
                code = None
            # Restart from the settings screen: start the game again and keep tracking it.
            if code != RESTART_EXIT_CODE or restarts >= int(settings.get("max_restarts", 3)):
                break
            restarts += 1
            print(f"   o jogo pediu para reiniciar (tela de configurações): reinício {restarts}")
            log.write(f"\n[run_game] ===== reinicio {restarts}: o jogo saiu com o codigo {RESTART_EXIT_CODE} e foi aberto de novo =====\n".encode())
            log.flush()
            launch_args = args + ["--restarted", str(process.pid)]
            launch_env, timeout = restart_environment(env, settings, run_dir, restarts)
    seconds = (datetime.datetime.now() - started).total_seconds()
    if settings.get("userdata_mode", "copy") == "copy":
        added = merge_shadercache(config, userdata)
        if added:
            print(f"   {added} shader(s) novo(s) no cache dos testes ({test_shadercache(config).relative_to(ROOT)})")

    exit_name = "interrompido" if code is None else EXIT_NAMES.get(code, f"código {code & 0xFFFFFFFF:#x}")
    text, frames, problems, pacing = analyse_log(log_path)
    screenshots = sorted(str(p.relative_to(run_dir)) for p in run_dir.rglob("*.png") if "userdata" not in p.parts)
    perf = frame_stats(run_dir / "frametimes.csv", int(settings.get("perf_from_frame", 0)))
    results = check(settings, exit_name, text, frames, screenshots, perf, restarts, seconds)
    events = parse_events(text)
    passed = all(ok for _, ok, _ in results)

    restarted = f" | {restarts} reinício(s)" if restarts else ""
    print(f"   saída: {exit_name} | {seconds:.0f} s | {frames} quadros | {len(screenshots)} captura(s){restarted}")
    if perf:
        print(f"   desempenho: {describe_perf(perf)}")
    elif pacing:
        print(f"   {pacing}")
    marks_path = run_dir / "marks.txt"
    marks = marks_path.read_text(encoding="utf-8", errors="replace").splitlines() if marks_path.exists() else []
    for line in marks:
        print(f"   [marca] {line}")
    for line in problems[:5]:
        print(f"   {line}")
    # When each milestone came (input clock: seconds since the first controller read).
    if events:
        shown = []
        for event, clock, _, _, detail in events:
            if event in ("cinematic", "pause") and len(events) > 12:
                continue
            extra = f" {detail.split(' (')[0]}" if event in ("level_start", "boot_failed") and detail else ""
            shown.append(f"{event}{extra} {clock:.1f} s")
        print("   eventos: " + " | ".join(shown))
    # Replay: how far each SYNC moved the timeline (later = the load took longer than recorded).
    for line in re.findall(r"^\[input\] replay (?:sync|finished|t=).*$", text, re.MULTILINE):
        print(f"   {line[8:]}")
    for label, ok, detail in results:
        print(f"   [{'ok' if ok else 'FALHOU'}] {label}: {detail}")

    result = {"profile": name, "passed": passed, "exit": exit_name, "exit_code": code, "restarts": restarts, "seconds": round(seconds, 1),
              "frames": frames, "screenshots": screenshots, "problems": problems[:20], "pacing": pacing,
              "perf": perf, "marks": marks,
              "events": [{"event": n, "input_clock": c, "frame": f, "tick": t, "detail": d} for n, c, f, t, d in events],
              "checks": [{"check": label, "ok": ok, "detail": detail} for label, ok, detail in results],
              "run_dir": str(run_dir)}
    # Recordings made during the run (F5, record_input): new or rewritten config/recorded/*.toml.
    recordings = sorted(p for p in RECORDED.glob("*.toml") if recorded_before.get(p) != p.stat().st_mtime) if RECORDED.is_dir() else []
    if recordings:
        result["recordings"] = [str(p) for p in recordings]
        for path in recordings:
            print(f"   gravação: {path.relative_to(ROOT)}  ->  python tools/run_game.py {path.stem}")
    (run_dir / "result.json").write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    return result


def main():
    # Accented messages; pipes default to the ANSI code page on Windows.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="Roda o CrashRecomp com um perfil de config/run.toml.")
    parser.add_argument("profile", nargs="?", help="perfil a rodar")
    parser.add_argument("--all", action="store_true", help="roda todos os perfis com test = true")
    parser.add_argument("--list", action="store_true", help="lista os perfis")
    parser.add_argument("--set", action="append", default=[], metavar="CHAVE=VALOR",
                        help="sobrescreve uma opção do perfil (ex.: timeout=60, audio=false)")
    parser.add_argument("--no-build", action="store_true", help="não compila antes de rodar")
    parser.add_argument("--config", default=str(CONFIG), help="arquivo de configuração")
    parser.add_argument("--exe", help="roda outro executável no lugar de paths.exe (ex.: um linkado à parte "
                                      "enquanto build\\CrashRecomp.exe está aberto); implica --no-build")
    parser.add_argument("--dist", action="store_true", help="roda dist\\CrashRecomp.exe como o jogador (só --userdata; "
                        "o exe acha jogo, mods, idiomas e data sozinho); implica --no-build (atualize com tools\\make_dist.py)")
    parser.add_argument("--runs", help="pasta das execuções no lugar de paths.runs (ex.: build/testes_auto)")
    args = parser.parse_args()

    config = load_config(args.config)
    if args.runs:
        config["paths"]["runs"] = args.runs
    if args.dist:
        config["paths"]["exe"] = str(resolve(config["paths"].get("dist", "dist")) / "CrashRecomp.exe")
        args.no_build = True
    if args.exe:
        config["paths"]["exe"] = args.exe
        args.no_build = True
    profiles = config.get("profiles", {})
    if args.list or not (args.profile or args.all):
        for name, p in profiles.items():
            tag = "teste" if p.get("test") else "grav." if p.get("_file") else "     "
            print(f"{name:24} {tag}  {p.get('description', '')}")
        return 0

    overrides = dict(parse_override(s) for s in args.set)
    names = [n for n, p in profiles.items() if p.get("test")] if args.all else [args.profile]
    do_build = config.get("build", {}).get("auto", True) and not args.no_build

    results = []
    for i, name in enumerate(names):
        results.append(run_profile(config, name, overrides, do_build and i == 0, args.dist))
        if results[-1]["exit"] == "build":
            break
    if len(results) > 1:
        print("\n== resumo")
        for r in results:
            perf = r.get("perf")
            fps = f"  {perf['avg_fps']:5.1f} fps, p99 {perf['p99_ms']:5.1f} ms, {perf['hitches']:3} > 50 ms" if perf else ""
            print(f"   {'ok    ' if r['passed'] else 'FALHOU'} {r['profile']:24} {r['exit']:10} {r.get('frames', 0):6} quadros{fps}")
    return 0 if all(r["passed"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
