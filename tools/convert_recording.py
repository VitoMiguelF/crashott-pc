"""Converts a profile recorded with F5 (config/recorded/<name>.toml) to one that boots straight into
the game (runtime/settings/direct_boot.h) instead of replaying the way through the intros and menus.
The recording itself is left as it is: the new profile goes to config/recorded/<name>_boot.toml.

    python tools/convert_recording.py gravado_20261020_221744 --boot load:1
    python tools/convert_recording.py gravado_20261020_221744 --boot load:1 --no-calibrate

The recorded replay starts at the game's first controller read and walks the menus (START on the
logos, DOWN, A on LOAD GAME...). The converted one boots with `boot` and starts with
[t, "EVENT", "gameplay_ready"]: the replay waits for the player to have control and re-aligns its
timeline there, so the recorded play keeps its distance to that moment. What came before it (the
menus) is dropped; everything after it (presses, sticks, the loads' SYNC, SHOT, END) is kept.

When the player got control in the recording is not written in it (older recordings have no
events), so by default the converter replays the original once (tools/run_game.py, alone, a copy
of its saves) and reads it from the run's log: the tick of the first gameplay_ready minus how far
the SYNCs had moved the timeline by then. --no-calibrate uses the last load (SYNC) before the
recorded part instead, which is earlier than the real moment (the title card, the HUD fading in)
by up to a second or so. Recordings made now have EVENT entries already; they don't need this.
"""
import argparse
import re
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RECORDED = ROOT / "config" / "recorded"
TICKS = 30
SYNC_LINE = re.compile(r"^\[input\] replay sync \d+ at t=([\d.]+) s: .*\(tick (-?\d+), timeline ([+-][\d.]+) s\)", re.MULTILINE)
EVENT_LINE = re.compile(r"^\[test\] event (\w+) \(frame \d+, input clock [-\d.]+ s, tick (-?\d+)\)", re.MULTILINE)


def calibrate(name, anchor, runs):
    """Replays the recording once and returns the recorded time (s) of the first `anchor` event."""
    print(f"calibrando: rodando {name} uma vez para ver quando acontece {anchor} (sozinho, numa cópia dos saves)...")
    command = [sys.executable, str(ROOT / "tools" / "run_game.py"), name, "--no-build", "--runs", runs]
    output = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
    print(output)
    folder = re.search(r"pasta: (.+)", output)
    if not folder:
        raise SystemExit("não achei a pasta da execução na saída do run_game.py")
    log = (Path(folder.group(1).strip()) / "run.log").read_text(encoding="utf-8", errors="replace")
    event = next((int(m.group(2)) for m in EVENT_LINE.finditer(log) if m.group(1) == anchor), None)
    if event is None:
        raise SystemExit(f"o evento {anchor} não aconteceu no replay da gravação (o exe tem o boot direto? compile antes)")
    # The shift in effect at that tick: the last SYNC that ended before it.
    shift = 0.0
    for m in SYNC_LINE.finditer(log):
        if int(m.group(2)) <= event:
            shift = float(m.group(3))
    recorded = event / TICKS - shift
    print(f"{anchor}: tick {event} do replay, linha do tempo {shift:+.2f} s -> {recorded:.2f} s no tempo da gravação")
    return recorded


