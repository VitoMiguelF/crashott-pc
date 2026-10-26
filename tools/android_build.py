"""Builds the Android app (docs/internals/android_port.md): libmain.so + libSDL3.so for arm64, SDL3's
Java glue, the APK assembled with aapt2 / d8 / zipalign / apksigner (no Gradle, no Android Studio).

    python tools/android_build.py               # configure if needed, build, assemble the APK
    python tools/android_build.py build         # only libmain.so / libSDL3.so (CMake + Ninja)
    python tools/android_build.py apk           # only the APK, from what build left

The APK holds the game recompiled from the player's own dump (generated/ppc): it is personal, for
their phone, sideloaded; never published, never in git (build/ is not). Installing it and copying
the game to the phone are separate steps the player runs (adb install, adb push).

Needs (unpacked under build/android; tools/toolchain.py will fetch them for players later):
  ndk/                    Android NDK r30 (toolchains/llvm/prebuilt/<host>, build/cmake)
  build-tools/android-37.0/   aapt2, zipalign, lib/d8.jar, lib/apksigner.jar
  platform/android-37.2/android.jar
  sdl/SDL3-3.4.18/        the SDL source: libSDL3.so and android-project's Java glue
and a JDK (javac, keytool, java) on PATH or in JAVA_HOME, plus the bundled toolchain's CMake/Ninja.
"""
import argparse
import os
import secrets
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build" / "android"
NDK = WORK / "ndk"
BUILD_TOOLS = WORK / "build-tools" / "android-37.0"
ANDROID_JAR = WORK / "platform" / "android-37.2" / "android.jar"
SDL_SOURCE = WORK / "sdl" / "SDL3-3.4.18"
CMAKE_BUILD = WORK / "cmake"
OUT = WORK / "apk"
KEYSTORE = WORK / "keystore" / "crashrecomp.jks"  # made once; updates need the same key
MIN_SDK = 30     # memfd_create in bionic
TARGET_SDK = 35
WINDOWS = sys.platform == "win32"
EXE = ".exe" if WINDOWS else ""
HOST = "windows-x86_64" if WINDOWS else "linux-x86_64"


def say(text):
    print(f"[android] {text}", flush=True)


def run(command, **kwargs):
    command = [str(c) for c in command]
    result = subprocess.run(command, **kwargs)
    if result.returncode:
        sys.exit(f"failed ({result.returncode}): {' '.join(command)}")
    return result


def jdk_tool(name):
    home = os.environ.get("JAVA_HOME")
    if home and (Path(home) / "bin" / (name + EXE)).is_file():
        return Path(home) / "bin" / (name + EXE)
    found = shutil.which(name)
    if not found:
        sys.exit(f"{name} not found: install a JDK 17+ (or set JAVA_HOME)")
    return Path(found)


def build_env():
    env = dict(os.environ)
    tc = ROOT / "toolchain"
    extra = [tc / "cmake" / "bin", tc / "ninja"]
    env["PATH"] = os.pathsep.join([str(p) for p in extra if p.is_dir()] + [env.get("PATH", "")])
    return env


def build():
    for path in (NDK / "build" / "cmake" / "android.toolchain.cmake", SDL_SOURCE / "CMakeLists.txt"):
        if not path.is_file():
            sys.exit(f"{path.relative_to(ROOT)} not found (see this script's docstring)")
    if not any((ROOT / "generated" / "ppc").glob("ppc_recomp.*.cpp")):
        sys.exit("generated/ppc is empty: recompile the dump first (tools/setup.py)")
    env = build_env()
    if not (CMAKE_BUILD / "CMakeCache.txt").is_file():
        say("configuring (NDK, arm64-v8a, API %d)" % MIN_SDK)
        run(["cmake", "-S", ROOT, "-B", CMAKE_BUILD, "-G", "Ninja",
             f"-DCMAKE_TOOLCHAIN_FILE={(NDK / 'build/cmake/android.toolchain.cmake').as_posix()}",
             "-DANDROID_ABI=arm64-v8a", f"-DANDROID_PLATFORM=android-{MIN_SDK}", "-DANDROID_STL=c++_static",
             "-DCMAKE_BUILD_TYPE=Release", f"-DCRASH_SDL3_SOURCE={SDL_SOURCE.as_posix()}", "-Wno-dev"], env=env)
    say("building libmain.so and libSDL3.so")
    run(["cmake", "--build", CMAKE_BUILD, "--target", "CrashRecomp"], env=env)


