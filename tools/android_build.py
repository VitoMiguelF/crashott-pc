"""Builds the Android app (docs/internals/android_port.md): libmain.so + libSDL3.so for arm64, SDL3's
Java glue, the APK assembled with aapt2 / d8 / zipalign / apksigner (no Gradle, no Android Studio).

    python tools/android_build.py               # configure if needed, build, assemble the APK
    python tools/android_build.py build         # only libmain.so / libSDL3.so (CMake + Ninja)
    python tools/android_build.py apk           # only the APK, from what build left
    python tools/android_build.py turnip        # Mesa's Turnip (Adreno 6xx/7xx with an old driver)
    python tools/android_build.py ffmpeg        # FFmpeg's WMA Pro + Vorbis decoders (the game's audio)
    python tools/android_build.py llvm          # clang + lld that run on the phone (the single APK)
    python tools/android_build.py dxc           # libdxcompiler.so: the shaders compiled on the phone

The APK holds the game recompiled from the player's own dump (generated/ppc): it is personal, for
their phone, sideloaded; never published, never in git (build/ is not). Installing it and copying
the game to the phone are separate steps the player runs (adb install, adb push).

Needs (unpacked under build/android; tools/toolchain.py will fetch them for players later):
  ndk/                    Android NDK r30 (toolchains/llvm/prebuilt/<host>, build/cmake)
  build-tools/android-37.0/   aapt2, zipalign, lib/d8.jar, lib/apksigner.jar
  platform/android-37.2/android.jar
  sdl/SDL3-3.4.18/        the SDL source: libSDL3.so and android-project's Java glue
and a JDK (javac, keytool, java) on PATH or in JAVA_HOME, plus the bundled toolchain's CMake/Ninja.

Optional, for Adreno GPUs whose own driver is older than Vulkan 1.3 (the Galaxy S20 FE's Adreno 650):
  turnip/mesa-26.2.5/     the Mesa source (archive.mesa3d.org), built by the `turnip` step
  turnip/adrenotools/     libadrenotools (github.com/bylaws/libadrenotools) with lib/linkernsbypass
  turnip/pytools/         meson, mako, markupsafe, pyyaml (pip --target); turnip/winflexbison/ on Windows
With them the APK carries libvulkan_freedreno.so and libadrenotools' hooks (android_main_android.cpp).

Optional, the game's audio (XMA through FFmpeg's WMA Pro decoder, audio/ffmpeg.cpp):
  ffmpeg/ffmpeg-8.1.3/    the FFmpeg source (ffmpeg.org), built by the `ffmpeg` step: avcodec + avutil
                          with only the WMA Pro and Vorbis decoders, as libavcodec.so / libavutil.so
  ffmpeg/bin/make.exe     GNU make (Windows: built from make-4.4.1 with build_w32.bat gcc)
Without it the game runs silent.

For the single APK (the game recompiled on the phone itself), a compiler that runs there:
  build/downloads/android/llvm/llvm-project-21.1.8.src/   the LLVM source (github.com/llvm/llvm-project
                          releases), built by the `llvm` step: first llvm-tblgen / clang-tblgen for
                          this machine, then clang and lld for Android arm64 (AArch64 target only)
  build/downloads/android/dxc/   DirectXShaderCompiler (git clone --recursive github.com/microsoft/
                          DirectXShaderCompiler), built by the `dxc` step the same way: its own
                          table generators here, then libdxcompiler.so (SPIR-V) for Android arm64,
                          which the APK carries (rhi_vulkan_shaders.cpp loads it); on Windows its
                          table generators need Visual Studio's C++ tools with the C++ ATL component
"""
import argparse
import os
import re
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
TURNIP = WORK / "turnip"
MESA_SOURCE = TURNIP / "mesa-26.2.5"
ADRENOTOOLS_SOURCE = TURNIP / "adrenotools"
TURNIP_BUILD = TURNIP / "build-tu"
TURNIP_LIB = TURNIP_BUILD / "src" / "freedreno" / "vulkan" / "libvulkan_freedreno.so"
FFMPEG = WORK / "ffmpeg"
FFMPEG_SOURCE = FFMPEG / "ffmpeg-8.1.3"
FFMPEG_BUILD = FFMPEG / "build-arm64"
FFMPEG_INSTALL = FFMPEG / "install"
FFMPEG_LIBS = [FFMPEG_INSTALL / "lib" / name for name in ("libavcodec.so", "libavutil.so")]
DOWNLOADS = ROOT / "build" / "downloads" / "android"
LLVM_SOURCE = DOWNLOADS / "llvm" / "llvm-project-21.1.8.src"
LLVM = WORK / "llvm"
LLVM_HOST_BUILD = LLVM / "build-host"    # the table generators, for this machine
LLVM_BUILD = LLVM / "build-arm64"        # clang and lld for the phone
LLVM_TOOLS = [LLVM_BUILD / "bin" / name for name in ("clang", "lld")]
DXC_SOURCE = DOWNLOADS / "dxc"
DXC = WORK / "dxc"
DXC_HOST_BUILD = DXC / "build-host"
DXC_BUILD = DXC / "build-arm64"
DXC_LIB = DXC_BUILD / "lib" / "libdxcompiler.so"
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
    adrenotools = ADRENOTOOLS_SOURCE if (ADRENOTOOLS_SOURCE / "lib" / "linkernsbypass" / "CMakeLists.txt").is_file() else None
    say("configuring (NDK, arm64-v8a, API %d%s)" % (MIN_SDK, ", libadrenotools" if adrenotools else ""))
    run(["cmake", "-S", ROOT, "-B", CMAKE_BUILD, "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={(NDK / 'build/cmake/android.toolchain.cmake').as_posix()}",
         "-DANDROID_ABI=arm64-v8a", f"-DANDROID_PLATFORM=android-{MIN_SDK}", "-DANDROID_STL=c++_static",
         "-DCMAKE_BUILD_TYPE=Release", f"-DCRASH_SDL3_SOURCE={SDL_SOURCE.as_posix()}",
         f"-DCRASH_ADRENOTOOLS_SOURCE={adrenotools.as_posix() if adrenotools else ''}",
         f"-DCRASH_FFMPEG_ANDROID={FFMPEG_INSTALL.as_posix() if all(f.is_file() for f in FFMPEG_LIBS) else ''}", "-Wno-dev"],
        env=env, stdout=subprocess.DEVNULL)
    say("building libmain.so and libSDL3.so")
    run(["cmake", "--build", CMAKE_BUILD, "--target", "CrashRecomp", "CrashLauncher"], env=env)


