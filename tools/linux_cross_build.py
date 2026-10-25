"""Cross-compiles the game for Linux x86-64 from Windows with zig (docs/internals/linux_port.md).

    python tools/linux_cross_build.py              # every object, then the link -> build/linux/CrashRecomp
    python tools/linux_cross_build.py check [substring ...]  # runtime files only, -O0, errors listed
    python tools/linux_cross_build.py link         # the link alone (objects already built)

Needs (nothing is downloaded here; build/ is not in git):
  build/downloads/zig/ziglang/zig.exe     the ziglang wheel from PyPI, unzipped (pip download ziglang)
  build/downloads/sdl/SDL3-*/include      the SDL3 3.2.x source tarball, unpacked (headers only)

No CMake: the same sources as CMakeLists.txt's non-WIN32 branch (runtime/ without *_win32.cpp,
generated/ppc, XexImage), glibc 2.35, -mavx, libc++ linked statically. SDL3 is not built here
(no X11/Wayland headers): the link uses a stub libSDL3.so.0 holding just the SDL functions the
game calls, and the system's SDL3 takes its place at run time. Anything else left undefined is
listed (a platform function without a POSIX version).
"""

import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOWNLOADS = ROOT / "build" / "downloads"
ZIG = DOWNLOADS / "zig" / "ziglang" / ("zig.exe" if os.name == "nt" else "zig")
OUT = ROOT / "build" / "linux"
TARGET = "x86_64-linux-gnu.2.35"
XR = ROOT / "tools" / "XenonRecomp"


def sdl_include():
    found = sorted((DOWNLOADS / "sdl").glob("SDL3-*/include"))
    if not found:
        sys.exit("build/downloads/sdl/SDL3-*/include not found: unpack the SDL3 3.2.x source tarball there")
    return found[-1]


def jobs(kind):
    common = ["-target", TARGET, "-mavx", "-fno-strict-aliasing", "-O2", "-DNDEBUG", "-w"]
    simde = f"-I{XR / 'thirdparty/simde'}"
    runtime = common + ["-std=gnu++20", "-DCRASH_HAVE_FFMPEG=1", "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN",
                        f"-I{ROOT / 'thirdparty/ffmpeg/include'}", f"-I{ROOT / 'thirdparty/vulkan/include'}",
                        f"-I{ROOT / 'thirdparty/dxc/include'}", f"-I{ROOT / 'thirdparty/stb'}", f"-I{ROOT / 'generated/ppc'}", simde,
                        f"-I{sdl_include()}"]
    xex = [f"-I{XR / 'XenonUtils'}", simde, f"-I{XR / 'thirdparty/libmspack/libmspack/mspack'}",
           f"-I{XR / 'thirdparty/tiny-AES-c'}", f"-I{XR / 'thirdparty/TinySHA1'}", "-DNOMINMAX"]
    result = []
    if kind in ("runtime", "all"):
        for path in sorted((ROOT / "runtime").rglob("*.cpp")):
            rel = path.relative_to(ROOT).as_posix()
            if "/xex_image/" not in rel and "/gpu/dlss_module/" not in rel and not rel.endswith("_win32.cpp"):
                result.append(("c++", path, runtime))
    if kind == "all":
        ppc = common + ["-std=gnu++20", f"-I{ROOT / 'generated/ppc'}", simde]
        result += [("c++", path, ppc) for path in sorted((ROOT / "generated" / "ppc").glob("*.cpp"))]
        result.append(("c++", ROOT / "runtime/xex_image/xex_image.cpp", common + ["-std=gnu++20"] + xex))
        for name in ["xex.cpp", "image.cpp", "xex_patcher.cpp", "memory_mapped_file.cpp"]:
            result.append(("c++", XR / "XenonUtils" / name, common + ["-std=gnu++20"] + xex))
        for name in ["thirdparty/libmspack/libmspack/mspack/lzxd.c", "thirdparty/tiny-AES-c/aes.c"]:
            result.append(("cc", XR / name, common + xex))
    return result


def compile_one(job, check):
    lang, source, flags = job
    folder = OUT / ("check" if check else "obj")
    folder.mkdir(parents=True, exist_ok=True)
    obj = folder / (source.relative_to(ROOT).as_posix().replace("/", "__") + ".o")
    command = [str(ZIG), lang] + flags + (["-O0", "-ferror-limit=0"] if check else []) + ["-c", str(source), "-o", str(obj)]
    result = subprocess.run(command, capture_output=True, text=True, cwd=ROOT)
    return source, result.returncode, result.stdout + result.stderr


def compile_all(kind, filters, check):
    selected = [j for j in jobs(kind) if not filters or any(f in j[1].as_posix() for f in filters)]
    failed = 0
    with ThreadPoolExecutor(int(os.environ.get("JOBS", "6"))) as pool:  # zig processes are heavy: not cpu_count
        for source, code, output in pool.map(lambda j: compile_one(j, check), selected):
            errors = [line for line in output.splitlines() if "error:" in line]
            if code or errors:
                failed += 1
                print(f"== {source.relative_to(ROOT).as_posix()} ({len(errors)} errors)")
                for line in errors[:12]:
                    print("   " + line[:240])
    print(f"{failed} of {len(selected)} failed")
    return failed


def link():
    objects = sorted((OUT / "obj").glob("*.o"))
    exe = OUT / "CrashRecomp"
    arguments = ["-target", TARGET] + [o.as_posix() for o in objects] + ["-lpthread", "-ldl", "-o", exe.as_posix()]
    response = OUT / "link.rsp"

    def run(extra=()):
        response.write_text("\n".join(f'"{a}"' for a in arguments + list(extra)), encoding="utf-8")
        result = subprocess.run([str(ZIG), "c++", f"@{response}"], capture_output=True, text=True, cwd=ROOT)
        return result.returncode, result.stdout + result.stderr

    code, output = run()
    undefined = sorted(set(re.findall(r"undefined symbol: (\S+)", output)))
    sdl = [u for u in undefined if u.startswith("SDL_")]
    other = [u for u in undefined if not u.startswith("SDL_")]
    if other:
        print(f"undefined outside SDL ({len(other)}):")
        for name in other:
            print("   " + name)
        return 1
    if code and not sdl:
        print(output[:3000])
        return 1
    if sdl:
        stub = OUT / "sdl_stub.c"
        stub.write_text("".join(f"void {name}(void) {{}}\n" for name in sdl), encoding="ascii")
        library = OUT / "libSDL3.so.0"
        result = subprocess.run([str(ZIG), "cc", "-target", TARGET, "-shared", "-fPIC", "-Wl,-soname,libSDL3.so.0",
                                 str(stub), "-o", str(library)], capture_output=True, text=True)
        if result.returncode:
            print(result.stdout + result.stderr)
            return 1
        code, output = run([library.as_posix()])
        if code:
            print(output[:3000])
            return 1
    print(f"linked {exe.relative_to(ROOT).as_posix()} ({len(objects)} objects; {len(sdl)} SDL3 functions from the system's libSDL3.so.0)")
    return 0


def main():
    if not ZIG.exists():
        sys.exit(f"{ZIG.relative_to(ROOT)} not found: unzip the ziglang wheel (pip download ziglang) into build/downloads/zig")
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    if mode == "check":
        return 1 if compile_all("runtime", sys.argv[2:], check=True) else 0
    if mode == "link":
        return link()
    return 1 if compile_all("all", [], check=False) else link()


if __name__ == "__main__":
    sys.exit(main())