def keystore():
    password_file = KEYSTORE.with_suffix(".password")
    if not KEYSTORE.is_file():
        KEYSTORE.parent.mkdir(parents=True, exist_ok=True)
        password = secrets.token_hex(16)
        password_file.write_text(password, encoding="ascii")
        say(f"new signing key: {KEYSTORE.relative_to(ROOT)} (keep it: an update signed with another key won't install)")
        run([jdk_tool("keytool"), "-genkeypair", "-keystore", KEYSTORE, "-alias", "crashrecomp", "-keyalg", "RSA", "-keysize", "3072",
             "-validity", "10000", "-storepass", password, "-keypass", password, "-dname", "CN=CrashRecomp personal build"],
            stdout=subprocess.DEVNULL)
    return password_file.read_text(encoding="ascii").strip()


def apk():
    libmain = CMAKE_BUILD / "libmain.so"
    libsdl = next(iter(sorted(CMAKE_BUILD.rglob("libSDL3.so"))), None)
    if not libmain.is_file() or not libsdl:
        sys.exit("libmain.so / libSDL3.so not built: run `python tools/android_build.py build` first")
    if OUT.exists():
        shutil.rmtree(OUT)
    classes = OUT / "classes"
    classes.mkdir(parents=True)

    # Java: SDL's glue + CrashActivity -> classes.dex
    sources = sorted((SDL_SOURCE / "android-project/app/src/main/java").rglob("*.java")) + \
        sorted((ROOT / "android" / "java").rglob("*.java"))
    say(f"javac: {len(sources)} files")
    (OUT / "sources.txt").write_text("\n".join(f'"{s.as_posix()}"' for s in sources), encoding="utf-8")
    run([jdk_tool("javac"), "--release", "11", "-nowarn", "-Xlint:none", "-classpath", ANDROID_JAR, "-d", classes,
         f"@{OUT / 'sources.txt'}"], stdout=subprocess.DEVNULL)
    class_files = [str(p) for p in classes.rglob("*.class")]
    run([jdk_tool("java"), "-cp", BUILD_TOOLS / "lib" / "d8.jar", "com.android.tools.r8.D8", "--release", "--min-api", MIN_SDK,
         "--lib", ANDROID_JAR, "--output", OUT, *class_files])

    # Manifest -> base APK (no resources beyond the manifest yet)
    say("aapt2 link")
    run([BUILD_TOOLS / ("aapt2" + EXE), "link", "-o", OUT / "base.apk", "-I", ANDROID_JAR, "--manifest", ROOT / "android" / "AndroidManifest.xml",
         "--min-sdk-version", MIN_SDK, "--target-sdk-version", TARGET_SDK])

    # classes.dex + the .so files (stored, not compressed: extractNativeLibs=false maps them in place;
    # libmain.so without its debug sections)
    strip = NDK / "toolchains" / "llvm" / "prebuilt" / HOST / "bin" / ("llvm-strip" + EXE)
    stripped = OUT / "libmain.so"
    run([strip, "--strip-debug", "-o", stripped, libmain])
    unaligned = OUT / "unaligned.apk"
    shutil.copy(OUT / "base.apk", unaligned)
    with zipfile.ZipFile(unaligned, "a") as z:
        z.write(OUT / "classes.dex", "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
        z.write(libsdl, "lib/arm64-v8a/libSDL3.so", compress_type=zipfile.ZIP_STORED)
        z.write(stripped, "lib/arm64-v8a/libmain.so", compress_type=zipfile.ZIP_STORED)

    # 16 KB alignment of the stored .so files (-P 16), 4 bytes for the rest; then the signature.
    aligned = OUT / "aligned.apk"
    run([BUILD_TOOLS / ("zipalign" + EXE), "-P", "16", "-f", "4", unaligned, aligned])
    final = OUT / "CrashOTTDE-personal.apk"
    password = keystore()
    run([jdk_tool("java"), "-jar", BUILD_TOOLS / "lib" / "apksigner.jar", "sign", "--ks", KEYSTORE, "--ks-pass", f"pass:{password}",
         "--min-sdk-version", MIN_SDK, "--out", final, aligned])
    run([jdk_tool("java"), "-jar", BUILD_TOOLS / "lib" / "apksigner.jar", "verify", "--min-sdk-version", MIN_SDK, final])
    size = final.stat().st_size
    say(f"{final.relative_to(ROOT)}: {size / 2**20:.1f} MB (personal: holds your recompiled game, don't share it)")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("step", nargs="?", default="all", choices=["all", "build", "apk"])
    step = parser.parse_args().step
    if step in ("all", "build"):
        build()
    if step in ("all", "apk"):
        apk()


if __name__ == "__main__":
    main()