def ffmpeg():
    """FFmpeg for the game's audio: libavcodec.so + libavutil.so for arm64, only the WMA Pro (XMA)
    and Vorbis (audio mods) decoders. Its configure needs a POSIX shell (Git Bash on Windows) and
    GNU make; the paths it writes are rewritten to C:/ form for a native Windows make."""
    if not (FFMPEG_SOURCE / "configure").is_file():
        sys.exit(f"{FFMPEG_SOURCE.relative_to(ROOT)} not found (see this script's docstring)")
    bash = shutil.which("bash") or sys.exit("ffmpeg: needs bash (Git Bash on Windows)")
    env = build_env()
    tmp = FFMPEG / "tmp"  # configure's test programs (the system temp folder may refuse to run them)
    tmp.mkdir(parents=True, exist_ok=True)
    env.update(TMPDIR=str(tmp), TMP=str(tmp), TEMP=str(tmp))
    extra = [FFMPEG / "bin", ROOT / "toolchain" / "llvm-mingw" / "bin"]  # make.exe; gcc as the host compiler
    env["PATH"] = os.pathsep.join([str(p) for p in extra if p.is_dir()] + [env["PATH"]])
    # configure checks a host compiler even with nothing to run on the host (CRASH_HOST_CC overrides).
    host_cc = os.environ.get("CRASH_HOST_CC") or shutil.which("gcc", path=env["PATH"]) or shutil.which("cc", path=env["PATH"])
    if not host_cc:
        sys.exit("ffmpeg: no host C compiler (the bundled toolchain's llvm-mingw, or set CRASH_HOST_CC)")
    FFMPEG_BUILD.mkdir(parents=True, exist_ok=True)
    bin_dir = NDK / "toolchains" / "llvm" / "prebuilt" / HOST / "bin"
    tool = lambda name: (bin_dir / (name + EXE)).as_posix()
    target = f"--target=aarch64-linux-android{MIN_SDK}"
    if not (FFMPEG_BUILD / "ffbuild" / "config.mak").is_file():
        say("configuring FFmpeg (arm64, WMA Pro + Vorbis decoders)")
        run([bash, (FFMPEG_SOURCE / "configure").as_posix(), f"--prefix={FFMPEG_INSTALL.as_posix()}", "--target-os=android",
             "--arch=aarch64", "--cpu=armv8-a", "--enable-cross-compile", f"--cc={tool('clang')}", f"--cxx={tool('clang++')}",
             f"--ar={tool('llvm-ar')}", f"--nm={tool('llvm-nm')}", f"--ranlib={tool('llvm-ranlib')}", f"--strip={tool('llvm-strip')}",
             f"--extra-cflags={target} -O2 -fPIC", f"--extra-ldflags={target} -Wl,-z,max-page-size=16384", f"--host-cc={Path(host_cc).as_posix()}",
             "--disable-everything", "--enable-decoder=wmapro,vorbis", "--disable-programs", "--disable-doc", "--disable-avdevice",
             "--disable-avformat", "--disable-avfilter", "--disable-swscale", "--disable-swresample", "--disable-network",
             "--disable-autodetect", "--enable-shared", "--disable-static", "--enable-pic"],
            cwd=FFMPEG_BUILD, env=env, stdout=subprocess.DEVNULL)
        if WINDOWS:
            # configure writes MSYS paths (/c/...), which a native Windows make can't open.
            for name in ("Makefile", "ffbuild/config.mak"):
                path = FFMPEG_BUILD / name
                text = path.read_text(encoding="utf-8")
                path.write_text(re.sub(r"(?<![\w/])/([a-zA-Z])/", lambda m: m.group(1).upper() + ":/", text), encoding="utf-8", newline="")
    say("building libavcodec.so and libavutil.so")
    make = shutil.which("make", path=env["PATH"]) or sys.exit("ffmpeg: GNU make not found (see this script's docstring)")
    run([make, f"-j{os.cpu_count() or 4}"], cwd=FFMPEG_BUILD, env=env, stdout=subprocess.DEVNULL)
    run([make, "install"], cwd=FFMPEG_BUILD, env=env, stdout=subprocess.DEVNULL)
    # `make install` may leave libavutil's headers out on Windows (a long command line): copy them.
    headers = FFMPEG_INSTALL / "include" / "libavutil"
    headers.mkdir(parents=True, exist_ok=True)
    for header in list((FFMPEG_SOURCE / "libavutil").glob("*.h")) + [FFMPEG_BUILD / "libavutil" / n for n in ("avconfig.h", "ffversion.h")]:
        if header.is_file() and not (headers / header.name).is_file():
            shutil.copy(header, headers / header.name)


