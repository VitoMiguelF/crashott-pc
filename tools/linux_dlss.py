"""Builds DLSS for the Linux release (thirdparty/dlss/bin): run once on the release machine (clang + lld),
never on the player's, like CrashDlss.dll on Windows.

    python3 tools/linux_dlss.py

- libCrashDlss.so: our runtime/gpu/dlss_module/crash_dlss.cpp linked with NGX's Linux static library
  (lib/Linux_x86_64/libnvsdk_ngx.a of the DLSS SDK 310.9.16, the version setup.py uses on Windows),
  against the Steam Runtime 3 sysroot (tools/linux_sdl3.py unpacks it: glibc 2.31). NGX's library is
  built with GCC and needs libstdc++, so libCrashDlss.so links the system's libstdc++.so.6; the game
  (static libc++) only calls its C interface. The SDK reaches players only in object code form inside
  it, as its license allows.
- libnvidia-ngx-dlss.so.310.9.16: NVIDIA's DLSS runtime (lib/Linux_x86_64/rel), which NGX loads from
  the folder CrashDlss_Init names (the exe's).
The driver's own NGX core (libnvidia-ngx.so.1, NVIDIA's proprietary driver) does the rest: DLSS runs
only on an NVIDIA RTX GPU with that driver, which CrashDlss_Probe checks.

The installer carries both as its "dlss" component (tools/make_appimage.py) and installs them only
where the probe says DLSS runs; CMake copies them next to the exe when they are here.
"""
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import linux_sdl3  # noqa: E402  (the sysroot)
import setup  # noqa: E402  (DOWNLOADS: the SDK's headers, pinned)

ROOT = linux_sdl3.ROOT
WORK = ROOT / "build" / "dlss-linux"
OUT = ROOT / "thirdparty" / "dlss" / "bin"
SDK = "https://github.com/NVIDIA/DLSS/raw/v310.9.16/"
NGX_LIB = (SDK + "lib/Linux_x86_64/libnvsdk_ngx.a", "d3c50bb0d9624fc35e502a8e929b8fc1e2b505017ebdb84c0d5b627e11a235df")
NGX_DLSS = (SDK + "lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16",
            "30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6")


def main():
    for tool in ("clang++", "ld.lld"):
        if not shutil.which(tool):
            raise SystemExit(f"falta {tool} nesta máquina (a de release)")
    sysroot = WORK.parent / "steamrt" / "sysroot"
    linux_sdl3.unpack_sysroot(linux_sdl3.download(*linux_sdl3.SYSROOT, linux_sdl3.DOWNLOADS / "steamrt" /
                                                  "com.valvesoftware.SteamRuntime.Sdk-amd64,i386-sniper-sysroot.tar.gz"), sysroot)
    include = WORK / "include"
    for url, dest, digest in setup.DOWNLOADS:
        if dest.startswith("thirdparty/dlss/include/"):
            linux_sdl3.download(url, digest, include / Path(dest).name)
    lib = linux_sdl3.download(*NGX_LIB, linux_sdl3.DOWNLOADS / "dlss-linux" / "libnvsdk_ngx.a")
    runtime = linux_sdl3.download(*NGX_DLSS, linux_sdl3.DOWNLOADS / "dlss-linux" / Path(NGX_DLSS[0]).name)

    OUT.mkdir(parents=True, exist_ok=True)
    target = OUT / "libCrashDlss.so"
    command = ["clang++", "--target=x86_64-linux-gnu", f"--sysroot={sysroot}", "-fuse-ld=lld", "-shared", "-fPIC", "-O2",
               "-std=c++17", "-fvisibility=hidden", "-DCRASH_HAVE_DLSS=1", f"-I{include}", f"-I{ROOT / 'thirdparty' / 'vulkan' / 'include'}",
               str(ROOT / "runtime" / "gpu" / "dlss_module" / "crash_dlss.cpp"), str(lib), "-ldl", "-Wl,--no-undefined",
               "-Wl,--as-needed", "-Wl,-soname,libCrashDlss.so", "-o", str(target)]
    subprocess.run(command, check=True)
    shutil.copy2(runtime, OUT / runtime.name)
    needed = [w.strip("[]") for w in subprocess.run(["readelf", "-d", str(target)], capture_output=True, text=True, check=True).stdout.split()
              if w.startswith("[lib") or w.startswith("[ld-")]
    exports = subprocess.run(["nm", "-D", "--defined-only", str(target)], capture_output=True, text=True, check=True).stdout
    names = sorted(line.split()[-1] for line in exports.splitlines() if " T CrashDlss_" in line)
    print(f"{target}: NEEDED {', '.join(needed)}; exports {', '.join(names)}")
    print(f"{OUT / runtime.name}: NVIDIA's DLSS runtime")
    return 0


if __name__ == "__main__":
    sys.exit(main())
