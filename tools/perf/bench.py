"""Benchmark do CrashRecomp: roda o perfil `desempenho_vila` (config/run.toml; NEW GAME, a vila da
primeira fase, a cena mais pesada) com um conjunto de opções e mede o trecho jogado (no
relógio INPUT, de 2 s depois de o limite de FPS mudar em t = 152 s até o fim do replay). `--profile desempenho` usa o episódio 2
(LOAD GAME) e mede os últimos ~46 s.

    python tools/perf/bench.py <nome> [--fps 0|30|60|120] [--settings "video.resolution=1920x1080;..."] [--exe build\\X.exe]
                               [--affinity 0xFF] [--sample 300] [--profile desempenho_vila] [--no-build]

Escreve build/desempenho/<nome>.json (FPS, frame time, pacing do renderer, tempo de CPU por thread se
--sample) e, com --sample N, build/desempenho/<nome>.samples.txt (tools/perf/sampler.exe a N us por
amostra durante a parte jogada; tools/perf/profile.py dá os nomes).
"""
import argparse
import csv
import ctypes
import json
import re
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build" / "desempenho"
SAMPLER = ROOT / "build" / "perf" / "sampler.exe"

PACING = re.compile(r"present pacing: (\d+) frames, avg ([\d.]+) ms, max ([\d.]+) ms, (\d+) over 50 ms; waiting for the GPU ([\d.]+) ms/frame, "
                    r"submit ([\d.]+) ms/frame, longest Present ([\d.]+) ms, ring waits ([\d.]+) ms/frame")
CP = re.compile(r"CP thread ms/frame: (.*)")


def set_affinity(pid, mask):
    kernel32 = ctypes.windll.kernel32
    handle = kernel32.OpenProcess(0x0200 | 0x0400, False, pid)  # SET_INFORMATION | QUERY_INFORMATION
    if not handle:
        print(f"   OpenProcess({pid}) falhou para a afinidade", file=sys.stderr)
        return False
    ok = kernel32.SetProcessAffinityMask(handle, ctypes.c_size_t(mask))
    kernel32.CloseHandle(handle)
    return bool(ok)