def entry_text(entry):
    def value(v):
        return f'"{v}"' if isinstance(v, str) else (f"{v:.2f}" if isinstance(v, float) else str(v))
    return "[" + ", ".join(value(v) for v in entry) + "]"


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="Converte uma gravação (F5) para começar com o boot direto.")
    parser.add_argument("name", help="nome do perfil gravado (config/recorded/<nome>.toml)")
    parser.add_argument("--boot", required=True, help='o boot do perfil novo: "load:1", "load:2:L1_E3", "level:L1_E2"...')
    parser.add_argument("--anchor", default="gameplay_ready", help="o evento onde o replay recomeça (padrão gameplay_ready)")
    parser.add_argument("--no-calibrate", action="store_true", help="não roda a gravação; usa o último SYNC antes do F5")
    parser.add_argument("--runs", default="build/testes_auto/conversao", help="pasta da execução de calibração")
    args = parser.parse_args()

    source = RECORDED / f"{args.name}.toml"
    with open(source, "rb") as f:
        profile = tomllib.load(f)["profiles"][args.name]
    text = source.read_text(encoding="utf-8")
    replay = profile["replay"]
    f5 = re.search(r"# --- F5: inicio da gravacao \(([\d.]+) s\)", text)
    start = float(f5.group(1)) if f5 else min(e[0] for e in replay if e[1] == "SHOT")

    if args.no_calibrate:
        syncs = [e[0] for e in replay if e[1] == "SYNC" and e[0] <= start]
        if not syncs:
            raise SystemExit("nenhum SYNC antes do F5: use a calibração")
        anchor_time = syncs[-1]
        print(f"sem calibração: {args.anchor} no último SYNC antes do F5, {anchor_time:.2f} s")
    else:
        anchor_time = calibrate(args.name, args.anchor, args.runs)
    if anchor_time > start:
        print(f"aviso: {args.anchor} veio depois do F5 ({anchor_time:.2f} > {start:.2f} s): o que foi gravado antes dele cai fora")

    # A load (SYNC) that ended by the anchor would move the timeline away from it again.
    kept = [e for e in replay if e[0] > anchor_time or (e[0] == anchor_time and e[1] != "SYNC")]
    dropped = [e for e in replay if e[0] < anchor_time and e[1] != "SYNC"]
    new_name = f"{args.name}_boot"
    lines = [f"# Convertido de {source.name} por tools/convert_recording.py: começa com boot = {args.boot!r} e",
             f"# re-alinha em {args.anchor} (gravação: {anchor_time:.2f} s); o caminho pelos menus ({len(dropped)} entradas antes) saiu.",
             f"[profiles.{new_name}]",
             f'description = "{profile.get("description", "")} | convertido: boot {args.boot}, a partir de {args.anchor}"',
             "test = false",
             f"timeout = {int(max(e[0] for e in replay) - anchor_time + 90)}",
             f"audio = {'true' if profile.get('audio', True) else 'false'}",
             'userdata_mode = "copy"']
    if profile.get("userdata_from"):
        lines.append(f'userdata_from = "{profile["userdata_from"]}"   # relativo a este arquivo')
    if profile.get("env"):
        lines.append("env = { " + ", ".join(f"{k} = '{v}'" for k, v in profile["env"].items()) + " }")
    lines.append(f'boot = "{args.boot}"')
    lines.append("replay = [")
    lines.append(f'    [{anchor_time:.2f}, "EVENT", "{args.anchor}"],   # o jogador tem o controle (o replay espera e re-alinha aqui)')
    row = []
    for e in kept:
        item = entry_text(e)
        if e[1] in ("SYNC", "SHOT", "END", "EVENT", "WAIT"):
            if row:
                lines.append("    " + " ".join(row))
                row = []
            lines.append(f"    {item},")
        else:
            row.append(item + ",")
            if len(row) == 4:
                lines.append("    " + " ".join(row))
                row = []
    if row:
        lines.append("    " + " ".join(row))
    lines.append("]")
    lines.append("expect = { exit = [\"ok\"], events = [\"" + args.anchor + "\"], log_absent = ['\\[fatal\\]', '\\[crash\\]', "
                 "'replay sync .* timed out', 'replay event .* timed out'], log_contains = ['\\[input\\] replay finished'] }")
    target = RECORDED / f"{new_name}.toml"
    target.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"escrito {target.relative_to(ROOT)} ({len(kept)} entradas; {len(dropped)} do caminho pelos menus fora)")
    print(f"rode com: python tools/run_game.py {new_name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
