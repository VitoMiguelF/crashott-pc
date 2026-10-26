# Linux port (Steam Deck first, Android later)

Branch `linux-port`. The goal is a native Linux build for the Steam Deck. SDL3 is the window,
input and audio layer so that an Android build can reuse it later. This page lists every Windows
dependency the game had, what replaces it, where each piece stands, and the x86-only code an
ARM64 build will meet.

**Before porting, try Proton:** the Windows build (Vulkan only, no D3D) may already run on the
Deck under Proton. If it does, the native port is a nicety (startup, integration), not a
requirement.

## Status in one paragraph

All runtime code now goes through `runtime/platform/` or a per-OS backend.
`python tools/check_portability.py` syntax-checks every runtime translation unit with the Win32
headers emptied: 92 of 92 are clean. The one it still flags, `gpu/rhi_vulkan_shaders.cpp`, is a
known false positive.

**The Linux build compiles and links.** `python tools/linux_cross_build.py` cross-compiles it from
Windows with zig: 189 objects (runtime, generated code, XexImage) for x86_64-linux-gnu,
glibc 2.35. The result, `build/linux/CrashRecomp`, is an ELF that needs only `libSDL3.so.0`,
`libc`, `libm` and the loader. **It has never run:** this machine has no Linux. So everything below
marked *written* compiles but is unverified at run time.

**First run (2026-10-25, CachyOS, RTX 5080, NVIDIA 615, Wayland/PipeWire, SDL 3.4, FFmpeg 9):** the
native CMake build (clang 23) boots to the main menu and into L1_E1 (`CRASH_BOOT=level:L1_E1
CRASH_SKIP_CINEMATICS=1`) with the right picture, sound and the F1 overlay, ~1000 fps unlimited.
Three fixes were needed: page-aligned `mprotect` in `CommitMemory`/`DecommitMemory`/`ProtectMemory`
(the kernel heap commits 16-byte aligned blocks: `GuestAlloc: host commit failed at 0x7F000010`),
`HostPath` splitting the guest's `\` (`make_preferred` does nothing on POSIX, so
`shaders\simpleshaderlitvp.out` was one file name and the game crashed on the missing shader) with a
case-insensitive match per component, and the system FFmpeg (below). Verified at run time: guest
memory views (memfd), demand commit and the GPU/XMA MMIO traps (SIGSEGV + trap flag: 20,372 XMA
register stores emulated), fibers, the SDL window thread, keyboard, SDL audio (PipeWire, 48 kHz).

The Windows build is unchanged in behaviour: it builds with llvm-mingw, and the `vila_sem_cena`
and `menu` profiles pass at 30 fps and with no frame limit.

## The platform layer

| File | What |
|---|---|
| `runtime/platform/platform.h` | Time (QPC / CLOCK_MONOTONIC), wakeable timer, threads (created suspended), priorities, names, fibers, guest memory (reserve with views, commit, protect, query), fault handlers, dynamic libraries, process (exe path, args, restart, env, exit), message box / folder picker, desktop display mode |
| `runtime/platform/file.h` | `plat::File`: positional read/write (pread/pwrite), size, stat, dispositions as CreateFile has them |
| `runtime/platform/text.h` | UTF-8 ⇄ wchar_t / UTF-16 without the OS (wchar_t is UTF-16 on Windows, UTF-32 on Linux) |
| `runtime/platform/keys.h` | `VK_*` codes (the `[controls]` format) for non-Windows builds |
| `runtime/platform/image.h` | PNG → RGBA (WIC on Windows) |
| `runtime/platform/text_canvas.h` | OS text drawn into a CPU bitmap (GDI on Windows), for the developer console and the subtitles |
| `runtime/kernel/xinput_types.h`, `runtime/gpu/dxgi_format.h` | The XInput structures and the DXGI_FORMAT values, without the Windows headers |
| `runtime/gpu/window.h` | The game window (Win32 or SDL3), its Vulkan surface, focus, clipboard, scripted keys |
| `runtime/kernel/xinput_backend.h` | The "Xbox" pads (XInput or SDL_Gamepad) |
| `runtime/audio/audio_device.h` | The stereo float output (XAudio2 or SDL_AudioStream) |
| `runtime/diagnostics_host.h` | Crash handler, host address names, watchdog thread sampling |