def frame_window(csv_path, span=None, seconds=46.0, tail=2.0):
    rows = [(int(r["frame"]), float(r["ms"]), float(r["input_s"])) for r in csv.DictReader(open(csv_path))]
    end = rows[-1][2]
    start, stop = span if span else (end - tail - seconds, end - tail)
    window = [r for r in rows if start <= r[2] <= stop]
    ms = [r[1] for r in window]
    ms_sorted = sorted(ms)
    total = sum(ms)
    p99 = ms_sorted[int(len(ms_sorted) * 0.99) - 1] if ms_sorted else 0
    worst = ms_sorted[-max(1, len(ms_sorted) // 100):]
    return {
        "frames": len(ms), "seconds": round(total / 1000, 2),
        "fps": round(len(ms) / (total / 1000), 2) if total else 0,
        "ms_avg": round(statistics.mean(ms), 3), "ms_median": round(statistics.median(ms), 3),
        "ms_p99": round(p99, 2), "ms_max": round(max(ms), 2),
        "fps_1pct_low": round(1000 / statistics.mean(worst), 2),
        "over_25ms": sum(1 for m in ms if m > 25), "over_50ms": sum(1 for m in ms if m > 50),
        "first_frame": window[0][0], "last_frame": window[-1][0],
        "input_s": [round(window[0][2], 2), round(window[-1][2], 2)],
    }


def unlock_window(run_dir, settle=2.0, tail=1.0):
    """From the frame limit switch (CRASH_FPS_LIMIT_AT) + `settle` s to the end - `tail`, on the
    input clock. The replay's game clock runs ahead of the wall clock during the opening (the
    cutscene reads the controller once per frame), so "the last N seconds" would reach back into
    the 30 fps part. Older logs (no input clock in that line): --window."""
    rows = [(float(r["ms"]), float(r["input_s"])) for r in csv.DictReader(open(run_dir / "frametimes.csv"))]
    end = rows[-1][1]
    for line in open(run_dir / "run.log", encoding="utf-8", errors="replace"):
        m = re.search(r"CRASH_FPS_LIMIT_AT: .*input clock ([\d.]+) s", line)
        if m:
            return (float(m.group(1)) + settle, end - tail)
        # Direct boot profiles: 20 s from the moment the player has the controller (+1 s).
        m = re.search(r"\[test\] event gameplay_ready \(frame \d+, input clock ([\d.]+) s", line)
        if m:
            start = float(m.group(1)) + 1.0
            return (start, min(start + 20.0, end - tail))
    return None  # logs from before the input clock was logged: pass --window


def pacing_window(log_path, first_frame, last_frame):
    """Average of the [gpu] present pacing / CP lines logged inside the frame window (one per 300 frames)."""
    pacing, cp, focus = [], [], []
    frame = 0
    for line in open(log_path, encoding="utf-8", errors="replace"):
        m = PACING.search(line)
        if m:
            frame += int(m.group(1))
            if first_frame + 300 <= frame <= last_frame:
                pacing.append([float(x) for x in m.groups()[1:]])
                if "; window " in line:
                    focus.append("not focused" not in line)
            continue
        m = CP.search(line)
        if m and pacing and first_frame + 300 <= frame <= last_frame and len(cp) < len(pacing):
            cp.append({k.strip(): float(v) for k, v in (part.rsplit(" ", 1) for part in m.group(1).split(", "))})
    if not pacing:
        return {}
    keys = ["avg_ms", "max_ms", "over50", "gpu_wait_ms", "submit_ms", "longest_present_ms", "ring_wait_ms"]
    result = {k: round(statistics.mean(p[i] for p in pacing), 3) for i, k in enumerate(keys)}
    if cp:
        result["cp_ms"] = {k: round(statistics.mean(c[k] for c in cp), 3) for k in cp[0]}
        result["cp_total_ms"] = round(sum(result["cp_ms"].values()), 3)
    result["lines"] = len(pacing)
    if focus:
        result["window_focused"] = f"{sum(focus)}/{len(focus)}"
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("name")
    parser.add_argument("--settings", default="video.resolution=1920x1080;graphics.upscaler=off")
    parser.add_argument("--fps", type=int, default=0, help="limite de FPS no trecho medido (0 = sem limite); até lá, 30")
    parser.add_argument("--unlock-at", type=float, default=110.5, help="tempo do replay em que o limite muda")
    parser.add_argument("--env", default="", help="variáveis extras: A=1;B=2")
    parser.add_argument("--exe")
    parser.add_argument("--affinity", type=lambda s: int(s, 0))
    parser.add_argument("--sample", type=int, help="intervalo do amostrador em us")
    parser.add_argument("--sample-start", type=float, default=168.0, help="segundos depois de abrir o jogo")
    parser.add_argument("--sample-seconds", type=float, default=25.0)
    parser.add_argument("--profile", default="desempenho_vila")
    parser.add_argument("--window", default=None, help="trecho medido no relógio INPUT, ex.: 152,195 (padrão do desempenho_vila)")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--reanalyze", action="store_true", help="só recalcula o <nome>.json a partir da pasta da execução")
    args = parser.parse_args()
    if args.reanalyze:
        result = json.load(open(OUT / f"{args.name}.json", encoding="utf-8"))
        run_dir = Path(result["run_dir"])
        span = tuple(float(x) for x in args.window.split(",")) if args.window else unlock_window(run_dir)
        result["frames"] = frame_window(run_dir / "frametimes.csv", span)
        result["pacing"] = pacing_window(run_dir / "run.log", result["frames"]["first_frame"], result["frames"]["last_frame"])
        (OUT / f"{args.name}.json").write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
        f = result["frames"]
        print(f"{args.name}: {f['fps']} fps, {f['ms_avg']} ms, p99 {f['ms_p99']} ms, 1% low {f['fps_1pct_low']} fps, janela {f['input_s']}, "
              f"CP {result['pacing'].get('cp_total_ms')} ms/quadro")
        return

    OUT.mkdir(parents=True, exist_ok=True)
    # --set env=... replaces the profile's env in run_game.py: start from it.
    import tomllib
    profile_env = tomllib.load(open(ROOT / "config" / "run.toml", "rb")).get("profiles", {}).get(args.profile, {}).get("env", {})
    env = dict(profile_env)
    # desempenho_vila reaches the village through the opening cutscene, which must run at 30 fps
    # (it reads the controller once per frame): the limit changes when gameplay starts. The other
    # profiles (direct boot, cutscene skipped) use the limit from the start.
    staged = args.profile == "desempenho_vila"
    env["CRASH_SETTINGS"] = f"video.fps_limit={30 if staged else args.fps};graphics.hd_remaster=false;" + args.settings
    if staged and args.fps != 30:
        env["CRASH_FPS_LIMIT_AT"] = f"{args.unlock_at:g}:{args.fps}"
    for part in filter(None, args.env.split(";")):
        k, v = part.split("=", 1)
        env[k] = v
    env_toml = "{" + ",".join(f'{k}="{v}"' for k, v in env.items()) + "}"
    print(f"   env: {env_toml}")
    command = [sys.executable, str(ROOT / "tools" / "run_game.py"), args.profile, "--set", f"env={env_toml}"]
    if args.exe:
        command += ["--exe", args.exe]
    elif args.no_build:
        command.append("--no-build")

    print(f"== {args.name}: {args.settings} {args.env}")
    process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                               encoding="utf-8", errors="replace")
    run_dir, pid, output = None, None, []
    sampler = None
    for line in process.stdout:
        output.append(line)
        print("   " + line.rstrip())
        m = re.search(r"pasta: (.*)", line)
        if m:
            run_dir = Path(m.group(1).strip())
        m = re.search(r"jogo: PID (\d+)", line)
        if m and pid is None:
            pid = int(m.group(1))
            if args.affinity:
                print(f"   afinidade 0x{args.affinity:X}: {set_affinity(pid, args.affinity)}")
            if args.sample:
                samples = OUT / f"{args.name}.samples.txt"

                def run_sampler():
                    time.sleep(args.sample_start)
                    subprocess.run([str(SAMPLER), str(pid), str(args.sample_seconds), str(args.sample), str(samples)])

                sampler = threading.Thread(target=run_sampler)
                sampler.start()
    process.wait()
    if sampler:
        sampler.join()

    result = {"name": args.name, "settings": args.settings, "fps_limit": args.fps, "env": args.env, "exe": args.exe, "affinity": args.affinity,
              "run_dir": str(run_dir), "pid": pid, "exit": process.returncode,
              "time": time.strftime("%Y-%m-%d %H:%M:%S")}
    span = tuple(float(x) for x in args.window.split(",")) if args.window else unlock_window(run_dir)
    result["frames"] = frame_window(run_dir / "frametimes.csv", span, seconds=46.0, tail=1.0)
    result["pacing"] = pacing_window(run_dir / "run.log", result["frames"]["first_frame"], result["frames"]["last_frame"])
    (OUT / f"{args.name}.json").write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    f = result["frames"]
    print(f"   -> {f['fps']} fps, {f['ms_avg']} ms médio, p99 {f['ms_p99']} ms, 1% low {f['fps_1pct_low']} fps, "
          f"{f['over_25ms']} > 25 ms; CP {result['pacing'].get('cp_total_ms')} ms/quadro, GPU wait {result['pacing'].get('gpu_wait_ms')} ms, "
          f"janela em foco {result['pacing'].get('window_focused', '?')}")


if __name__ == "__main__":
    main()