def turnip():
    """Mesa's Vulkan driver for Adreno (freedreno/Turnip, kgsl kernel interface) as an Android HAL
    module, which libadrenotools loads in place of the system driver."""
    for path in (MESA_SOURCE / "meson.build", TURNIP / "pytools" / "mesonbuild"):
        if not path.exists():
            sys.exit(f"{path.relative_to(ROOT)} not found (see this script's docstring)")
    # Two fixes for building on Windows: the freedreno include check compares '/' paths, and
    # win_flex's --wincompat output needs <io.h>, which only Windows targets have.
    check = MESA_SOURCE / "src" / "freedreno" / "tests" / "check_xml_includes.awk"
    text = check.read_text(encoding="utf-8")
    if "gsub(" not in text:
        backslash = "\\"
        text = text.replace("    relpath = FILENAME\n", "    relpath = FILENAME\n    gsub(/" + backslash * 2 + "/, \"/\", relpath)\n")
        check.write_text(text, encoding="utf-8", newline="")
    meson_build = MESA_SOURCE / "meson.build"
    text = meson_build.read_text(encoding="utf-8")
    old = "    prog_flex = [prog_flex, '--wincompat']"
    if old in text:
        meson_build.write_text(text.replace(old, "    prog_flex = host_machine.system() == 'windows' ? [prog_flex, '--wincompat'] : [prog_flex]"),
                               encoding="utf-8", newline="")

    bin_dir = NDK / "toolchains" / "llvm" / "prebuilt" / HOST / "bin"
    cross = TURNIP / "android-aarch64.cross"
    cross.write_text(f"""[binaries]
c = ['{(bin_dir / ("clang" + EXE)).as_posix()}', '--target=aarch64-linux-android{MIN_SDK}']
cpp = ['{(bin_dir / ("clang++" + EXE)).as_posix()}', '--target=aarch64-linux-android{MIN_SDK}']
ar = '{(bin_dir / ("llvm-ar" + EXE)).as_posix()}'
strip = '{(bin_dir / ("llvm-strip" + EXE)).as_posix()}'
nm = '{(bin_dir / ("llvm-nm" + EXE)).as_posix()}'
c_ld = 'lld'
cpp_ld = 'lld'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'

[built-in options]
cpp_link_args = ['-static-libstdc++']
""", encoding="utf-8")
    env = build_env()
    env["PYTHONPATH"] = str(TURNIP / "pytools")
    if (TURNIP / "winflexbison").is_dir():
        env["PATH"] = os.pathsep.join([str(TURNIP / "winflexbison"), env["PATH"]])
    meson = [sys.executable, "-c", "import sys; from mesonbuild.mesonmain import main; sys.exit(main())"]
    command = [*meson, "setup", TURNIP_BUILD, MESA_SOURCE, "--cross-file", cross]
    llvm_mingw = ROOT / "toolchain" / "llvm-mingw" / "bin"
    if WINDOWS and llvm_mingw.is_dir():
        # The build machine's compiler (Mesa wants one even when nothing runs on the host).
        native = TURNIP / "native.ini"
        native.write_text(f"""[binaries]
c = '{(llvm_mingw / "clang.exe").as_posix()}'
cpp = '{(llvm_mingw / "clang++.exe").as_posix()}'
ar = '{(llvm_mingw / "llvm-ar.exe").as_posix()}'
""", encoding="utf-8")
        command += ["--native-file", native]
    if not (TURNIP_BUILD / "build.ninja").is_file():
        say("configuring Turnip (Mesa, kgsl, Android HAL)")
        run(command + ["-Dbuildtype=release", "-Dplatforms=android", f"-Dplatform-sdk-version={MIN_SDK}", "-Dandroid-stub=true",
                       "-Dandroid-libbacktrace=disabled", "-Dgallium-drivers=", "-Dvulkan-drivers=freedreno", "-Dfreedreno-kmds=kgsl",
                       "-Dllvm=disabled", "-Dshader-cache=enabled", "-Dzlib=enabled", "-Dzstd=disabled", "-Degl=disabled",
                       "-Dopengl=false", "-Dgles1=disabled", "-Dgles2=disabled", "-Dglx=disabled", "-Dgbm=disabled",
                       "-Dxmlconfig=disabled", "-Dexpat=disabled", "-Dvalgrind=disabled", "-Db_ndebug=true", "-Dstrip=true"],
            env=env, stdout=subprocess.DEVNULL)
    say("building libvulkan_freedreno.so")
    run(["ninja", "-C", TURNIP_BUILD, TURNIP_LIB.relative_to(TURNIP_BUILD).as_posix()], env=env, stdout=subprocess.DEVNULL)


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


