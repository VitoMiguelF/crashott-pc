"""What in runtime/ still needs Win32 (the Linux port, docs/internals/linux_port.md).

    python tools/check_portability.py [substring ...]   # only the runtime files whose path has one

Syntax-checks every runtime translation unit of the Windows build (build/compile_commands.json,
made with `ninja -t compdb` here) with the Win32 headers emptied and CRASH_NO_WIN32_HEADERS
defined: whatever still compiles only because of <windows.h> & co. shows up as an undeclared
name. The OS backends (*_win32.cpp, runtime/platform/) are skipped; *_posix.cpp / *_sdl.cpp are not
in the Windows build at all, so this is no substitute for a real Linux compile.

Known false positive: gpu/rhi_vulkan_shaders.cpp (dxcapi.h takes its Windows branch because _WIN32
stays defined; on Linux it uses the vendored WinAdapter.h).
"""

import json
import os
import re
import shutil
import subprocess
import sys
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
HEADERS = BUILD / "portability_headers"
WIN32_HEADERS = [
    "windows.h", "windowsx.h", "xinput.h", "xaudio2.h", "timeapi.h", "shellapi.h", "shobjidl.h", "shlobj.h",
    "commdlg.h", "objidl.h", "unknwn.h", "wincodec.h", "dwmapi.h", "winhttp.h", "tlhelp32.h", "psapi.h",
    "dbghelp.h", "setupapi.h", "cfgmgr32.h", "devguid.h", "initguid.h", "hidsdi.h", "hidpi.h", "hidusage.h",
    "wrl/client.h",
]
SKIP = ("/platform/", "_win32.cpp", "dlss_module", "xex_image")
KNOWN = {"runtime/gpu/rhi_vulkan_shaders.cpp"}


def ninja():
    for candidate in (ROOT / "toolchain" / "ninja" / "ninja.exe", ROOT / "toolchain" / "ninja" / "ninja"):
        if candidate.exists():
            return str(candidate)
    return shutil.which("ninja") or "ninja"


def main():
    filters = sys.argv[1:]
    if not (BUILD / "build.ninja").exists():
        sys.exit("build/ has no Ninja build: build the game first (python tools/run_game.py ... or build_game.cmd)")
    compdb = subprocess.run([ninja(), "-C", str(BUILD), "-t", "compdb"], capture_output=True, text=True, check=True).stdout
    (BUILD / "compile_commands.json").write_text(compdb, encoding="utf-8")
    for name in WIN32_HEADERS:
        path = HEADERS / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#pragma once\n", encoding="ascii")

    jobs = []
    for entry in json.loads(compdb):
        file = entry["file"].replace("\\", "/")
        command = entry.get("command", "")
        if "CrashRecomp.dir" not in entry.get("output", "") + command or "/runtime/" not in file:
            continue
        if any(s in file for s in SKIP) or (filters and not any(f in file for f in filters)):
            continue
        jobs.append(entry)

    env = dict(os.environ)
    toolchain_bin = ROOT / "toolchain" / "llvm-mingw" / "bin"
    if toolchain_bin.exists():
        env["PATH"] = str(toolchain_bin) + os.pathsep + env["PATH"]

    def check(entry):
        command = re.sub(r"\s-o\s+\S+", " ", entry["command"]).replace(" -c ", " ")
        command += f' -fsyntax-only -isystem "{HEADERS}" -DCRASH_NO_WIN32_HEADERS -ferror-limit=0 -w'
        result = subprocess.run(command, shell=True, cwd=entry["directory"], capture_output=True, text=True, env=env)
        return entry["file"], result.stdout + result.stderr

    names = defaultdict(Counter)
    with ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        for file, output in pool.map(check, jobs):
            rel = Path(file).resolve().relative_to(ROOT).as_posix()
            for m in re.finditer(r"error: (?:use of undeclared identifier|unknown type name|no member named|no type named) '([^']+)'", output):
                names[rel][m.group(1)] += 1
            for line in output.splitlines():
                if "error:" in line and not re.search(r"undeclared identifier|unknown type name|no member named|no type named", line):
                    names[rel]["<" + line.split("error:", 1)[1].strip()[:70] + ">"] += 1
    for rel in sorted(names):
        known = " (known false positive)" if rel in KNOWN else ""
        print(f"{rel}{known}: " + ", ".join(name for name, _ in names[rel].most_common(30)))
    real = [rel for rel in names if rel not in KNOWN]
    print(f"\n{len(real)} of {len(jobs)} runtime files still need Win32" + (f" (+{len(names) - len(real)} known)" if len(names) > len(real) else ""))
    return 1 if real else 0


if __name__ == "__main__":
    sys.exit(main())
