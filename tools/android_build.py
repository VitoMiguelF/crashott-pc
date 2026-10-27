"""Builds the Android app (docs/internals/android_port.md), the single APK: the launcher, libSDL3.so
and what builds the game on the phone, SDL3's Java glue, assembled with aapt2 / d8 / zipalign /
apksigner (no Gradle, no Android Studio).

    python tools/android_build.py               # configure if needed, build, toolchain, assemble the APK
    python tools/android_build.py build         # the launcher, libSDL3.so and the runtime (CMake + Ninja)
    python tools/android_build.py apk           # only the APK, from what build and toolchain left
    python tools/android_build.py turnip        # Mesa's Turnip (Adreno 6xx/7xx with an old driver)
    python tools/android_build.py ffmpeg        # FFmpeg's WMA Pro + Vorbis decoders (the game's audio)
    python tools/android_build.py llvm          # clang + lld that run on the phone (the single APK)
    python tools/android_build.py dxc           # libdxcompiler.so: the shaders compiled on the phone
    python tools/android_build.py xenonrecomp   # XenonRecomp for arm64 (the phone recompiles the dump)
    python tools/android_build.py toolchain     # what compiles the game on the phone, in one folder
    python tools/android_build.py push -s <serial>   # test builds: this machine's libmain.so (`build`)
                                                # into the app on the phone (an APK built --debuggable)

The APK holds no game code: on the phone the launcher asks for the player's dump, copies it into the
app and builds the game from it (XenonRecomp, clang, lld: GameInstaller.java), as CrashSetup does on
a PC. The `build` step still compiles the runtime on this machine against a local generated/ppc
(its headers ppc_config.h / ppc_context.h): the runtime objects go into the APK as a static library,
the generated code doesn't.

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
import hashlib
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
XENON_SOURCE = ROOT / "tools" / "XenonRecomp"
XENON_BUILD = WORK / "xenonrecomp"
XENON_EXE = XENON_BUILD / "XenonRecomp" / "XenonRecomp"
ONDEVICE_ROOT = WORK / "ondevice"           # the `toolchain` step: what the phone builds the game with
ONDEVICE = ONDEVICE_ROOT / "toolchain"      # bin/ (for adb tests), sysroot/, lib/clang/<v>/
ONDEVICE_ZIP = WORK / "ondevice.zip"         # all of it but bin/: the APK's assets/ondevice.zip
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
    # A poll of a GPU timestamp (no time left) reads the retired timestamp: some kgsl kernels (the
    # S20 FE's) block on a zero-timeout IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID until it retires, so
    # every vkGetSemaphoreCounterValue of the emulated timeline waited for the GPU to finish the
    # frame and the CPU never recorded the next one while the GPU drew (docs/internals/android_port.md).
    kgsl = MESA_SOURCE / "src" / "freedreno" / "vulkan" / "tu_knl_kgsl.cc"
    text = kgsl.read_text(encoding="utf-8")
    old = "      .timeout = get_relative_ms(abs_timeout_ns),\n   };\n\n   while (true) {\n"
    if "KGSL_TIMESTAMP_RETIRED, " not in text:
        if old not in text:
            sys.exit(f"{kgsl.relative_to(ROOT)}: wait_timestamp_safe not as expected (another Mesa version?)")
        kgsl.write_text(text.replace(old, """      .timeout = get_relative_ms(abs_timeout_ns),
   };

   if (wait.timeout == 0) {
      struct kgsl_cmdstream_readtimestamp_ctxtid read = { context_id, KGSL_TIMESTAMP_RETIRED, 0 };
      if (safe_ioctl(fd, IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID, &read) == 0)
         return timestamp_cmp(read.timestamp, timestamp) ? VK_SUCCESS : VK_TIMEOUT;
   }

   while (true) {
"""), encoding="utf-8", newline="")

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
    """The single APK: no game code. The launcher, SDL3 and the libraries the game loads, plus what
    builds the game on the phone from the player's dump (the `toolchain` step's ondevice.zip in
    assets/, clang / lld / XenonRecomp as lib*.so programs); GameInstaller.java makes libmain.so."""
    liblauncher = CMAKE_BUILD / "libcrashlauncher.so"
    libsdl = next(iter(sorted(CMAKE_BUILD.rglob("libSDL3.so"))), None)
    if not liblauncher.is_file() or not libsdl:
        sys.exit("libcrashlauncher.so / libSDL3.so not built: run `python tools/android_build.py build` first")
    programs = {"libcrashclang.so": ONDEVICE / "bin" / "clang", "libcrashlld.so": ONDEVICE / "bin" / "ld.lld",
                "libxenonrecomp.so": ONDEVICE / "bin" / "XenonRecomp"}
    if not ONDEVICE_ZIP.is_file() or not all(f.is_file() for f in programs.values()):
        sys.exit("the on-device toolchain is not made: run `python tools/android_build.py toolchain` first")
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

    # classes.dex + the .so files. extractNativeLibs="true": Android unpacks them into the app's
    # native library folder, where libadrenotools loads its hooks and Turnip from
    # (android_main_android.cpp) and the only place the app may run programs from; so they can be
    # compressed.
    strip = NDK / "toolchains" / "llvm" / "prebuilt" / HOST / "bin" / ("llvm-strip" + EXE)
    launcher_stripped = OUT / "libcrashlauncher.so"
    run([strip, "--strip-debug", "-o", launcher_stripped, liblauncher])
    unaligned = OUT / "unaligned.apk"
    shutil.copy(OUT / "base.apk", unaligned)
    with zipfile.ZipFile(unaligned, "a") as z:
        z.write(OUT / "classes.dex", "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
        z.write(libsdl, "lib/arm64-v8a/libSDL3.so", compress_type=zipfile.ZIP_DEFLATED)
        for name, program in programs.items():
            z.write(program, f"lib/arm64-v8a/{name}", compress_type=zipfile.ZIP_DEFLATED)
        # Stored: already compressed, and read as a stream.
        z.write(ONDEVICE_ZIP, "assets/ondevice.zip", compress_type=zipfile.ZIP_STORED)
        with zipfile.ZipFile(ONDEVICE_ZIP) as bundle:
            z.writestr("assets/ondevice.stamp", bundle.read("ondevice.stamp"), compress_type=zipfile.ZIP_STORED)
        say("with the on-device toolchain (clang, lld, XenonRecomp, runtime): the game is built on the phone")
        # The language packs, copied into the files folder by LauncherActivity (the PC's
        # make_dist.py puts them next to the exe); the subtitles of a pack come with the
        # downloaded subtitles (Subtitles.java), not from here.
        for f in sorted((ROOT / "languages").rglob("*")):
            if f.is_file() and f.suffix != ".md" and f.name != "subtitles.txt":
                z.write(f, "assets/" + f.relative_to(ROOT).as_posix(), compress_type=zipfile.ZIP_DEFLATED)
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
    final = OUT / "CrashOTTDE.apk"
    password = keystore()
    run([jdk_tool("java"), "-jar", BUILD_TOOLS / "lib" / "apksigner.jar", "sign", "--ks", KEYSTORE, "--ks-pass", f"pass:{password}",
         "--min-sdk-version", MIN_SDK, "--out", final, aligned])
    run([jdk_tool("java"), "-jar", BUILD_TOOLS / "lib" / "apksigner.jar", "verify", "--min-sdk-version", MIN_SDK, final])
    size = final.stat().st_size
    say(f"{final.relative_to(ROOT)}: {size / 2**20:.1f} MB (no game code: the phone builds it from the player's dump)")


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


def xenonrecomp():
    """XenonRecomp (tools/XenonRecomp, the pinned source with patches/XenonRecomp.patch, as
    tools/setup.py makes it) as an Android arm64 program: the phone turns the player's default.xex
    into generated/ppc itself."""
    if not (XENON_SOURCE / "XenonRecomp" / "main.cpp").is_file():
        sys.exit(f"{XENON_SOURCE.relative_to(ROOT)} not found: tools/setup.py fetches and patches it")
    env = build_env()
    if not (XENON_BUILD / "build.ninja").is_file():
        say("configuring XenonRecomp (Android arm64)")
        run(["cmake", "-S", XENON_SOURCE, "-B", XENON_BUILD, "-G", "Ninja",
             f"-DCMAKE_TOOLCHAIN_FILE={(NDK / 'build/cmake/android.toolchain.cmake').as_posix()}",
             "-DANDROID_ABI=arm64-v8a", f"-DANDROID_PLATFORM=android-{MIN_SDK}", "-DANDROID_STL=c++_static",
             # The pinned fmt uses malloc without <cstdlib>, which the NDK's libc++ no longer brings in.
             "-DCMAKE_CXX_FLAGS=-include cstdlib",
             "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-z,max-page-size=16384", "-Wno-dev"],
            env=env, stdout=subprocess.DEVNULL)
    say("building XenonRecomp (Android arm64)")
    run(["cmake", "--build", XENON_BUILD, "--target", "XenonRecomp"], **background_build(env))


def toolchain():
    """What the phone builds the game with, in build/android/ondevice/: toolchain/bin/ (clang,
    ld.lld, XenonRecomp: the `llvm` and `xenonrecomp` steps', stripped; in the APK as lib*.so
    programs), toolchain/sysroot/ (the NDK's headers, API-level stubs and C++ runtime, arm64 only),
    toolchain/lib/clang/<version>/ (clang's headers, the NDK's compiler-rt builtins and libunwind),
    runtime/ (libcrashruntime.a and the static libraries libmain.so links), xenon/ (config/Crash.toml
    and its switch tables, XenonRecomp's ppc_context.h) and include/simde. All but bin/ is zipped
    into ondevice.zip for the APK's assets. No game file goes in: the phone makes generated/ppc
    from the player's own default.xex."""
    for tool, step in [(LLVM_TOOLS[0], "llvm"), (LLVM_TOOLS[1], "llvm"), (XENON_EXE, "xenonrecomp")]:
        if not tool.is_file():
            sys.exit(f"{tool.relative_to(ROOT)} not built: run `python tools/android_build.py {step}` first")
    prebuilt = NDK / "toolchains" / "llvm" / "prebuilt" / HOST
    ndk_sysroot = prebuilt / "sysroot"
    ndk_resource = next(iter(sorted((prebuilt / "lib" / "clang").iterdir())))
    resource = next(iter(sorted((LLVM_BUILD / "lib" / "clang").iterdir())))
    if ONDEVICE_ROOT.exists():
        shutil.rmtree(ONDEVICE_ROOT)
    strip = prebuilt / "bin" / ("llvm-strip" + EXE)
    (ONDEVICE / "bin").mkdir(parents=True)
    run([strip, "--strip-all", "-o", ONDEVICE / "bin" / "clang", LLVM_TOOLS[0]])
    run([strip, "--strip-all", "-o", ONDEVICE / "bin" / "ld.lld", LLVM_TOOLS[1]])
    triple = "aarch64-linux-android"
    # Headers: everything but the other architectures' asm/ folders.
    others = {"arm-linux-androideabi", "i686-linux-android", "x86_64-linux-android", "riscv64-linux-android"}
    shutil.copytree(ndk_sysroot / "usr" / "include", ONDEVICE / "sysroot" / "usr" / "include",
                    ignore=lambda folder, names: [n for n in names if n in others])
    libs = ONDEVICE / "sysroot" / "usr" / "lib" / triple
    shutil.copytree(ndk_sysroot / "usr" / "lib" / triple / str(MIN_SDK), libs / str(MIN_SDK))
    for name in ("libc++_static.a", "libc++abi.a"):
        shutil.copy(ndk_sysroot / "usr" / "lib" / triple / name, libs / name)
    target = ONDEVICE / "lib" / "clang" / resource.name
    shutil.copytree(resource / "include", target / "include")
    (target / "lib" / "linux" / "aarch64").mkdir(parents=True)
    shutil.copy(ndk_resource / "lib" / "linux" / "libclang_rt.builtins-aarch64-android.a", target / "lib" / "linux")
    for name in ("libunwind.a", "libatomic.a"):
        shutil.copy(ndk_resource / "lib" / "linux" / "aarch64" / name, target / "lib" / "linux" / "aarch64" / name)
    run([strip, "--strip-all", "-o", ONDEVICE / "bin" / "XenonRecomp", XENON_EXE])

    # The runtime (runtime/, built by the `build` step: the same objects as libmain.so's, so the
    # game links the same on the phone) and the static libraries it needs, without debug info.
    objects = sorted((CMAKE_BUILD / "CMakeFiles" / "CrashRecomp.dir").rglob("*.o"))
    if not objects:
        sys.exit("the runtime is not built: run `python tools/android_build.py build` first")
    runtime = ONDEVICE_ROOT / "runtime"
    runtime.mkdir(parents=True)
    run([prebuilt / "bin" / ("llvm-ar" + EXE), "rcs", runtime / "libcrashruntime.a", *objects])
    for library in [runtime / "libcrashruntime.a", CMAKE_BUILD / "libXexImage.a", CMAKE_BUILD / "adrenotools" / "libadrenotools.a",
                    CMAKE_BUILD / "adrenotools" / "lib" / "linkernsbypass" / "liblinkernsbypass.a"]:
        if library.is_file():
            run([strip, "--strip-debug", "-o", runtime / library.name, library])
    # What XenonRecomp reads (the project's config, XenonRecomp's context header) and what the
    # recompiled code includes (simde).
    xenon = ONDEVICE_ROOT / "xenon"
    xenon.mkdir()
    for path in (ROOT / "config" / "Crash.toml", ROOT / "config" / "Crash_switch_tables.toml", XENON_SOURCE / "XenonUtils" / "ppc_context.h"):
        shutil.copy(path, xenon / path.name)
    shutil.copytree(XENON_SOURCE / "thirdparty" / "simde", ONDEVICE_ROOT / "include" / "simde",
                    ignore=shutil.ignore_patterns(".git*", "test", "*.md", "COPYING"))

    # One zip for the APK (bin/ goes in as lib*.so programs), named by its content: the phone
    # unpacks it again when it changes.
    entries = sorted(f for f in ONDEVICE_ROOT.rglob("*") if f.is_file() and ONDEVICE / "bin" not in f.parents)
    digest = hashlib.sha256()
    for f in entries:
        digest.update(f.relative_to(ONDEVICE_ROOT).as_posix().encode())
        digest.update(f.read_bytes())
    stamp = digest.hexdigest()[:16]
    with zipfile.ZipFile(ONDEVICE_ZIP, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for f in entries:
            z.write(f, f.relative_to(ONDEVICE_ROOT).as_posix())
        z.writestr("ondevice.stamp", stamp)
    size = sum(f.stat().st_size for f in entries)
    say(f"{ONDEVICE_ZIP.relative_to(ROOT)}: {ONDEVICE_ZIP.stat().st_size / 1e6:.0f} MB ({size / 1e6:.0f} MB unpacked, stamp {stamp})")


def push(serial):
    """Test builds without the phone's recompile: the libmain.so the `build` step linked here (the
    same runtime and generated code as the phone's own link) replaces the app's, through run-as (so
    the installed APK must be built --debuggable). libmain.stamp stays, so the launcher keeps it;
    a new APK with another ondevice.stamp makes the phone build its own again."""
    library = CMAKE_BUILD / "libmain.so"
    if not library.is_file():
        sys.exit("build/android/cmake/libmain.so not built: run `python tools/android_build.py build` first")
    adb = shutil.which("adb") or sys.exit("push: adb not found on PATH")
    device = [adb] + (["-s", serial] if serial else [])
    stripped = CMAKE_BUILD / "libmain.push.so"
    run([NDK / "toolchains" / "llvm" / "prebuilt" / HOST / "bin" / ("llvm-strip" + EXE), "--strip-debug", "-o", stripped, library])
    package = "io.github.vitomiguelf.crashottde"
    run(device + ["shell", "am", "force-stop", package])
    run(device + ["push", stripped, "/data/local/tmp/libmain.so"], stdout=subprocess.DEVNULL)
    run(device + ["shell", f"chmod 644 /data/local/tmp/libmain.so && run-as {package} cp /data/local/tmp/libmain.so files/libmain.so"
                  f" && run-as {package} chmod 700 files/libmain.so; rm -f /data/local/tmp/libmain.so"])
    say(f"libmain.so ({stripped.stat().st_size / 1e6:.0f} MB) in the app on {serial or 'the phone'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("step", nargs="?", default="all", choices=["all", "build", "apk", "turnip", "ffmpeg", "llvm", "dxc", "xenonrecomp", "toolchain", "push"])
    parser.add_argument("--debuggable", action="store_true",
                        help="android:debuggable APK, for profiling on the phone (simpleperf record --app <package>)")
    parser.add_argument("-s", "--serial", help="push: the phone (adb -s)")
    args = parser.parse_args()
    step = args.step
    if step == "push":
        push(args.serial)
        return
    if step == "turnip" or (step == "all" and MESA_SOURCE.is_dir() and not TURNIP_LIB.is_file()):
        turnip()
    if step == "ffmpeg" or (step == "all" and FFMPEG_SOURCE.is_dir() and not all(f.is_file() for f in FFMPEG_LIBS)):
        ffmpeg()
    if step == "llvm":
        llvm()
    if step == "dxc":
        dxc()
    if step == "xenonrecomp":
        xenonrecomp()
    if step == "toolchain":
        toolchain()
    if step in ("all", "build"):
        build()
    if step == "all":
        toolchain()
    if step in ("all", "apk"):
        apk(args.debuggable)


if __name__ == "__main__":
    main()
