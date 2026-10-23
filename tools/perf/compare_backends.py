r"""Runs a tools/run_game.py profile on Direct3D 12 and on Vulkan, one after the other, and compares
them: exit, frames, frame times (run_game's perf summary) and the screenshots pixel by pixel, plus a
side-by-side PNG (D3D12 left, Vulkan right).

    python tools\perf\compare_backends.py vila_sem_cena
    python tools\perf\compare_backends.py configuracoes --set "env={ CRASH_SETTINGS = \"video.antialiasing=smaa\" }"
    python tools\perf\compare_backends.py menu --only vulkan --set gpu_debug=true

Run folders go to build\vulkan\<backend>\ (--runs to change). Screenshots are paired in order; with
animated scenes the frame a shot lands on can differ by a few frames between the runs.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare_runs  # noqa: E402


def run(profile, backend, runs, extra, no_build):
    folder = runs / backend
    command = [sys.executable, str(ROOT / "tools" / "run_game.py"), profile, "--runs", str(folder), "--set", f"renderer={backend}"]
    for item in extra:
        command += ["--set", item]
    if no_build:
        command.append("--no-build")
    print(f"== {profile} on {backend}")
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    for line in result.stdout.splitlines():
        if any(key in line for key in ("jogo: PID", "saída:", "desempenho:", "[FALHOU]")):
            print("  " + line.strip())
    runs_made = sorted(folder.glob(f"*_{profile}"))
    return runs_made[-1] if runs_made else None


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser()
    parser.add_argument("profile")
    parser.add_argument("--set", action="append", default=[], help="passed to run_game.py (KEY=VALUE)")
    parser.add_argument("--runs", default=str(ROOT / "build" / "vulkan"))
    parser.add_argument("--only", choices=["d3d12", "vulkan"], help="run only one backend")
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    runs = Path(args.runs)
    folders = {}
    for backend in ("d3d12", "vulkan"):
        if args.only and args.only != backend:
            continue
        folders[backend] = run(args.profile, backend, runs, args.set, args.no_build or backend == "vulkan")
    if len(folders) < 2 or not all(folders.values()):
        return
    a, b = folders["d3d12"], folders["vulkan"]
    for folder in (a, b):
        result = json.loads((folder / "result.json").read_text(encoding="utf-8"))
        perf = result.get("perf", {})
        print(f"{folder.parent.name:6} {folder.name}: {result.get('exit')}, {result.get('frames')} frames, "
              f"{perf.get('avg_fps', 0):.1f} fps avg, p99 {perf.get('p99_ms', 0):.1f} ms, 1% low {perf.get('low_1pct_fps', 0):.1f} fps")
    sys.argv = ["compare_runs", str(a), str(b)]
    compare_runs.main()
    out = runs / f"{args.profile}_d3d12_vs_vulkan.png"
    subprocess.run([sys.executable, str(Path(__file__).resolve().parent / "side_by_side.py"), str(a), str(b), str(out)], check=False)
    print(f"side by side: {out}")


if __name__ == "__main__":
    main()