CMake picks the files by suffix: `*_win32.cpp` on Windows, `*_posix.cpp` and `*_sdl.cpp`
elsewhere. Headers that used to pull in `<windows.h>` for a type use the guard
`#if defined(_WIN32) && !defined(CRASH_NO_WIN32_HEADERS)`; the portability check defines
`CRASH_NO_WIN32_HEADERS`.

## Inventory

Sizes are rough: **S** is under a day, **M** a few days, **L** a week or more. They count the
work left for Linux, not what was moved already.

### Runtime

| Area | Windows API (was) | Replacement | State | Left |
|---|---|---|---|---|
| Window, message loop | `CreateWindowExW`, WndProc, DPI awareness, `ClipCursor`, `SetCursor` | `gpu/window_sdl.cpp`: SDL3 window on its own thread, events to `input::pc`, relative mouse mode for the camera, desktop fullscreen | written | S: compile, test on X11/Wayland/Gamescope |
| Keyboard / mouse | WM_KEY*, WM_CHAR, raw input (WM_INPUT), `GetAsyncKeyState`, `GetForegroundWindow` | SDL key/text/motion events. Scancodes map to the VK of a US layout, so they are positional (WASD stays WASD on AZERTY). `pc::KeyHeld` / `window::IsFocused` | written | S |
| Vulkan surface | `vkCreateWin32SurfaceKHR` | `SDL_Vulkan_CreateSurface`, instance extensions from SDL | written | none |
| Exclusive fullscreen | `VK_EXT_full_screen_exclusive` | none: borderless and fullscreen are both the desktop fullscreen window | written | none |
| Xbox pads | `xinput1_4.dll` | `kernel/xinput_backend_sdl.cpp`: SDL_Gamepad by position, rumble via `SDL_RumbleGamepad` | written | S |
| DualSense / DS4 HID | HID API + SetupAPI (`hid_pads_win32.cpp`, 590 lines: reports, CRC, Bluetooth) | `hid_pads_sdl.cpp`: SDL's HIDAPI drivers read the pads; the ones `SDL_GetGamepadType` calls PS3/PS4/PS5 get the `hid::` interface (own ids, `NoteDevice(PlayStation)` on activity, rumble) and are left out of the SDL "Xbox" slots, so `pad_slots.cpp` treats them as on Windows: PlayStation icons in AUTO, touchpad click = BACK. Mapping shared in `kernel/gamepad_sdl.h` | written; a DualSense (USB) is detected as one | S: confirm the AUTO icon switch by hand; DualShock 4 / Bluetooth untested |
| Audio output | XAudio2 (32-buffer ring, voice callback) | `audio/audio_device_sdl.cpp`: one `SDL_AudioStream`, SDL resamples | written | S |
| Files, VFS | `CreateFileW`, `ReadFile` with OVERLAPPED offsets, `GetFileInformationByHandle`, `FindFirstFileW` | `plat::File` (pread/pwrite, `fstat`), `std::filesystem` | done | `HostPath` splits the guest path on `\` and `/` and, when a component doesn't exist as spelled, takes the directory entry that matches it ignoring case (the dump has `DOF.out`, `LEIA.txt`). Only misses list a directory. S: a cache if mods with big folders make misses slow |
| Paths | `GetModuleFileNameW`, `%APPDATA%` | `plat::ExecutablePath` (`/proc/self/exe`). Paths are next to the exe, as on Windows | done | S: XDG data dir if the install dir is read-only (Flatpak) |
| Guest memory | `VirtualAlloc2` / `MapViewOfFile3` placeholders: physical memory aliased at 0xA0000000/0xC0000000, demand commit | memfd + `mmap(MAP_SHARED \| MAP_FIXED)` views, a committed-page bitmap for `QueryMemory`, SIGSEGV demand commit. Commit/decommit/protect round out to whole pages as VirtualAlloc does | verified | none |
| Fault handlers | vectored exception handlers (demand commit, GPU/XMA MMIO traps, texture watch) | sigaction SIGSEGV/SIGBUS/SIGTRAP. Handlers run newest first, `SetUnhandledFaultHandler` runs last. Single-step uses the x86 trap flag; the write bit comes from `REG_ERR` | written | S |
| Threads, events, TLS | `CreateThread(CREATE_SUSPENDED)`, `ResumeThread`, priorities, `SetThreadDescription`, TLS slots | pthreads with a start gate for suspended threads, `setpriority` (nice values; negative ones need CAP_SYS_NICE and fail silently), `pthread_setname_np`. Guest events and semaphores already used std primitives; guest TLS is the runtime's own table | written | S |
| Fibers | `CreateFiber` / `SwitchToFiber` | `ucontext` (makecontext/swapcontext) | written | S on glibc. **Android: bionic has no ucontext**, so it needs a small asm switch (or boost.context) |
| Timer resolution, waits | `timeBeginPeriod`, waitable timers (vblank), `SleepEx` | `nanosleep`. `CreateTimerWait` returns null on POSIX and the callers fall back to sleeping | done | none |
| Kernel / XAM HLE | `file_imports`, `memory_imports`, `thread_imports`, `rtl_imports` (time fields), `xam` content and saves called Win32 directly | all through `plat::` / `std::chrono` (`RtlTimeToTimeFields` via chrono, NT epoch 1601) | done | none |
| DLL loading | `LoadLibraryW` / `GetProcAddress` | `plat::OpenLibrary` (dlopen next to the exe first): `libvulkan.so.1` (Android `libvulkan.so`), `libavcodec.so.62` etc., `libdxcompiler.so` | done | FFmpeg: `.so` files next to the exe (`thirdparty/ffmpeg/lib`), else the system's: CMake then builds against the system headers (`find_path`) so `LIBAVCODEC_VERSION_MAJOR` matches (FFmpeg 9 = `libavcodec.so.63` on Arch). DXC: `libdxcompiler.so` of the system (`directx-shader-compiler`), required: without it the renderer refuses to start. S: packaging |
| DLSS | `CrashDlss.dll` (NGX static lib) + `nvngx_dlss.dll` | done: `libCrashDlss.so` (the same `crash_dlss.cpp` + NGX's Linux `libnvsdk_ngx.a`, built once by `tools/linux_dlss.py` against the Steam Runtime 3 sysroot, linking the system's libstdc++) + NVIDIA's `libnvidia-ngx-dlss.so.310.9.16`, next to the exe; the driver's `libnvidia-ngx.so.1` does the rest. The game, the launcher's probe and the installer's GPU check (component "dlss") use them | none |
| FSR 3.1 | `amd_fidelityfx_vk.dll` | **not on Linux for now** (decided 2026-10-25): AMD ships no Linux library. The way, when it is wanted: build FidelityFX SDK 1.1.4's ffx-api + Vulkan backend + FSR3 upscaler from source (MIT) into `libamd_fidelityfx_vk.so` once on the release machine, with the shader permutations made by a script instead of AMD's Windows-only FidelityFX_SC (KytyPS5's PR #117 did it). `upscaler.cpp` already looks for that name. Without it the launcher doesn't offer FSR and the Linux installer says "no upscaler" on non-RTX GPUs | L |
| Upscaler version info | `GetFileVersionInfoW` | `plat::LibraryFileVersion` (empty on POSIX) | done | none |
| Text drawing (console, subtitles) | GDI: `CreateFontW` (Segoe UI / Consolas), `DrawTextW` word wrap, `TextOutW`, DIB sections | `text_canvas_posix.cpp`: FreeType renders, fontconfig picks `sans-serif` (semibold) and `monospace` (Noto Sans / DejaVu here); greedy word wrap at spaces and line breaks, centred, like `DT_WORDBREAK \| DT_CENTER`; glyphs cached per font and size. Built without freetype2/fontconfig (`CRASH_HAVE_FREETYPE` unset, e.g. the zig cross-build) it stays a stub | verified (subtitles) | S: test the developer console |
| PNG decoding | WIC (language fonts, menu art from `default.rcf`) | `plat::DecodePng` with stb_image (`thirdparty/stb`, PNG only). It decodes all 20,474 PNGs in `default.rcf` byte-identical to WIC (RGB, palette + tRNS, RGBA) | done | none |
| Clipboard, dead keys | `OpenClipboard`, `ToUnicode` | `window::ClipboardText` (SDL_GetClipboardText), `FlushDeadKey` (no-op on SDL) | written | none |
| Message box, folder picker | `MessageBoxW`, `IFileOpenDialog` | `zenity` / `kdialog`, else stderr | written | S: `SDL_ShowSimpleMessageBox` once SDL is up |
| Restart from settings | `CreateProcessW` with the same console | `fork` + `execv` (`plat::StartSelf`), `waitpid`-free wait via `kill(pid, 0)` | written | S |
| Crash reports, watchdog | VEH + unhandled filter, `GetModuleHandleExA`, `SuspendThread` / `GetThreadContext` | `diagnostics_posix.cpp`: last-resort fault handler, `dladdr`. The watchdog logs guest registers but no host rip | written | S: sample threads with a signal + ucontext |
| `CRASH_WATCH` (hardware watchpoints) | debug registers via `SetThreadContext`, toolhelp | none (`diagnostics_watch_win32.cpp` is Windows only) | Windows only | M: `perf_event_open` breakpoints, if ever needed |
| Power throttling | `SetProcessInformation(ProcessPowerThrottling)` | none (true) | done | none |
| Display mode | `EnumDisplaySettingsW` | `plat::DesktopDisplayMode` (`platform_sdl.cpp`, physical pixels) | written | none |

### Launcher, installer, tools

| Piece | Windows dependency | Plan | Left |
|---|---|---|---|
| Launcher (`CrashLauncher`, `launcher/*.cpp/.inc`, ~4.5k lines) | Dear ImGui on Win32 + D3D11, WIC, `dwmapi`, WinHTTP + bcrypt (the updater), `ShellExecute` | done: the same sources with `#ifdef _WIN32` around the OS parts. SDL3 window + events + pads, Dear ImGui's SDL3 + SDL_Renderer backends, `RunLauncher(argc, argv)` (WinMain / main call it), fork + exec (`Spawn`), `SDL_OpenURL`, the system font from fontconfig (`plat::SystemFontFile`), a small SHA-256, key capture through `platform/keys_sdl.h` (shared with the game). No UPDATE yet (`updater.inc` stubs) | S: the updater (download the next AppImage) |
| Installer (`CrashSetup`, `launcher/installer*.inc`, `tools/setup.py`, `tools/make_installer.py`) | LZMS payload inside a Windows EXE, Start menu / desktop shortcuts, `CrashDlss.dll` only on RTX | done: `CrashSetup-<tag>-x86_64.AppImage` (`tools/make_appimage.py`) with the same screens; payload = the AppImage's `payload/` + `.crashrecomp_payload` index (`installer_payload.inc`), setup.py in its own process group (CANCEL = `kill(-pgid)`), `.desktop` shortcuts to the launcher (desktop + applications menu, "Add to Steam" there), install folder remembered in `~/.config/crashrecomp/install_dir`, dialogs by `plat::PickFolder` + `SDL_ShowMessageBox` | none |
| `tools/buildenv.py`, `tools/toolchain.py` | write `.cmd` scripts run with `cmd /d /c`; the portable toolchain is llvm-mingw / CMake / Ninja / Python **Windows** zips; clang path `clang.exe` | done: Linux pins in `toolchain.py` (zig, CMake, Ninja, python-build-standalone; `toolchain/bin/zig-cc` etc. wrappers with `-target x86_64-linux-gnu.2.35`); `buildenv.build_env()` / `build_commands()` instead of `.cmd`; toolchains `bundled` (zig) and `system` (PATH clang) | none |
| `tools/run_game.py` | builds through `cmd`, runs `build\CrashRecomp.exe`, `--dist` uses `dist\CrashRecomp.exe` | done: `build()` through `buildenv.build_commands`, `run.toml`'s `build/CrashRecomp.exe` read as `build/CrashRecomp`. `vila_sem_cena` and `menu` pass | none |
| `tools/make_dist.py`, `make_release.py`, `check_game_files.py`, `perf/*` | `CrashRecomp.exe` and `.dll` lists (FFmpeg, DXC, FSR, DLSS, vulkan_layers), `perf/bench.py` uses Win32 APIs | `make_dist.py` done (exe without `.exe`, the `.so` files next to it, `CrashOTTDE` with `--installed`, a Linux LEIA.txt, `/proc` for the running check); `make_release.py`, `perf/*` not yet | S each |
| Backslash paths | many tools print or join `\` paths; most go through `pathlib` and are fine | fix as found | S |

## Build status

**Windows (llvm-mingw):** builds `CrashRecomp`, `CrashLauncher` and `CrashSetup`.
`python tools/run_game.py vila_sem_cena` and `menu` pass.

**Linux, cross-compiled with zig (verified):**

- Put the [ziglang](https://pypi.org/project/ziglang/) 0.15 wheel, unzipped, in `build/downloads/zig`.
- Put the SDL3 3.2.x source tarball, unpacked, in `build/downloads/sdl`. Only its headers are used.
- Run `python tools/linux_cross_build.py`. It produces `build/linux/CrashRecomp`.

SDL3 itself is not built here, because there are no X11/Wayland headers. Instead the link uses a
stub `libSDL3.so.0` that holds the 52 SDL functions the game calls, and at run time the
system's SDL3 takes its place. No other symbol is left undefined.

`python tools/linux_cross_build.py check [file ...]` recompiles runtime files at -O0 and lists
their errors.

The first compile also found Microsoft CRT calls that the empty-header check cannot see:
`_wfopen`, `_stricmp`, `_strnicmp`, `localtime_s` and `_strtoui64`. They now go through
`platform/crt.h` / `plat::CompareNoCase`. Some files still had a stray `<windows.h>` or
`<xinput.h>` include. And `NAME_MAX` is a `<limits.h>` macro on Linux.

**Linux, CMake (verified 2026-10-25, clang 23, CMake 4.4, Ninja):** `CMakeLists.txt` has a non-`WIN32` branch for a native build:

- `-mavx`;
- `find_package(SDL3 CONFIG)`, `Threads`, `${CMAKE_DL_LIBS}`;
- a `-Wl,-Map` map file;
- no launcher, installer, DLSS or FSR;
- FFmpeg from `thirdparty/ffmpeg/lib/*.so.*` or the system;
- DXC from `thirdparty/dxc/bin/libdxcompiler.so` or `$VULKAN_SDK/lib`.

Packages (Arch): `sdl3 vulkan-icd-loader ffmpeg directx-shader-compiler freetype2 fontconfig clang cmake ninja` plus the
Vulkan driver. Build:

```sh
cmake -S . -B build/linux-native -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux-native --target CrashRecomp
```

The only compile error was a missing `<climits>` in `settings/language_font.cpp` (`LONG_MAX`).
To run it, copy `build/linux-native/CrashRecomp` next to `game/`, `data/`, `languages/` and
`userdata/` (the folder layout of `dist`).

Not done yet:

1. ~~First run~~ (done on desktop Linux; the Deck is item 5).
2. ~~VFS case-folding for the game dump~~ (done).
3. ~~`plat::TextCanvas` with FreeType~~ (done: FreeType + fontconfig).
4. First boot: guest memory views, fault handlers, fibers, the timer fallback, and audio latency
   with SDL's queue.
5. Steam Deck: run under Gamescope (fullscreen, 1280×800, the Hor+ path), Deck controls through
   SDL (Steam Input exposes them as a gamepad), and suspend/resume.
6. tools: ~~buildenv / run_game POSIX branch, FFmpeg/DXC `.so` files~~ (done, see "Building on a Steam Deck"); ~~packaging~~ (the AppImage, see "The installer"); our own SDL3 build; the installer's ImGui UI.
7. Optional: DLSS (NGX Linux), FSR 3.1 from source, launcher.

## Building on a Steam Deck (`setup.py` on Linux)

A stock SteamOS has a read-only root, no package manager, no compiler and no `-dev` headers, so
`python3 tools/setup.py --game <dump>` takes nothing from the system but glibc, Vulkan, and the
libraries its desktop already has:

| Piece | Where from | Notes |
|---|---|---|
| Compiler | zig 0.17 (`toolchain/zig`), as `toolchain/bin/zig-cc` / `zig-c++` / `zig-ar` / `zig-ranlib` | `-target x86_64-linux-gnu.2.35`: the exe needs GLIBC_2.35 at most (SteamOS 3.8 has 2.41); libc++ is linked statically, so the exe's NEEDED are libSDL3, libm, libc |
| CMake, Ninja, Python | Kitware's and ninja's Linux builds, python-build-standalone 3.13 | `setup.py` runs with `toolchain/python/bin/python3` |
| XenonRecomp | built by the same zig (`-include cstdlib`, like llvm-mingw) | |
| FFmpeg | BtbN's `linux64-lgpl-shared-8.1` (glibc 2.28): `libavcodec.so.62`, `libavutil.so.60`, `libswresample.so.6` next to the exe | they have no rpath: `audio/ffmpeg.cpp` loads avutil, then swresample, then avcodec by path |
| DXC | Microsoft's `linux_dxc_*.tar.gz`: `libdxcompiler.so` next to the exe | needs GLIBC_2.38, libstdc++ and libz (SteamOS has them) |
| SDL3 | Steam Runtime 3 (`libsdl3-0` / `libsdl3-dev` 3.4.18 .debs from repo.steampowered.com, unpacked in Python): headers + the library to link against | not shipped next to the exe: Valve's build links X11, Wayland, PulseAudio, libdecor... directly, which a bare SteamOS may lack. At run time the game uses the SDL3 Steam provides to what it starts (or the system's). Valve's pool keeps only the current version, so the installer must carry the .debs |
| FreeType, fontconfig | FreeType 2.14.3's headers (signature checked); the libraries are loaded with dlopen | `platform/text_canvas_posix.cpp` declares fontconfig's few calls itself |

Measured on a 16-thread desktop: XenonRecomp + the recompilation + the game's build from scratch in
about one minute (56-77 s). The exe gets `$ORIGIN` as **RUNPATH**: zig's linker ignores `--disable-new-dtags`, so it
comes after `LD_LIBRARY_PATH` and doesn't reach dlopen'ed libraries' dependencies (hence the FFmpeg
load order). `-Wl,-Map` is refused by zig's driver: CMake adds the map only when `check_linker_flag`
accepts it.

Still to do for the Deck: run it on one (build time on 4 Zen 2 cores, Game Mode / Gamescope, Steam
Input); our own SDL3 with every backend loaded at run time (built once at release time, against the
Steam Runtime 3 sysroot), shipped next to the exe.

## DLSS on Linux: `tools/linux_dlss.py`

`libCrashDlss.so` is `crash_dlss.cpp` (exports with default visibility, the probe's Vulkan through
dlopen) linked with NGX's `lib/Linux_x86_64/libnvsdk_ngx.a` of the same SDK 310.9.16. That library
is GCC code needing libstdc++, while the game links zig's libc++ statically: the module links the
system's libstdc++.so.6 and only its C interface is crossed. Traps found on the first runs:
- a libstdc++ exception thrown inside the module (std::filesystem::path of a wide "Área..." under the
  "C" locale) unwound into the game's libunwind/libc++abi and crashed: nothing in the module may throw
  (paths are converted to UTF-8 by hand, folders made with mkdir);
- NGX can't load its feature library from a real path that isn't ASCII ("feature not found",
  0xBAD00004), whatever the thread locale, and it resolves symbolic links: when the exe's folder
  isn't ASCII, `libnvidia-ngx-dlss.so.*` is copied (when changed) to `~/.cache/crashrecomp-ngx` (or
  `$XDG_RUNTIME_DIR` / `/tmp`, the first ASCII one) and that folder is NGX's. The data folder may have
  any name.
Checked on an RTX 5080 (driver 615.78): `vila_sem_cena` with DLSS Quality (854x480 -> 1280x720), 21,393
frames upscaled, 0 failed, ~490 fps; the launcher's probe says available and offers DLSS; the AppImage
installer's GPU check installs the two files.

## SDL3 shipped with the game: `tools/linux_sdl3.py`

Valve's libSDL3 (Steam Runtime 3, what setup.py builds against) links X11, Wayland, PulseAudio,
libdecor... directly: outside Steam, a bare SteamOS may lack one and nothing starts. `linux_sdl3.py`
(release machine, once) builds the official SDL 3.4.18 with every backend loaded by dlopen
(`SDL_DEPS_SHARED`) against the Steam Runtime 3 SDK sysroot (1.2 GB, glibc 2.31, pinned snapshot;
only its amd64 headers and libraries unpacked, absolute symlinks made relative), with the host's
clang + lld. Result: `libSDL3.so.0` needing only libc, libm, libdl, libpthread (GLIBC_2.29), into
`thirdparty/sdl3` with a `.portable` marker; CMake then copies it next to the exe and the launcher,
make_appimage carries it. Traps: paths with spaces broke `CMAKE_C_FLAGS` and pkg-config, so the
sysroot goes through a link in /tmp; the host's wayland-scanner (1.24) generates calls to
`wl_proxy_marshal_flags`, which the sysroot's 1.18 headers lack, so the sysroot's own scanner runs
through its own loader (`ld-linux-x86-64.so.2 --library-path`). Checked: the game with it opens the
window, plays through PipeWire and sees a DualSense.

## The installer: `CrashSetup-<tag>-x86_64.AppImage`

`python3 tools/make_appimage.py <tag>` (on Linux, after `toolchain.py fetch` and a `setup.py` run, or
`setup.py --installer-only` with no game, that left thirdparty/'s Linux files) builds `release/<tag>/CrashSetup-<tag>-x86_64.AppImage`, the
counterpart of CrashSetup.exe: ~176 MB (zstd squashfs), 30,000 files, of which toolchain/ is ~560 MB
and thirdparty/ ~140 MB unpacked. The payload is `make_installer.collect()` with the Linux
third-party list (no launcher, DLSS or FSR) plus `tools/appimage_install.py`; no game file, no
recompiled code. It is packed by appimagetool 1.9.1 (it carries mksquashfs; run with
`APPIMAGE_EXTRACT_AND_RUN=1`, so no FUSE or sudo on the release machine) with the type2 runtime
20251108, both pinned. GLib's option parser refused the paths with "Área" under some locales: the
script passes them relative to the project, with `LC_ALL=C.UTF-8`.

When run, AppRun starts `usr/bin/CrashSetup` (`installer.inc` built for Linux, with `usr/lib/libSDL3.so.0`):
the Windows installer's screens. With `--text` or no display it starts `tools/appimage_install.py` with
the bundled Python instead, as follows. It asks for the dump
(checked like setup.py: the files and default.xex's SHA-256), the install folder (default
`~/Games/Crash of the Titans Definitive Edition`) and the subtitles; copies the project into
`<install>/CrashRecomp`; runs its `setup.py --copy-game --play-dir <install>`, the `@@step` /
`@@progress` lines driving a zenity progress window (or kdialog + qdbus, or the terminal); and writes
`~/.local/share/applications/crashrecomp-crashottde.desktop`, whose right-click "Add to Steam" in the
Deck's desktop mode puts the game in Game Mode. `--game <dir> --dir <dir> --yes --text` installs with
no questions. The installed layout is Windows' (`CrashOTTDE`, the .so files, game/, userdata/, mods/
next to it; the project with languages/ and data/ in CrashRecomp/).

Measured here (16 threads): the whole install, with the 3.1 GB copy of the dump and the build, in
1 min 8 s (`--text`) and 1 min 20 s (the graphical installer: `--autostart --autofinish --shots`,
10 screens saved, the same as Windows' in Portuguese), then the installed CrashLauncher's PLAY started
CrashOTTDE to the menu. Inside the AppImage a restart (`Relaunch`) runs `$APPIMAGE` again, not the
exe in the mount, which goes away with this process. Not tried yet: the dialogs by hand, a Steam Deck.

The release's AppImage is built by `.github/workflows/appimage.yml` on GitHub's Ubuntu 24.04 runner,
which has no game file: `toolchain.py fetch`, `linux_sdl3.py`, `linux_dlss.py`, then
`setup.py --installer-only` (XenonRecomp's source, thirdparty/ and `cmake --build build --target
CrashSetup` with `-DCRASH_INSTALLER_ONLY=ON`, which leaves out the game targets and so needs no
generated/ppc) and `make_appimage.py <tag>`. The runner's glibc doesn't reach the result: CrashSetup is
zig's (glibc 2.35 target), libSDL3 and libCrashDlss are built against the sniper sysroot (2.31), the
rest is the pinned downloads; a step checks our three binaries against 2.35 and lists the downloads'.
On a `v*` tag it attaches the AppImage to that release (waiting up to 30 min for it, else making a
draft) and adds its line to the release's SHA256SUMS.txt, keeping the others; run by hand
(workflow_dispatch) it leaves the AppImage as a workflow artifact.

## x86-only code (for an ARM64 / Android build)

The full, current list and the Android plan are in [android_port.md](android_port.md); this
table is where it started.

| Where | What | ARM64 replacement |
|---|---|---|
| `CMakeLists.txt` (`CRASH_ARCH_FLAGS`) | `-mavx` / `/arch:AVX` for the recompiled code and the runtime | drop it. SIMDe maps the SSE/AVX intrinsics to NEON (check the hot ones for speed: `vperm`, `vmsum`, `vcfsx`) |
| `generated/ppc` (`ppc_context.h`, XenonRecomp) | SIMDe for every vector instruction, `getcsr`/`setcsr` (MXCSR) for the FPSCR rounding / flush-to-zero, `__rdtsc` for `mftb` | already has `__aarch64__` paths: FPCR for rounding and FZ, `cntvct_el0` for `__rdtsc` |
| `kernel/thread_imports.cpp` | `__rdtsc` to calibrate `KeQueryPerformanceFrequency` against the steady clock | `<x86intrin.h>` is now x86-only. On ARM64 the generated `__rdtsc` (`cntvct_el0`) is calibrated the same way (or read `cntfrq_el0`) |
| `gpu/xenos_renderer.cpp` (~line 170) | SSE2 `_mm_*` intrinsics in a 64-byte content hash | SIMDe, or the scalar loop the hash allows |
| `audio/xma.cpp` (`DecodeStore`) | decodes the x86 `mov` that faulted on the XMA register window to emulate the store | an AArch64 store decoder (STR/STRH/STRB/STP, register + immediate forms). Returns false on non-x86 for now |
| `gpu/gpu.cpp`, `audio/xma.cpp` (MMIO traps) | let the faulting store run under the x86 **trap flag** (single step), then re-protect the page | ARM64 Linux has no user single step: emulate the decoded store and advance the PC by 4 |
| `platform/platform_posix.cpp` (`FaultInfo`) | `Register(x86Index)` uses x86 numbering; the write bit comes from `REG_ERR & 2`; rip / `REG_EFL` | ARM64: `uc_mcontext.regs[]` / `pc`, and the write bit from `ESR_EL1` (WnR) in the `esr_context` record |
| `platform/platform_posix.cpp` (fibers) | `ucontext` | Android/bionic has none: an asm context switch |

## Notes and traps

- `plat::ThreadPriority` maps to nice values on Linux. Without CAP_SYS_NICE, raising priority
  silently does nothing. That is fine for correctness, but the GPU command processor thread
  (the bottleneck, see performance.md) gets no boost.
- The SDL window runs on its own thread, as on Windows. SDL allows this on X11 and Wayland as long
  as all video calls stay on that thread. `window::Placement` reads the position from the caller's
  thread, which is acceptable for a restart. Android needs the window on the main thread.
- SDL text events only start while the developer console is open. On the Deck they would bring
  up the on-screen keyboard.
- The window's `PostKey` / `PostChar` (console scripts) are SDL user events on Linux, handled on
  the window thread like real keys.
- `plat::QueryMemory(...).readable` is "committed" on POSIX: the protection is not tracked, so
  `CRASH_MEMDIFF_TIMES` would fault on a no-access guard page there.
