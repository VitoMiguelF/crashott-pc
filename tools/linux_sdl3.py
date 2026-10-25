"""Builds the SDL3 the Linux release ships next to the game and the installer (thirdparty/sdl3): run once on
the release machine (needs clang, lld, CMake and Ninja there), never on the player's.

    python3 tools/linux_sdl3.py

Why our own: the Steam Runtime's libSDL3.so.0 (what setup.py fetches to build against) links X11,
Wayland, PulseAudio, libdecor... directly, and a SteamOS desktop session started outside Steam may lack
one of them, so the game or the installer wouldn't start. SDL's own default loads every backend with
dlopen (SDL_DEPS_SHARED): this build needs only glibc, so it runs anywhere and uses what is there
(X11 or Wayland, PipeWire or PulseAudio, udev, D-Bus).

It is built against the Steam Runtime 3 ("sniper") SDK sysroot (Debian 11, glibc 2.31: older than every
SteamOS 3), the one Valve publishes for building Linux games, with the headers of every backend:
  - the official SDL 3.4.18 source (the version Steam Runtime 3 has), pinned by SHA-256;
  - the sysroot snapshot, pinned by SHA-256 (1.2 GB, kept in build/downloads/steamrt); only its amd64
    headers and libraries are unpacked (build/steamrt/sysroot), absolute symlinks made relative.
The result goes to thirdparty/sdl3 (include/, lib/libSDL3.so.0, LICENSE.txt, .portable): setup.py then
skips the Steam Runtime .debs, CMake links against it and copies it next to the exe, and
tools/make_appimage.py carries it.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOWNLOADS = ROOT / "build" / "downloads"
WORK = ROOT / "build" / "steamrt"
OUT = ROOT / "thirdparty" / "sdl3"

SDL_VERSION = "3.4.18"
SDL = (f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-{SDL_VERSION}.tar.gz",
       "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3")
SNAPSHOT = "3.0.20261021.266025"
SYSROOT = (f"https://repo.steampowered.com/steamrt-images-sniper/snapshots/{SNAPSHOT}/"
           "com.valvesoftware.SteamRuntime.Sdk-amd64%2Ci386-sniper-sysroot.tar.gz",
           "8f71947035f97925a311b2ed753f22efcf2f13d6f1631945b8e537f8903ce2cf")
# What a C build for x86-64 needs of the sysroot.
SYSROOT_PARTS = ("usr/include/", "usr/lib/x86_64-linux-gnu/", "lib/x86_64-linux-gnu/", "usr/lib/gcc/x86_64-linux-gnu/",
                 "lib64/", "usr/share/pkgconfig/", "usr/lib/pkgconfig/", "usr/bin/wayland-scanner", "usr/share/wayland/",
                 "usr/share/wayland-protocols/")
# The libraries the built libSDL3.so.0 may need (anything else means a backend got linked).
ALLOWED_NEEDED = {"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1", "ld-linux-x86-64.so.2"}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while block := f.read(1 << 22):
            h.update(block)
    return h.hexdigest()


def download(url, digest, dest):
    if dest.is_file() and sha256(dest) == digest:
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"baixando {url}", flush=True)
    part = dest.with_name(dest.name + ".part")
    request = urllib.request.Request(url, headers={"User-Agent": "CrashRecomp-sdl3"})
    with urllib.request.urlopen(request, timeout=300) as response, open(part, "wb") as out:
        shutil.copyfileobj(response, out, 1 << 20)
    if sha256(part) != digest:
        part.unlink()
        raise SystemExit(f"{dest.name}: o arquivo baixado não confere (SHA-256)")
    part.replace(dest)
    return dest


def unpack_sysroot(archive, dest):
    stamp = dest / ".snapshot"
    if stamp.is_file() and stamp.read_text().strip() == SNAPSHOT:
        return
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    print(f"extraindo o sysroot em {dest}", flush=True)
    with tarfile.open(archive) as t:
        for member in t:
            name = member.name[2:] if member.name.startswith("./") else member.name
            if not name.startswith(SYSROOT_PARTS) or member.isdev():
                continue
            target = dest / name
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            if member.issym():
                link = member.linkname
                if link.startswith("/"):  # /lib/x86_64-linux-gnu/libm.so.6 -> inside the sysroot
                    link = os.path.relpath(dest / link.lstrip("/"), target.parent)
                if target.is_symlink() or target.exists():
                    target.unlink()
                os.symlink(link, target)
            elif member.islnk():
                source = dest / (member.linkname[2:] if member.linkname.startswith("./") else member.linkname)
                if source.exists():
                    shutil.copy2(source, target)
            elif member.isfile():
                with t.extractfile(member) as src, open(target, "wb") as out:
                    shutil.copyfileobj(src, out)
                os.chmod(target, member.mode & 0o755 | 0o600)
    stamp.write_text(SNAPSHOT + "\n")


def main():
    for tool in ("clang", "cmake", "ninja", "ld.lld"):
        if not shutil.which(tool):
            raise SystemExit(f"falta {tool} nesta máquina (a de release)")
    sysroot = WORK / "sysroot"
    unpack_sysroot(download(*SYSROOT, DOWNLOADS / "steamrt" / "com.valvesoftware.SteamRuntime.Sdk-amd64,i386-sniper-sysroot.tar.gz"),
                   sysroot)
    source_archive = download(*SDL, DOWNLOADS / f"SDL3-{SDL_VERSION}.tar.gz")
    source = WORK / f"SDL3-{SDL_VERSION}"
    if not (source / "CMakeLists.txt").is_file():
        with tarfile.open(source_archive) as t:
            t.extractall(WORK, filter="data")
    build, install = WORK / "sdl3-build", WORK / "sdl3-install"
    for folder in (build, install):
        if folder.exists():
            shutil.rmtree(folder)
    # pkg-config and the compiler flags split paths at spaces ("Área de trabalho"): the sysroot through
    # a link without them.
    import tempfile
    link = Path(tempfile.gettempdir()) / "crashrecomp-steamrt-sysroot"
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(sysroot)
    sysroot = link
    # SDL generates its Wayland protocol code with wayland-scanner: the sysroot's (1.18), matching its
    # headers (a newer scanner calls wl_proxy_marshal_flags, which 1.18 lacks), run with the sysroot's
    # own loader and libraries.
    scanner = WORK / "wayland-scanner"
    scanner.write_text("#!/bin/sh\n"
                       f'exec "{sysroot}/lib64/ld-linux-x86-64.so.2" --library-path '
                       f'"{sysroot}/lib/x86_64-linux-gnu:{sysroot}/usr/lib/x86_64-linux-gnu" '
                       f'"{sysroot}/usr/bin/wayland-scanner" "$@"\n', encoding="ascii")
    scanner.chmod(0o755)
    if subprocess.run([str(scanner), "--version"], capture_output=True).returncode != 0:
        raise SystemExit("o wayland-scanner do sysroot não roda nesta máquina")
    env = dict(os.environ, PKG_CONFIG_SYSROOT_DIR=str(sysroot),
               PKG_CONFIG_LIBDIR=os.pathsep.join(str(sysroot / p) for p in ("usr/lib/x86_64-linux-gnu/pkgconfig", "usr/share/pkgconfig")),
               PKG_CONFIG_PATH="")
    configure = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                 "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++", f"-DCMAKE_SYSROOT={sysroot}",
                 "-DCMAKE_C_COMPILER_TARGET=x86_64-linux-gnu", "-DCMAKE_CXX_COMPILER_TARGET=x86_64-linux-gnu",
                 "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld", "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld",
                 f"-DCMAKE_INSTALL_PREFIX={install}", "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER",
                 "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_TESTS=OFF", "-DSDL_EXAMPLES=OFF",
                 "-DSDL_DEPS_SHARED=ON", "-DSDL_X11=ON", "-DSDL_WAYLAND=ON", "-DSDL_WAYLAND_LIBDECOR=ON", "-DSDL_PIPEWIRE=ON",
                 "-DSDL_PULSEAUDIO=ON", "-DSDL_ALSA=ON", "-DSDL_JACK=OFF", "-DSDL_SNDIO=OFF", "-DSDL_VULKAN=ON",
                 "-DSDL_OPENGL=ON", "-DSDL_OPENGLES=ON", "-DSDL_KMSDRM=OFF", "-DSDL_HIDAPI=ON", "-DSDL_DBUS=ON",
                 "-DSDL_IBUS=ON", "-DSDL_LIBUDEV=ON", "-DSDL_LIBURING=OFF", "-DSDL_ASAN=OFF", f"-DWAYLAND_SCANNER={scanner}"]
    subprocess.run(configure, env=env, check=True)
    subprocess.run(["cmake", "--build", str(build)], env=env, check=True)
    subprocess.run(["cmake", "--install", str(build)], env=env, check=True, stdout=subprocess.DEVNULL)

    library = next((install / "lib").glob("libSDL3.so.0.*"))
    needed = set(subprocess.run(["readelf", "-d", str(library)], capture_output=True, text=True, check=True).stdout.split())
    needed = {word.strip("[]") for word in needed if word.startswith("[lib") or word.startswith("[ld-")}
    extra = sorted(needed - ALLOWED_NEEDED - {"libSDL3.so.0"})
    if extra:
        raise SystemExit(f"{library.name} ficou linkada a {', '.join(extra)}: algum backend não foi por dlopen")
    if OUT.exists():
        shutil.rmtree(OUT)
    (OUT / "lib").mkdir(parents=True)
    shutil.copy2(library, OUT / "lib" / "libSDL3.so.0")
    shutil.copytree(install / "include" / "SDL3", OUT / "include" / "SDL3")
    shutil.copy2(source / "LICENSE.txt", OUT / "LICENSE.txt")
    (OUT / ".portable").write_text(f"SDL {SDL_VERSION}, Steam Runtime 3 sysroot {SNAPSHOT}, every backend by dlopen\n")
    print(f"{OUT / 'lib' / 'libSDL3.so.0'}: NEEDED {', '.join(sorted(needed))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