def apk(debuggable=False):
    libmain = CMAKE_BUILD / "libmain.so"
    liblauncher = CMAKE_BUILD / "libcrashlauncher.so"
    libsdl = next(iter(sorted(CMAKE_BUILD.rglob("libSDL3.so"))), None)
    if not libmain.is_file() or not liblauncher.is_file() or not libsdl:
        sys.exit("libmain.so / libcrashlauncher.so / libSDL3.so not built: run `python tools/android_build.py build` first")
    if OUT.exists():
        shutil.rmtree(OUT)
    classes = OUT / "classes"
    classes.mkdir(parents=True)

    # Java: SDL's glue + LauncherActivity + CrashActivity -> classes.dex
    sources = sorted((SDL_SOURCE / "android-project/app/src/main/java").rglob("*.java")) + \
        sorted((ROOT / "android" / "java").rglob("*.java"))
    say(f"javac: {len(sources)} files")
    (OUT / "sources.txt").write_text("\n".join(f'"{s.as_posix()}"' for s in sources), encoding="utf-8")
    run([jdk_tool("javac"), "--release", "11", "-nowarn", "-Xlint:none", "-classpath", ANDROID_JAR, "-d", classes,
         f"@{OUT / 'sources.txt'}"], stdout=subprocess.DEVNULL)
    class_files = [str(p) for p in classes.rglob("*.class")]
    run([jdk_tool("java"), "-cp", BUILD_TOOLS / "lib" / "d8.jar", "com.android.tools.r8.D8", "--release", "--min-api", MIN_SDK,
         "--lib", ANDROID_JAR, "--output", OUT, *class_files])

    # Resources: android/res (the launcher icon) plus launcher/crashrecomp.png, the project's icon, as
    # @drawable/crashrecomp_icon. Manifest + resources -> base APK.
    res = OUT / "res"
    shutil.copytree(ROOT / "android" / "res", res)
    (res / "drawable-nodpi").mkdir(exist_ok=True)
    shutil.copy(ROOT / "launcher" / "crashrecomp.png", res / "drawable-nodpi" / "crashrecomp_icon.png")
    say("aapt2 compile + link")
    run([BUILD_TOOLS / ("aapt2" + EXE), "compile", "--dir", res, "-o", OUT / "res.zip"])
    run([BUILD_TOOLS / ("aapt2" + EXE), "link", "-o", OUT / "base.apk", "-I", ANDROID_JAR, "--manifest", ROOT / "android" / "AndroidManifest.xml",
         "--min-sdk-version", MIN_SDK, "--target-sdk-version", TARGET_SDK, OUT / "res.zip"] + (["--debug-mode"] if debuggable else []))
    if debuggable:
        say("debuggable (simpleperf / run-as can reach the app)")

    # classes.dex + the .so files (libmain.so without its debug sections). extractNativeLibs="true":
    # Android unpacks them into the app's native library folder, where libadrenotools loads its
    # hooks and Turnip from (android_main_android.cpp); so they can be compressed.
    strip = NDK / "toolchains" / "llvm" / "prebuilt" / HOST / "bin" / ("llvm-strip" + EXE)
    stripped = OUT / "libmain.so"
    run([strip, "--strip-debug", "-o", stripped, libmain])
    launcher_stripped = OUT / "libcrashlauncher.so"
    run([strip, "--strip-debug", "-o", launcher_stripped, liblauncher])
    unaligned = OUT / "unaligned.apk"
    shutil.copy(OUT / "base.apk", unaligned)
    with zipfile.ZipFile(unaligned, "a") as z:
        z.write(OUT / "classes.dex", "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
        z.write(libsdl, "lib/arm64-v8a/libSDL3.so", compress_type=zipfile.ZIP_DEFLATED)
        z.write(stripped, "lib/arm64-v8a/libmain.so", compress_type=zipfile.ZIP_DEFLATED)
        z.write(launcher_stripped, "lib/arm64-v8a/libcrashlauncher.so", compress_type=zipfile.ZIP_DEFLATED)
        hooks = [CMAKE_BUILD / "adrenotools" / "src" / "hook" / name for name in ("libhook_impl.so", "libmain_hook.so")]
        if all(h.is_file() for h in hooks) and TURNIP_LIB.is_file():
            for library in hooks + [TURNIP_LIB]:
                z.write(library, f"lib/arm64-v8a/{library.name}", compress_type=zipfile.ZIP_DEFLATED)
            say("with Turnip (libvulkan_freedreno.so) and libadrenotools' hooks")
        if all(f.is_file() for f in FFMPEG_LIBS):
            for library in FFMPEG_LIBS:
                z.write(library, f"lib/arm64-v8a/{library.name}", compress_type=zipfile.ZIP_DEFLATED)
            say("with FFmpeg (libavcodec.so, libavutil.so): audio")
        if DXC_LIB.is_file():
            # The shaders compiled on the phone (rhi_vulkan_shaders.cpp: dlopen by name, found in
            # the app's library folder); 446 MB with its debug information, 26 MB without.
            dxc_stripped = OUT / "libdxcompiler.so"
            run([strip, "--strip-all", "-o", dxc_stripped, DXC_LIB])
            z.write(dxc_stripped, "lib/arm64-v8a/libdxcompiler.so", compress_type=zipfile.ZIP_DEFLATED)
            say("with DXC (libdxcompiler.so): shaders compiled on the phone")

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


def llvm():
    """clang and lld as Android arm64 programs (the phone compiles and links the recompiled game,
    docs/internals/android_port.md, single APK). LLVM's build runs its table generators while it
    builds, so they are built first for this machine, with the bundled llvm-mingw on Windows."""
    if not (LLVM_SOURCE / "llvm" / "CMakeLists.txt").is_file():
        sys.exit(f"{LLVM_SOURCE.relative_to(ROOT)} not found (see this script's docstring)")
    env = build_env()
    lean = ["-DCMAKE_BUILD_TYPE=Release", "-DLLVM_TARGETS_TO_BUILD=AArch64", "-DLLVM_INCLUDE_TESTS=OFF",
            "-DLLVM_INCLUDE_EXAMPLES=OFF", "-DLLVM_INCLUDE_BENCHMARKS=OFF", "-DLLVM_INCLUDE_DOCS=OFF", "-DLLVM_ENABLE_ZLIB=OFF",
            "-DLLVM_ENABLE_ZSTD=OFF", "-DLLVM_ENABLE_LIBXML2=OFF", "-DLLVM_ENABLE_LIBEDIT=OFF", "-DLLVM_ENABLE_LIBPFM=OFF",
            "-DLLVM_ENABLE_BINDINGS=OFF", "-DLLVM_ENABLE_PLUGINS=OFF", "-DCLANG_ENABLE_ARCMT=OFF",
            "-DCLANG_ENABLE_STATIC_ANALYZER=OFF", "-DCLANG_PLUGIN_SUPPORT=OFF", "-DLLVM_ENABLE_ASSERTIONS=OFF", "-Wno-dev"]
    tablegens = ["llvm-tblgen", "clang-tblgen", "llvm-min-tblgen"]
    host = host_compilers(env)
    if not (LLVM_HOST_BUILD / "build.ninja").is_file():
        say("configuring LLVM's table generators (this machine)")
        run(["cmake", "-S", LLVM_SOURCE / "llvm", "-B", LLVM_HOST_BUILD, "-G", "Ninja", "-DLLVM_ENABLE_PROJECTS=clang", *host, *lean],
            env=env, stdout=subprocess.DEVNULL)
    say("building " + ", ".join(tablegens))
    gentle = background_build(env)
    run(["cmake", "--build", LLVM_HOST_BUILD, "--target", *tablegens], **gentle)
    native = LLVM_HOST_BUILD / "bin"
    if not (LLVM_BUILD / "build.ninja").is_file():
        say("configuring clang + lld (Android arm64)")
        run(["cmake", "-S", LLVM_SOURCE / "llvm", "-B", LLVM_BUILD, "-G", "Ninja",
             f"-DCMAKE_TOOLCHAIN_FILE={(NDK / 'build/cmake/android.toolchain.cmake').as_posix()}",
             "-DANDROID_ABI=arm64-v8a", f"-DANDROID_PLATFORM=android-{MIN_SDK}", "-DANDROID_STL=c++_static",
             "-DLLVM_ENABLE_PROJECTS=clang;lld", f"-DLLVM_HOST_TRIPLE=aarch64-linux-android{MIN_SDK}",
             f"-DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-linux-android{MIN_SDK}", f"-DLLVM_NATIVE_TOOL_DIR={native.as_posix()}",
             f"-DLLVM_TABLEGEN={(native / ('llvm-tblgen' + EXE)).as_posix()}", f"-DCLANG_TABLEGEN={(native / ('clang-tblgen' + EXE)).as_posix()}",
             "-DLLVM_BUILD_TOOLS=OFF", "-DLLVM_BUILD_UTILS=OFF", "-DLLVM_ENABLE_TERMINFO=OFF", "-DLLVM_ENABLE_PIC=ON",
             "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-z,max-page-size=16384", *lean],
            env=env, stdout=subprocess.DEVNULL)
    say("building clang and lld (Android arm64)")
    run(["cmake", "--build", LLVM_BUILD, "--target", "clang", "lld"], **gentle)
    for tool in LLVM_TOOLS:
        say(f"{tool.relative_to(ROOT)}: {tool.stat().st_size / 1e6:.0f} MB")


def background_build(env):
    """LLVM and DXC are long builds: a few jobs (CRASH_BUILD_JOBS, a quarter of the cores by default)
    at a low priority, so the PC stays usable while they run. The `run` keywords for them."""
    env = dict(env)
    env["CMAKE_BUILD_PARALLEL_LEVEL"] = os.environ.get("CRASH_BUILD_JOBS") or str(max(2, (os.cpu_count() or 8) // 4))
    say(f"{env['CMAKE_BUILD_PARALLEL_LEVEL']} jobs at a low priority (CRASH_BUILD_JOBS to change)")
    if WINDOWS:
        return {"env": env, "creationflags": subprocess.IDLE_PRIORITY_CLASS}
    return {"env": env, "preexec_fn": lambda: os.nice(15)}


def host_compilers(env):
    """This machine's C / C++ compilers for CMake: the bundled llvm-mingw, else a clang / gcc on PATH."""
    search = os.pathsep.join([str(ROOT / "toolchain" / "llvm-mingw" / "bin"), env["PATH"]])
    cc, cxx = shutil.which("clang", path=search), shutil.which("clang++", path=search)
    if not (cc and cxx):
        cc, cxx = shutil.which("gcc", path=search), shutil.which("g++", path=search)
    if not (cc and cxx):
        sys.exit("no compiler for this machine (the bundled llvm-mingw, or clang / gcc on PATH)")
    return [f"-DCMAKE_C_COMPILER={Path(cc).as_posix()}", f"-DCMAKE_CXX_COMPILER={Path(cxx).as_posix()}"]


def msvc_env(env):
    """`env` inside Visual Studio's vcvars64 (vswhere), or None without Visual Studio."""
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.is_file():
        return None
    found = subprocess.run([vswhere, "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                            "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
    vcvars = Path(found) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat" if found else None
    if not vcvars or not vcvars.is_file():
        return None
    out = subprocess.run(f'cmd /s /c ""{vcvars}" >nul && set"', capture_output=True, text=True, env=env, shell=True).stdout
    merged = dict(env)
    for line in out.splitlines():
        name, sep, value = line.partition("=")
        if sep and name:
            merged[name] = value
    # The bundled CMake / Ninja first, as in build_env.
    merged["PATH"] = os.pathsep.join([p for p in env["PATH"].split(os.pathsep) if "toolchain" in p.lower()] + [merged.get("PATH", merged.get("Path", ""))])
    return merged


def dxc():
    """DXC as an Android arm64 library with SPIR-V output, for the shaders the renderer compiles on
    the phone (no shader cache made on a PC). DXC is an LLVM 3.7 fork: its table generators come
    first, built for this machine."""
    if not (DXC_SOURCE / "CMakeLists.txt").is_file() or not (DXC_SOURCE / "external" / "SPIRV-Tools" / "CMakeLists.txt").is_file():
        sys.exit(f"{DXC_SOURCE.relative_to(ROOT)} not found or without its submodules (see this script's docstring)")
    env = build_env()
    cache = ["-C", DXC_SOURCE / "cmake" / "caches" / "PredefinedParams.cmake"]
    lean = ["-DCMAKE_BUILD_TYPE=Release", "-DHLSL_INCLUDE_TESTS=OFF", "-DSPIRV_BUILD_TESTS=OFF", "-DLLVM_INCLUDE_TESTS=OFF",
            "-DCLANG_INCLUDE_TESTS=OFF", "-DLLVM_APPEND_VC_REV=OFF", "-DHLSL_ENABLE_ANALYZE=OFF", "-DENABLE_SPIRV_CODEGEN=ON",
            "-DSPIRV_SKIP_EXECUTABLES=ON", "-DSPIRV_SKIP_TESTS=ON", "-DLLVM_ENABLE_ASSERTIONS=OFF", "-Wno-dev"]
    # On Windows DXC's own code expects MSVC (MinGW lacks what its Windows paths use).
    host_env, host = env, None
    if WINDOWS:
        host_env = msvc_env(env) or sys.exit("dxc: needs Visual Studio's C++ tools on Windows (for its table generators)")
        # No include tracking for rc.exe: CMake's wrapper passes it cl's "Note: including file:"
        # prefix, which a translated MSVC ("Observação: ...") turns into a broken command line.
        host = ["-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl", "-DCMAKE_NINJA_CMCLDEPS_RC=OFF"]
    if not (DXC_HOST_BUILD / "build.ninja").is_file():
        say("configuring DXC's table generators (this machine)")
        run(["cmake", *cache, "-S", DXC_SOURCE, "-B", DXC_HOST_BUILD, "-G", "Ninja", *(host or host_compilers(env)), *lean],
            env=host_env, stdout=subprocess.DEVNULL)
    say("building DXC's llvm-tblgen and clang-tblgen")
    run(["cmake", "--build", DXC_HOST_BUILD, "--target", "llvm-tblgen", "clang-tblgen"], **background_build(host_env))
    gentle = background_build(env)
    native = DXC_HOST_BUILD / "bin"
    if not (DXC_BUILD / "build.ninja").is_file():
        say("configuring libdxcompiler.so (Android arm64)")
        run(["cmake", *cache, "-S", DXC_SOURCE, "-B", DXC_BUILD, "-G", "Ninja",
             f"-DCMAKE_TOOLCHAIN_FILE={(NDK / 'build/cmake/android.toolchain.cmake').as_posix()}",
             "-DANDROID_ABI=arm64-v8a", f"-DANDROID_PLATFORM=android-{MIN_SDK}", "-DANDROID_STL=c++_static",
             f"-DLLVM_TABLEGEN={(native / ('llvm-tblgen' + EXE)).as_posix()}", f"-DCLANG_TABLEGEN={(native / ('clang-tblgen' + EXE)).as_posix()}",
             f"-DLLVM_HOST_TRIPLE=aarch64-linux-android{MIN_SDK}", "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384", *lean],
            env=env, stdout=subprocess.DEVNULL)
    say("building libdxcompiler.so (Android arm64)")
    run(["cmake", "--build", DXC_BUILD, "--target", "dxcompiler"], **gentle)
    say(f"{DXC_LIB.relative_to(ROOT)}: {DXC_LIB.stat().st_size / 1e6:.0f} MB")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("step", nargs="?", default="all", choices=["all", "build", "apk", "turnip", "ffmpeg", "llvm", "dxc"])
    parser.add_argument("--debuggable", action="store_true",
                        help="android:debuggable APK, for profiling on the phone (simpleperf record --app <package>)")
    args = parser.parse_args()
    step = args.step
    if step == "turnip" or (step == "all" and MESA_SOURCE.is_dir() and not TURNIP_LIB.is_file()):
        turnip()
    if step == "ffmpeg" or (step == "all" and FFMPEG_SOURCE.is_dir() and not all(f.is_file() for f in FFMPEG_LIBS)):
        ffmpeg()
    if step == "llvm":
        llvm()
    if step == "dxc":
        dxc()
    if step in ("all", "build"):
        build()
    if step in ("all", "apk"):
        apk(args.debuggable)


if __name__ == "__main__":
    main()
