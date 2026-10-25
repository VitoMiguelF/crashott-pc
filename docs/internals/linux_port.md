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
| DualSense / DS4 HID | HID API + SetupAPI (`hid_pads_win32.cpp`, 590 lines: reports, CRC, Bluetooth) | `hid_pads_posix.cpp` stub. SDL already reads these pads as gamepads. PlayStation icons and the touchpad click need the gamepad type from SDL (`SDL_GetGamepadType`) or a hidraw port | stub | M |
| Audio output | XAudio2 (32-buffer ring, voice callback) | `audio/audio_device_sdl.cpp`: one `SDL_AudioStream`, SDL resamples | written | S |
| Files, VFS | `CreateFileW`, `ReadFile` with OVERLAPPED offsets, `GetFileInformationByHandle`, `FindFirstFileW` | `plat::File` (pread/pwrite, `fstat`), `std::filesystem` | done | **M: case sensitivity.** The VFS lowercases guest paths and `HostPath` joins them to the dump folder. On a case-sensitive FS that only works if the dump is all lowercase. It needs a case-folding lookup (cached directory listing) in `fs/vfs.cpp`, the `.rcf` overlay and mods |
| Paths | `GetModuleFileNameW`, `%APPDATA%` | `plat::ExecutablePath` (`/proc/self/exe`). Paths are next to the exe, as on Windows | done | S: XDG data dir if the install dir is read-only (Flatpak) |
| Guest memory | `VirtualAlloc2` / `MapViewOfFile3` placeholders: physical memory aliased at 0xA0000000/0xC0000000, demand commit | memfd + `mmap(MAP_SHARED \| MAP_FIXED)` views, a committed-page bitmap for `QueryMemory`, SIGSEGV demand commit | written | S |
| Fault handlers | vectored exception handlers (demand commit, GPU/XMA MMIO traps, texture watch) | sigaction SIGSEGV/SIGBUS/SIGTRAP. Handlers run newest first, `SetUnhandledFaultHandler` runs last. Single-step uses the x86 trap flag; the write bit comes from `REG_ERR` | written | S |
| Threads, events, TLS | `CreateThread(CREATE_SUSPENDED)`, `ResumeThread`, priorities, `SetThreadDescription`, TLS slots | pthreads with a start gate for suspended threads, `setpriority` (nice values; negative ones need CAP_SYS_NICE and fail silently), `pthread_setname_np`. Guest events and semaphores already used std primitives; guest TLS is the runtime's own table | written | S |
| Fibers | `CreateFiber` / `SwitchToFiber` | `ucontext` (makecontext/swapcontext) | written | S on glibc. **Android: bionic has no ucontext**, so it needs a small asm switch (or boost.context) |
| Timer resolution, waits | `timeBeginPeriod`, waitable timers (vblank), `SleepEx` | `nanosleep`. `CreateTimerWait` returns null on POSIX and the callers fall back to sleeping | done | none |
| Kernel / XAM HLE | `file_imports`, `memory_imports`, `thread_imports`, `rtl_imports` (time fields), `xam` content and saves called Win32 directly | all through `plat::` / `std::chrono` (`RtlTimeToTimeFields` via chrono, NT epoch 1601) | done | none |
| DLL loading | `LoadLibraryW` / `GetProcAddress` | `plat::OpenLibrary` (dlopen next to the exe first): `libvulkan.so.1` (Android `libvulkan.so`), `libavcodec.so.62` etc., `libdxcompiler.so` | done | S: ship or depend on FFmpeg 8 / DXC .so files |
| DLSS | `CrashDlss.dll` (NGX static lib) + `nvngx_dlss.dll` | none yet: NGX ships a Linux library, so build `CrashDlss` as a .so against it | Windows only | M |
| FSR 3.1 | `amd_fidelityfx_vk.dll` | none: no official Linux binary; build the FidelityFX SDK's Vulkan backend from source | Windows only | M |
| Upscaler version info | `GetFileVersionInfoW` | `plat::LibraryFileVersion` (empty on POSIX) | done | none |
| Text drawing (console, subtitles) | GDI: `CreateFontW` (Segoe UI / Consolas), `DrawTextW` word wrap, `TextOutW`, DIB sections | `plat::TextCanvas`. The POSIX version fails every call, so **the console and subtitles are not drawn** | stub | M: FreeType or stb_truetype + a bundled font (DejaVu Sans / Sans Mono), word wrap |
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
| Launcher (`CrashLauncher`, `launcher/*.cpp/.inc`, ~4.5k lines) | Dear ImGui on Win32 + D3D11, WIC, `dwmapi`, WinHTTP + bcrypt (the updater), `ShellExecute` | not built on Linux. The in-game settings screen covers the options; on the Deck the game starts directly. Port only if wanted: ImGui SDL3 + Vulkan backends, libcurl for the updater | L |
| Installer (`CrashSetup`, `launcher/installer*.inc`, `tools/setup.py`, `tools/make_installer.py`) | LZMS payload inside a Windows EXE, Start menu / desktop shortcuts, `CrashDlss.dll` only on RTX | a tarball or AppImage with a `.desktop` file; on the Deck, "Add a non-Steam game". `setup.py`'s dump checks are plain Python and reusable | M |
| `tools/buildenv.py`, `tools/toolchain.py` | write `.cmd` scripts run with `cmd /d /c`; the portable toolchain is llvm-mingw / CMake / Ninja / Python **Windows** zips; clang path `clang.exe` | a POSIX branch: a `sh` prelude, system clang + cmake + ninja (or zig), no toolchain download | S |
| `tools/run_game.py` | builds through `cmd`, runs `build\CrashRecomp.exe`, `--dist` uses `dist\CrashRecomp.exe` | exe suffix from `sys.platform`, build via buildenv's POSIX branch | S |
| `tools/make_dist.py`, `make_release.py`, `check_game_files.py`, `perf/*` | `CrashRecomp.exe` and `.dll` lists (FFmpeg, DXC, FSR, DLSS, vulkan_layers), `perf/bench.py` uses Win32 APIs | per-OS name tables (`.so`, no extension for the exe) | S each |
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

**Linux, CMake (not tried yet):** `CMakeLists.txt` has a non-`WIN32` branch for a native build:

- `-mavx`;
- `find_package(SDL3 CONFIG)`, `Threads`, `${CMAKE_DL_LIBS}`;
- a `-Wl,-Map` map file;
- no launcher, installer, DLSS or FSR;
- FFmpeg from `thirdparty/ffmpeg/lib/*.so.*` or the system;
- DXC from `thirdparty/dxc/bin/libdxcompiler.so` or `$VULKAN_SDK/lib`.

To try it:

```sh
cmake -S . -B build-linux -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --target CrashRecomp
```

Not done yet:

1. **First run** on a Linux machine or the Deck, with SDL3 ≥ 3.2 installed. Copy
   `build/linux/CrashRecomp` next to the game files as on Windows, but not the stub `libSDL3.so.0`.
   Also try the native CMake build there.
2. VFS case-folding for the game dump.
3. `plat::TextCanvas` with FreeType or stb_truetype (developer console, subtitles).
4. First boot: guest memory views, fault handlers, fibers, the timer fallback, and audio latency
   with SDL's queue.
5. Steam Deck: run under Gamescope (fullscreen, 1280×800, the Hor+ path), Deck controls through
   SDL (Steam Input exposes them as a gamepad), and suspend/resume.
6. tools: buildenv / run_game POSIX branch, packaging (tarball/AppImage), FFmpeg/DXC `.so` files.
7. Optional: DLSS (NGX Linux), FSR 3.1 from source, HID PlayStation reader (hidraw), launcher.

## x86-only code (for an ARM64 / Android build)

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
