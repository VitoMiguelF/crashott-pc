# Android port (plan and status)

Branch `android-port` (worktree `CrashRecomp-android`), started 2026-10-26 from main `a2768bd`.
The goal is the game on an Android phone (ARM64, Vulkan), built from the player's own dump and
installed by sideload. It builds on the Linux port ([linux_port.md](linux_port.md)): Win32 is
already behind `runtime/platform/` and the window, input and audio already have SDL3 backends.
What Android adds is a different CPU (ARM64), a different libc (bionic), mobile GPUs and an app
lifecycle.

## Ground rules (decided before the port started)

- **No game file, and nothing derived from one, is ever shipped or put in git.** That covers the
  dump, `generated/ppc`, the recompiled `.so`, extracted assets and the SPIR-V shader cache. The
  release carries only our code, our tools and third-party binaries.
- **"Path C": the PC builds the phone's game.** The player's Windows or Linux PC already
  recompiles the dump (`setup.py`). For Android it also cross-compiles the result for ARM64 and
  assembles an APK that exists only on that PC and that phone. It is installed by sideload
  (`adb install`, or by copying the APK to the phone).
- **Not the Play Store.** The Play Store is ruled out by trademark and IP, and by Google's policy
  against running code that isn't in the APK. Path C doesn't load outside code: the game code is
  inside the APK, built for that one player. It still can't be published.
- **Recompiling on the phone is a later option** (see the end of this page).

## Status

| Milestone | State |
|---|---|
| M0: this plan | done |
| M1: runtime + generated code compile and link as `libmain.so` for `aarch64-linux-android` | **done** (2026-10-26): NDK r30, `-Wl,--no-undefined`, 0 errors |
| M2: minimal APK (SDL3's Java glue) that starts, opens a Vulkan surface and logs to logcat | **done** (2026-10-26, S20 FE) |
| M3: boot the game with the dump in the app's storage | **done** (2026-10-26, S20 FE with Turnip): intros, title, main menu and settings screen drawn from a PC-made SPIR-V cache |
| M4 onward: renderer on mobile GPUs, touch, audio, packaging, performance | planned below |

### Verified so far

- **The generated code compiles for ARM64 as it is.** Here is how, with no download: the bundled
  llvm-mingw clang targeted `aarch64-w64-mingw32`, the same headers, `-O2`. That compiled all 80
  `generated/ppc` files (61 MB of C++): 0 errors, 24 s wall clock on 16 threads (x86-64: 18 s),
  15.4 MB of objects (x86-64: 16.3 MB).
  - This proves XenonRecomp's output is ARM64-clean: SIMDe takes the NEON path, FPCR replaces
    MXCSR, and `cntvct_el0` replaces `rdtsc`.
  - The OS target differs from Android's, but the generated code uses no OS header, so only the
    ABI changes, not the C++.
  - The disassembly holds 19,462 `msr fpcr` (the FPSCR flush/rounding switches) and 11 `cntvct`
    reads in 2.37 M instructions.
- In the game's code (`image.bin`, 966,727 instructions) the barriers and atomics are rare: 9
  `sync`, 9 `lwsync`, 13 `eieio`, 24 `lwarx` + 29 `stwcx.`, 2 `ldarx` + 4 `stdcx.`, 11 `mftb`.
  Locks go through kernel imports (critical sections, events), which the runtime implements with
  host primitives. So the memory-model fix below is small and cheap.
- **The fence and rounding patch** (`patches/XenonRecomp.patch`, this branch) works:
  - Regenerating with it adds exactly 31 `PPC_SYNC/LWSYNC/EIEIO()` lines plus the
    `ppc_context.h` changes, and nothing else.
  - The x86-64 disassembly of all 80 files is **identical** to main's.
  - On ARM64 they become 28 `dmb` (before: 0; neighbours merge).
- **The shared runtime is ARM64-clean.** Every runtime file except the POSIX/SDL backends passes
  `-fsyntax-only` for `aarch64-w64-mingw32`: 100 of 103. The 3 that fail are the Win32 x64
  backends (`CONTEXT.Rip/Rax/Dr6/EFlags` in `platform_win32.cpp`, `diagnostics_win32.cpp`,
  `diagnostics_watch_win32.cpp`), which Android doesn't build.
- The NEON `HashContent` gives the same value as the SSE2 one: 30 of 30 sizes and seeds, run on
  x86 through SIMDe's NEON emulation.

### First device: Galaxy S20 FE 5G (2026-10-26)

SM-G781B, Snapdragon 865 (SM8250), Adreno 650, Android 13 (API 33), 4 KB pages, 8 cores.
- **What works:** the M2 APK installs (`adb install`), SDL starts `SDL_main` from `libmain.so`, and
  logcat shows our log. The Vulkan instance, surface and swapchain work: 4 images, 2168x1008 then
  2400x1080 once immersive, `currentTransform` = ROTATE_90, so pre-rotation matters. The screen
  cleared orange: 361 frames in 3 s, a **120 Hz** panel.
- **The stock driver is too old for the renderer.** Samsung's Qualcomm driver is **Vulkan
  1.1.128** (`adb shell cmd gpu vkjson`).
  - It has geometry shaders, ETC2/ASTC, depthClamp, anisotropy, independentBlend and
    `VK_KHR_push_descriptor`.
  - It has **no** BC textures, and no dynamic rendering, synchronization2, timeline semaphores,
    demote, extended dynamic state or GPL, not even as extensions.
  - The probe's first version read no features on a pre-1.3 driver (fixed: the 1.0 features
    always, the 1.2/1.3 structures only where they exist).
- **The game with the dump on the phone** (3.2 GB `adb push` to `files/game` in 74 s, with the
  user's OK; the old probe APK):
  - It runs on the phone up to the renderer: paths from the app folder, the 4 GB guest reservation
    at 0x100000000 (memfd views), and `default.xex` decrypted in 0.2 s. The loader then sets up
    118 + 34 imports, 10 variable imports and the 20,296-function table.
  - After that come the VFS on `game/`, `settings.toml` created in `userdata/`, and the settings
    screen's fonts and art read from `default.rcf` in 61 ms on a thread.
  - The SDL window is created on the SDL thread through `RunOnMainThread`: client 2400x1080, the
    internal resolution follows the 2.39:1 screen (1720x720, Hor+).
  - It stops at `[vulkan] Adreno (TM) 650: Vulkan 1.1, 1.3 is needed`, then shows the "Vulkan 1.3
    needed" message, logged only (no SDL box from the game thread yet), and exits with code 3.
    No crash before that.
- **Ways forward for Adreno 6xx with old drivers:**
  - Mesa's **Turnip** driver loaded by the app with **libadrenotools** (BSD-2; what the Switch
    emulators do). Turnip gives Adreno 650 Vulkan 1.3 with the extensions above, and BC textures,
    which the hardware has and Qualcomm's driver hides. Needs a Turnip build, from the Mesa source
    or a published build. **Done, see below.**
  - A Vulkan 1.1 path in the renderer (render passes, binary semaphores, static state). L.
  - Newer phones (Adreno 7xx/8xx on Android 14+) ship 1.3 drivers: the S25 Ultra is the better
    first boot target.
- **Turnip on the S20 FE (2026-10-26, with the user's OK for the downloads and the install).**
  - Built here from Mesa 26.2.5 (`python tools/android_build.py turnip`): Meson 1.12.1 with the NDK as
    a cross compiler, `-Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl -Dplatforms=android
    -Dandroid-stub=true`, no GL/EGL. Result: `libvulkan_freedreno.so`, 16.7 MB, an Android HAL module
    (exports only `HMI`), 16 KB-aligned. Two Windows-host fixes to the Mesa tree, applied by the
    script: `check_xml_includes.awk` compared `/` paths, and `win_flex --wincompat` emits `<io.h>`.
    Meson also wanted PyYAML and win_flex_bison 2.5.25 (both approved for that case).
  - libadrenotools (commit 8fae8ce, + liblinkernsbypass aa39758) is built by our CMake when
    `CRASH_ADRENOTOOLS_SOURCE` is set: static into `libmain.so`, plus its `libhook_impl.so` and
    `libmain_hook.so`, which go into the APK with Turnip. The APK now has
    `extractNativeLibs="true"` (libadrenotools loads its hooks and the driver from the native
    library folder) and is 11.9 MB, the `.so` files compressed.
  - `OpenVulkanLibrary` (`android_main_android.cpp`) picks the driver: on a kgsl device (an Adreno)
    with Turnip in the APK it asks the system driver for its version and uses Turnip below 1.3;
    `files/vulkan_driver.txt` can say `system` or `turnip`. `vulkan_loader.cpp` and the probe use
    that handle; surfaces come from `vkCreateAndroidSurfaceKHR` on SDL's `ANativeWindow`, never from
    SDL's own Vulkan loader (the window has no `SDL_WINDOW_VULKAN` on Android).
  - **Result:** `system driver: Adreno (TM) 650, Vulkan 1.1.128` → `Mesa Turnip from the APK` →
    **Turnip Adreno 650, Vulkan 1.3.354, Mesa 26.2.5, conformance 1.2.7.1**: geometry shaders, BC
    textures (BC1/BC5 sampled), dynamic rendering, sync2, demote, timeline semaphores, push
    descriptors, D24S8; "renderer requirements: all present". GPL is listed but the renderer used
    whole pipelines. The orange probe ran at 120 Hz again.
  - **The game runs on the phone:** the renderer opens (swap chain 2400x1080, 5 images, mailbox,
    4.2 GB device-local heap), the guest's main thread starts, ~900 `VdSwap`s in 27 s, the first
    Bink video plays 9.9 s, threads, events and files all work. Log: `build\android\s20fe_turnip1.log`.
  - **Nothing shows yet:** there is no shader compiler on the phone and no SPIR-V cache in its
    `userdata/shadercache`, so every shader is missing (`rect_list_gs`, `blit_vs`, `fxaa_ps`... "not
    in the shader cache"); the screen keeps the probe's last orange frame. **Next:** a SPIR-V cache
    made by a PC run (same HLSL, same keys) pushed to the phone, or DXC built for Android.
  - **The SPIR-V cache from the PC (same day):** three PC test runs on copies of
    `tests\userdata_base` (`primeira_fase`, `vila_sem_cena`, `fase_direta`, main's exe: same
    translator) recorded the game's shader microcode (`shadercache\ucode`); `CrashRecomp.exe
    --precompile-shaders --userdata <folder>` turned it into 97 `.spv` (39 shaders x 2 variants +
    the built-ins), 2 MB, pushed with `adb push` to `files/userdata/shadercache`. The HLSL has no
    GPU-dependent part, so the PC's keys are the phone's.
  - With it the phone **draws the game**: the renderer starts complete (SMAA, warm-up 39 x 2 with 0
    failures), then the logo videos with the skip prompt, the title, the main menu and the
    settings screen (a touch reached the menu as a mouse click). One run-time vertex shader was
    still missing; the phone records its microcode too, so pulling `shadercache/ucode` back,
    running `--precompile-shaders` again and pushing closed it: the next run had 0 misses.
    Logs and screens: `build\android\s20fe_shaders1.*`, `s20fe_shaders2.*`.
  - **Next on the phone:** the picture is letterboxed on a portrait screen (the activity's
    `sensorLandscape` doesn't take effect: SDL sets the orientation from the window size), no
    audio decoder yet, touch controls, and the cache step in `android_build.py` (record on the
    PC, precompile, put it in the APK or push it).
  - Seen on the way: when the screen locks during start-up Android takes the surface away and SDL
    clears the window's `ANativeWindow`; `CreateVulkanSurface` now waits up to 10 s for it.

### The Android build (M1, M2)

`python tools/android_build.py` does the whole build in ~40 s here (16 threads; 446 objects,
including SDL3).
- **Configure:** CMake with the NDK r30 toolchain file, `arm64-v8a`, API 30, `c++_static`; SDL 3.4.18
  is built from its source as a subproject.
- **Libraries:** `libmain.so` is 60 MB with line tables and 10.7 MB stripped in the APK. Its NEEDED
  are only `libSDL3.so liblog.so libandroid.so libdl.so libm.so libc.so`. The LOAD segments are
  aligned to 16 KB, only `SDL_main` is exported, and there are 29 `dmb`.
- **APK:** `javac --release 11` compiles SDL's 12 Java files + `CrashActivity`; then `d8`,
  `aapt2 link` (manifest only), the stored `.so` files, `zipalign -P 16`, and `apksigner` with a
  key made once in `build/android/keystore`.
- **Result:** `CrashOTTDE-personal.apk` was 26.3 MB with stored `.so` files (11.9 MB since Turnip:
  compressed, `extractNativeLibs="true"`). `zipalign -c -P 16` passes, `apksigner verify`
  passes, and `aapt2 dump` shows minSdk 30, target 35, `arm64-v8a` and Vulkan 1.3 required.

The build log had one new problem: `-Wasm-operand-widths` on `ppc_context.h`'s ARM64 `setcsr`,
which passed a `uint32_t` to `msr fpcr`. That left FPCR's upper half undefined. It now passes a
`uint64_t` (in the patch).

What `libmain.so` does on the phone (`runtime/platform/android_main_android.cpp`):
1. It logs the device (model, SoC, Android/API, page size).
2. It points the "exe folder" at the app's external files folder (`CRASH_ANDROID_FILES`).
3. It runs a **Vulkan probe**: window + surface. It logs geometry shaders, BC/ETC2/ASTC, the 1.3
   features, push descriptors, GPL, depth clip/bias, D24S8, and a "renderer requirements" verdict.
   Then it clears the screen orange for 3 s through a swapchain.
4. If `game/default.xex` is there, it starts the game (`CrashMain`) on a 64 MB thread, with
   `args.txt` lines as arguments, and keeps the SDL thread for the window loop
   (`plat::android::RunOnMainThread`). Without a game it shows a message and stops after the probe.

The bionic side of `platform_posix.cpp`:
- fibers through an AArch64 switch (`crash_fiber_switch`: x19–x30, sp, d8–d15, FPCR);
- `HostPage()` from `sysconf`, so commit and protect use host pages, and decommit is 16 KB-safe
  (`ZeroCommitted`);
- the ARM64 `FaultInfo` (`regs`/`pc`/`sp`, WnR from `esr_context`);
- `ExecutablePath` from `CRASH_ANDROID_FILES`;
- no restart or folder picker;
- message boxes and `Log` go to logcat (tag `CrashRecomp`).

**MMIO on ARM64 (M3, written, not run yet).**
- `FaultInfo::DecodeStore` decodes STR/STRB/STRH (unsigned immediate or register offset) and STUR.
  Checked against the NDK build of 10 `generated/ppc` files: it covers 14,486 of the 14,490 stores
  in the guest-memory form (`str w8, [x20, w9, uxtw]`). The 4 it doesn't cover are `str s2`
  (`stfs` from an FP register), which never targets a hardware register.
- The GPU handler (`gpu.cpp`) and the XMA handler (`xma.cpp`) decode the store, do it (the GPU's
  through a brief unprotect, as the x86 single step did), advance `pc` by 4 and report the
  register.
- With a 16 KB host page, stores to the neighbouring registers that now fault are done the same
  way, without a report.
- Only 12 stores carry the `PPC_MM_STORE_U32` tag, so the fault path is the one that matters.
  `plat::HostPageSize()` is new (4 KB on Windows).

**Shaders without DXC (M3).** On Android a missing `libdxcompiler.so` no longer stops the device.
The renderer uses the cached SPIR-V, and logs the first 64 shaders that aren't in the cache.

The non-Android paths still compile: Windows (`main.cpp`, `diagnostics.cpp`, `xenos_renderer.cpp`)
and the glibc path of `platform_posix.cpp` (its only errors without `__ANDROID__` against bionic
headers are the `ucontext` functions glibc has).

## Architecture on Android

```
APK (built on the player's PC, never published)
├── lib/arm64-v8a/libmain.so    runtime + generated/ppc + XexImage (SDL_main), c++_static
├── lib/arm64-v8a/libSDL3.so    SDL 3.4.18 built with the NDK (AAudio, Android input, Vulkan)
├── lib/arm64-v8a/libavcodec…   FFmpeg LGPL, only the WMA Pro / XMA decoder (M6)
├── lib/arm64-v8a/libdxcompiler.so   DXC for Android (M4b, optional)
├── classes.dex                 SDL's org.libsdl.app Java glue + a tiny CrashActivity
├── res/, AndroidManifest.xml   landscape, fullscreen, game controller and Vulkan features
└── assets/                     languages/ and our own data (no game file)

/storage/emulated/0/Android/data/<package>/files/   (the app's external files dir)
├── game/        the dump (default.xex, *.rcf, shaders/...), pushed by the PC tool
├── userdata/    settings.toml, saves, shadercache/ (pipeline cache is per device)
├── mods/
└── data/        subtitles, symbols (derived from the dump: made on the PC, pushed)
```

Everything the runtime finds "next to the exe" on PC (`paths.cpp`) comes from the app's external
files directory on Android (`SDL_GetAndroidExternalStoragePath()`), which is ordinary POSIX
storage: `pread`, `std::filesystem` and the case-insensitive `HostPath` all work unchanged.

## ARM64: every x86-only piece and its fix

Sizes: **S** = under a day, **M** = a few days, **L** = a week or more.

### Generated code and XenonRecomp (`tools/XenonRecomp`, `patches/XenonRecomp.patch`)

| What | x86 behaviour | ARM64 | Fix | Size |
|---|---|---|---|---|
| Vector instructions (VMX128) | SIMDe → SSE4.1/AVX | SIMDe → NEON (automatic) | none; check the hot helpers for speed (`vperm` = `simde_mm_perm_epi8_` = 2 `pshufb` + blend → `tbl`; `vctsxs`, `vcfux`, `vsr`) | profile later |
| FPSCR rounding / flush-to-zero | MXCSR (`getcsr`/`setcsr`) | FPCR (`mrs/msr fpcr`), already in `ppc_context.h` | **bug, fixed in this branch:** `HostToGuest[]` was shared and in x86 order (RC 1 = down). On ARM RMode 1 = +inf, 2 = -inf, so `mffs` would report "up" as "down". The patch gives each architecture its own table | done |
| FPCR write cost | `ldmxcsr` | `msr fpcr` is a serializing write on many cores; 19,462 static sites | measure; if hot, cache the mode per thread (already partly done: `csr` is kept in the context) or coalesce switches in XenonRecomp | M (only if hot) |
| `mftb` | `__rdtsc()` (TSC) | `cntvct_el0` (in `ppc_context.h`) | `KeQueryPerformanceFrequency` measures the rate against the steady clock (`kernel/thread_imports.cpp`), which works for both. On ARM, `cntfrq_el0` gives it exactly | S |
| **Memory ordering** | `sync`, `lwsync`, `eieio` emitted as **no-ops**: fine under x86 TSO | ARM64 is weakly ordered like PowerPC: without fences, the guest's lock-free code and its ring buffer writes before the `CP_RB_WPTR` kick can be reordered | **done in this branch:** the recompiler emits `PPC_SYNC()` / `PPC_LWSYNC()` / `PPC_EIEIO()`, defined in `ppc_context.h`. They are empty on x86 (identical code) and `__atomic_thread_fence` (`dmb ish`) on ARM64. 31 sites in the game | done |
| `lwarx`/`stwcx.` | plain load + `__sync_bool_compare_and_swap` (full barrier) | same: the CAS is `ldaxr/stlxr` + full barrier | none. The load is a plain load and the CAS compares values, which is the same ABA behaviour as on x86 | none |
| `fctiwz`, `fctidz`, `vctsxs` | `cvttsd2si` returns INT_MIN on overflow / NaN | SIMDe emulates the x86 result unless `SIMDE_FAST_CONVERSION_RANGE` is set (it isn't) | none for correctness; test with `XenonTests` | S (test) |
| NaN / denormal details | SSE | NEON: `min/max` with NaN, the default NaN sign | SIMDe emulates x86 `min/max` (no `SIMDE_FAST_NANS`). The guest is PPC anyway, which is closer to ARM | none |
| `char` signedness | signed | **unsigned** by default on ARM | build everything with `-fsigned-char` (generated code uses `int8_t`; the runtime may not) | S |
| `long double` | 80-bit x87 | 128-bit soft-float on Android ARM64 | none used in runtime/ (grep) | none |
| `__int128` (`mulhdu`) | native | native in clang | none | none |
| `PPC_FUNC_PROLOGUE` alignment assume, `setjmp`/`longjmp` | | same | none | none |

### Runtime (`runtime/`)

| Where | What | Fix | Size |
|---|---|---|---|
| `CMakeLists.txt` `CRASH_ARCH_FLAGS` | `-mavx` on every non-Windows build | only on x86-64; ARM64: none (NEON is baseline) plus `-fsigned-char`. **Done in this branch** | done |
| `gpu/xenos_renderer.cpp` `HashContent` | SSE2 intrinsics (XXH3-style texture hash, ~25 MB per frame) | the same lanes with NEON (`vmull_u32` + `vextq_u64`), same result (tested). **Done in this branch** | done |
| `kernel/thread_imports.cpp` | `<x86intrin.h>` | already guarded; ARM64 uses `ppc_context.h`'s `__rdtsc` | none |
| `platform/platform.h` `CpuRelax` | `pause` / `yield` | already both | none |
| `platform/platform_posix.cpp` `FaultInfo` | x86 register numbering, `REG_RIP`, `REG_ERR & 2` (write bit), `REG_EFL` trap flag | ARM64: `uc_mcontext.regs[31]`, `pc`; the write bit is `ESR_EL1.WnR` (bit 6) from the `esr_context` record (`ESR_MAGIC`) in `uc_mcontext.__reserved`. **No user single step on ARM64 Linux** | M |
| `gpu/gpu.cpp` GPU MMIO trap | single-steps the faulting store with the trap flag | First choice, no fault at all: XenonRecomp already emits `PPC_MM_STORE_U32` for every store followed by `eieio` (the MMIO convention). Define it in `ppc_config.h`/`-D` as "if the address is in the GPU/XMA register window, call the runtime's register-write hook, else a plain store". Fallback for MMIO stores without `eieio`: emulate the faulting store (decode it, write the value, advance `pc` by 4), with one decoder shared with XMA | M |
| `audio/xma.cpp` `DecodeStore` | decodes x86 `mov [mem], r32/imm32` | an AArch64 decoder for what clang emits for `PPC_STORE_U32` (`rev w, w` + `str w, [x, x]` / `str w, [x, #imm]` / `stur`, also `strh`/`strb`, `stp w` and the pre/post-index forms), value from `regs[Rt]` (`wzr` = 0) | M |
| `platform/platform_posix.cpp` fibers | `ucontext` (`getcontext`/`makecontext`/`swapcontext`) | **bionic has none**. A ~40-line AArch64 switch (save x19–x30, sp, d8–d15 and FPCR, which carries the guest's rounding/flush state, as `FIBER_FLAG_FLOAT_SWITCH` does) plus a trampoline for the first entry | S |
| `platform/platform_posix.cpp` page size | `PAGE = 0x1000` for `mprotect` alignment | see "16 KB pages" | M |

### 16 KB pages

Android 15+ supports devices with **16 KB pages** (some ship that way; Pixel 8+ has it as a
developer option). The guest's page is 4 KB, and the runtime assumes the host's is too:

- `PageAlign` (commit/protect) must use `sysconf(_SC_PAGESIZE)`. Committing 4 KB then makes
  16 KB readable, which is harmless: guest code never relies on a reserved page faulting, and
  demand commit already exists.
- **`DecommitMemory` must not drop a whole host page that is partly still committed.**
  `MADV_DONTNEED` on 16 KB would zero neighbours. Fix: release only the host pages fully inside
  the range, `memset` the partial ones to zero and leave them readable. The 4 KB committed-page
  bitmap stays as it is. `page_heap.cpp` decommits per 4 KB.
- The GPU MMIO trap protects `0x7FC80000` + 4 KB and the XMA trap protects `0x7FEA1000` + 4 KB.
  On 16 KB pages, the protection covers the neighbouring registers too. The handlers must accept
  a fault anywhere in the host page and emulate the store, but report only the trapped
  registers. With store emulation (above) this is just a wider range check.
- Texture watch (`gpu/texture_watch.cpp`) unprotects 64 KB blocks, a multiple of 16 KB. That is
  conservative and fine.
- The physical views (0xA0000000 / 0xC0000000, memfd, 512 MB) are 16 KB-aligned. The 0xE0000000
  view (+0x1000 offset) is not mapped on PC either.
- `libmain.so` must be linked with `-Wl,-z,max-page-size=16384` (NDK r28+ does it by default).
  The APK's `.so` files must be stored uncompressed and 16 KB-aligned (`zipalign -P 16`).

### bionic and Android OS differences

| Item | Linux code | Android |
|---|---|---|
| `memfd_create` | glibc | bionic from API 30 (Android 11). **minSdk 30**, or `syscall(__NR_memfd_create)` below that |
| `MAP_FIXED_NOREPLACE` | `#ifdef`'d | kernel 4.17+; the fallback (any address) is fine since `base` is a variable |
| `/proc/self/exe`, `/proc/self/cmdline` | exe path, arguments | `app_process64`: paths come from SDL (`SDL_GetAndroidExternalStoragePath`, `SDL_GetAndroidInternalStoragePath`). Arguments come from an `extras` file or Intent extras (`CRASH_BOOT`, the `[test]` events) |
| `StartSelf` (restart from settings) | `fork` + `execv` | no exec of the app. Restart the Activity (`SDLActivity` recreate) or apply settings without restarting |
| Message box, folder picker | zenity / kdialog | `SDL_ShowSimpleMessageBox`. No folder picker (SAF, see storage) |
| Fonts (`text_canvas_posix.cpp`) | FreeType + fontconfig by `dlopen` | no fontconfig, and FreeType isn't a public NDK library: bundle FreeType (or `stb_truetype` next to `stb_image`) and read `/system/fonts/Roboto-*.ttf` / `DroidSansMono.ttf` |
| FFmpeg (XMA → WMA Pro) | system or BtbN `.so` | build FFmpeg (LGPL) with the NDK: `--disable-everything --enable-decoder=wmapro,xma2 --enable-shared`, ~2 MB. Without it the game is silent, as on PC |
| Signals | `sigaction` | ART owns SIGSEGV through libsigchain: our `sigaction` call is routed into its chain, and ART passes faults outside managed code to us (emulators with fastmem do this) |
| Thread priority | `setpriority` (nice) | the same for app threads within limits; also `sched_setaffinity` to the big cores for the GPU command processor thread (the bottleneck, performance.md) and ADPF `APerformanceHint` (API 33) |
| DLSS, FSR 3.1, `CRASH_WATCH`, launcher, installer | | not on Android |

## Graphics: Vulkan on Adreno and Mali

The renderer needs Vulkan 1.3 with dynamic rendering, synchronization2, timeline semaphores,
`shaderDemoteToHelperInvocation`, **geometry shaders** and `VK_KHR_push_descriptor`
(`rhi_vulkan.cpp`, device selection). That is fine on a desktop, but on phones:

| Requirement | Adreno (Qualcomm, 6xx/7xx/8xx) | Mali (Arm, Valhall / 5th gen) | Plan |
|---|---|---|---|
| Vulkan 1.3 | yes on recent drivers (Android 13+) | yes on recent drivers | keep; log and refuse below |
| `geometryShader` | yes | **no** (Mali has never exposed it) | GS-free path: rect lists (`RECT_LIST_GS`) expanded to two triangles in the vertex shader (vertex pulling from the storage ring, already used) or on the CPU; `HUD_GS` (Hor+ 2D) moved into the VS. Needed for Mali, optional on Adreno |
| BC1–BC5 textures (`textureCompressionBC`) | mostly yes on 7xx; check per device | **no** (ASTC/ETC2 only) | when the format reports no `SAMPLED_IMAGE` support: decode to RGBA8 on upload (CPU `bcdec`, public domain, or a compute shader). That costs 4–8× the memory for those textures. Transcoding to ASTC is an option for later |
| `VK_KHR_push_descriptor` | recent drivers | recent drivers | already only for draws (passes use regular sets): a fallback to per-draw sets from a pool if missing |
| D24S8 sampled | yes | yes | the renderer already falls back to D32S8 |
| `depthClamp`, anisotropy, `independentBlend` | yes | varies | already optional |
| `VK_EXT_graphics_pipeline_library` (async shaders) | some drivers | rare | the fallback already exists (whole pipelines; `CRASH_VULKAN_NO_EXT=gpl` tests it). `shader_compilation = async` still works without GPL, just slower to link |
| Tile-based GPUs | | | the renderer ends and restarts dynamic rendering for barriers and resolves. On a tiler each restart is a full tile load/store. Measure, then batch barriers and use `DONT_CARE` load/store ops where the EDRAM model allows |

Upscalers: no DLSS or FSR 3 on Android. The internal resolution scale works as on PC (the phone
can render below its panel resolution). FSR 1 (a spatial shader, MIT) could be added as a pass
later. Swapchain: `VK_KHR_surface` + `VK_KHR_android_surface` through `SDL_Vulkan_CreateSurface`
(unchanged). Handle `preTransform` (use `currentTransform`, or rotate in the present pass to avoid
the compositor's extra copy), `VK_ERROR_SURFACE_LOST_KHR` and surface recreation (see lifecycle).
The loader is `libvulkan.so` (the name is already in the Linux notes).

### Shaders

The renderer translates Xenos microcode to HLSL, then DXC turns it into SPIR-V, cached by the HLSL
hash in `userdata/shadercache`. There is no DXC binary for Android, so:

1. **M3/M4a: PC-made cache.** The PC tool runs `CrashRecomp --precompile-shaders` against the dump
   and pushes `shadercache/*.spv` to the phone. That covers the game's pixel shader files plus
   whatever microcode was recorded (`shadercache/ucode/`). It does **not** cover vertex shaders
   that were never seen: D3D binds them with vertex declarations at run time. The HLSL also depends
   on graphics options (`graphics::PatchShader`), so a settings change can miss. Good enough to boot
   and to test.
2. **M4b: DXC on the phone.** Cross-compile DXC (open source, LLVM-based) with the NDK into
   `libdxcompiler.so` (tens of MB), on the release machine or CI, from source. It holds no game
   data, so it may be shipped like the Windows `dxcompiler.dll`. This makes the phone
   self-sufficient. A recorded-ucode list shared between the player's PC and phone helps warm-up.

The pipeline cache (`VkPipelineCache`, `pipeline_keys.bin`) is per device and per driver. The
phone builds its own.

## Window, lifecycle and threads

- SDL3 on Android runs `SDL_main` on its own native thread ("SDLThread"). Window and event calls
  must stay on that thread. Today `window_sdl.cpp` creates the window on a dedicated window thread
  while the game runs on the main thread. On Android the roles swap: `SDL_main` keeps the window
  and the event loop, and starts the game on a new thread (64 MB stack, as for guest threads).
- **Lifecycle:** `SDL_EVENT_WILL_ENTER_BACKGROUND` → pause the guest (stop the vblank interrupt
  thread and the frame pacing, mute the audio stream), drop the swapchain and surface.
  `DID_ENTER_FOREGROUND` → recreate the surface (`SDL_Vulkan_CreateSurface` again: the
  `ANativeWindow` changes), the swapchain and the audio. `SDL_EVENT_LOW_MEMORY` → log. An
  interrupted save is the main risk: pause only between guest frames.
- Landscape only (`sensorLandscape`), immersive fullscreen, display cutout `shortEdges`, refresh
  rate via `ANativeWindow_setFrameRate` (30/60).
- `android:hasFragileUserData="true"`: when the player uninstalls, Android offers to keep
  `Android/data/<package>` (saves, dump, shader cache).

## Input

- **Gamepads (Bluetooth / USB):** SDL's Android joystick/gamepad backend feeds the existing
  `kernel/xinput_backend_sdl.cpp` and `hid_pads_sdl.cpp` (DualSense via SDL's HIDAPI over USB;
  over Bluetooth Android exposes it as a standard gamepad). Nothing new apart from testing.
- **Touch overlay (new, M5):** a virtual Xbox pad. The left stick is floating (left half of the
  screen), A/B/X/Y are on the right, the triggers, bumpers and START/BACK along the edges, and the
  layout follows the existing button icon art. SDL finger events map to an `XINPUT_STATE` in pad
  slot 0 (`pad_slots.cpp`). The renderer's overlay pass draws it, at 50% opacity, and hides it
  when a physical pad is used (the same "last device used" logic as the icons). Haptics: short
  `SDL_RumbleGamepad`-like vibration through `SDL_HapticRumble` on the phone.
- **Keyboard/mouse:** they work through SDL as on Linux (Bluetooth keyboards, DeX).

## Audio

`audio/audio_device_sdl.cpp` is unchanged. SDL3 uses AAudio (API 27+) or OpenSL ES, so the output
works with nothing new. The XMA decoder needs FFmpeg's WMA Pro (see the bionic table). The latency
and buffer size (`SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES`) still need tuning on the device.

## Storage: where the game files live on the phone

| Option | For | Against |
|---|---|---|
| **App-specific external storage** `Android/data/<pkg>/files/` (chosen) | ordinary POSIX paths (the VFS, `pread`, case folding work as they are); no permission needed; `adb push` reaches it from the PC tool; visible over USB (MTP) on most phones | deleted on uninstall unless the player keeps it (`hasFragileUserData`); some file managers hide `Android/data` on Android 11+ |
| App-internal storage (`/data/data/<pkg>/files`) | private, fast | not reachable by adb push or USB without `run-as`; same uninstall issue |
| Storage Access Framework (a folder the player picks) | the player picks any folder, survives uninstall | `content://` URIs, not paths: every read would go through JNI `ContentResolver` (slow for the 1.6 GB of `.rcf` files and their random reads). **Use it only to import:** an in-app "Import game" picks the dump folder with `ACTION_OPEN_DOCUMENT_TREE` and copies it into app storage once (M7) |
| `MANAGE_EXTERNAL_STORAGE` ("all files") | any path | a special permission meant for file managers; not needed |

The dump is ~1.6 GB (3.1 GB with `remaster_full`; HD packs are optional). The PC tool pushes
`game/`, `data/` and the starter `userdata/` with `adb push` (each push needs the player's OK in
our tooling), or the player copies them over USB.

## Distribution: building the APK on the player's PC (path C)

`tools/android_build.py` (new), run after `setup.py` has recompiled the dump on that PC:

1. **Compile `libmain.so`:** CMake with the NDK's `android.toolchain.cmake`,
   `ANDROID_ABI=arm64-v8a`, `ANDROID_PLATFORM=android-30`, `ANDROID_STL=c++_static`, our bundled
   CMake + Ninja. Sources are the same as the Linux build (`*_posix.cpp`, `*_sdl.cpp`) plus
   `*_android.cpp` where they differ. `generated/ppc` comes from the player's `setup.py` run. The
   game is a `SHARED` library with `SDL_main`, not an executable.
2. **SDL3 for Android:** built once from the pinned SDL 3.4.18 source with the same NDK
   (`libSDL3.so`). The release can ship it prebuilt, because it is our and SDL's code only.
3. **Java:** SDL's `android-project/app/src/main/java/org/libsdl/app/*.java` plus a small
   `CrashActivity extends SDLActivity` (`getLibraries()` = SDL3, main; storage path; immersive
   mode). Compiled with `javac --release 11 -cp android.jar`, then `d8 --min-api 30` →
   `classes.dex`. No Gradle.
4. **Resources and manifest:** `aapt2 compile` the few resources (icon, strings), then
   `aapt2 link -I android.jar --manifest AndroidManifest.xml` produces the base APK. The manifest
   declares `android.hardware.vulkan.version` 1.3 (required=true), `vulkan.level`,
   `gamepad`/`touchscreen` features, `isGame`, landscape, `extractNativeLibs=false`.
5. **Add `lib/arm64-v8a/*.so`** stored uncompressed, then `zipalign -P 16 -f 4`.
6. **Sign:** `apksigner sign` with a key generated once on that PC (`keytool` from the JDK, kept
   in the PC's `userdata/android/`). Updates must use the same key or Android refuses them: the
   player would have to uninstall, and uninstalling can drop the data. So the tool says where the
   key is and that it should be backed up.
7. **Install:** `adb install -r` over USB (with the player's OK), or copy the APK and open it
   (with "install unknown apps" allowed for the file manager).

The APK holds the recompiled game code, so it is as private as the dump. The tool names it
`CrashOTTDE-<date>-personal.apk`, and the docs say plainly not to share it. Package name:
`io.github.vitomiguelf.crashottde` (one fixed id so updates replace the app).

The **release** grows by Android tooling only: the scripts, the Java glue source, a prebuilt
`libSDL3.so` / FFmpeg / DXC for arm64. The NDK (~730 MB zip, ~2 GB unpacked; trimmed to the
Windows/Linux host LLVM + the arm64 sysroot) is fetched by `toolchain.py fetch --android` from
`dl.google.com`, pinned by checksum like the rest of `toolchain/`, only when the player asks for
the Android build.

### Recompiling on the phone (later)

XenonRecomp is portable C++ and would run on the phone, but then the 61 MB of generated C++ must
be compiled there. That needs a clang + lld + sysroot shipped in the APK (~150–250 MB), 10–30 min
of compile time on a phone, and loading the result with `dlopen` from app storage (keep the file
read-only; Android 10+ forbids `exec` from app data, but `dlopen` of a private `.so` is allowed).
Google Play would still be out (code not in the APK). Worth it only if players without a PC ask
for it; path C comes first.

## Toolchain

| Piece | Source | Notes |
|---|---|---|
| NDK r30 (LTS) | `dl.google.com/android/repository/android-ndk-r30-windows.zip`, 727,980,788 bytes | clang for `aarch64-linux-android30`, the sysroot, `android.toolchain.cmake`. Linux host: `-linux.zip` |
| build-tools 37.0.0 | `build-tools_r37_windows.zip`, 61 MB | `aapt2`, `d8` (`lib/d8.jar`), `zipalign`, `apksigner` (`lib/apksigner.jar`) |
| platform android-37.2 | `platform-37.2_r01.zip`, 68 MB | `android.jar` only (compile against it; minSdk stays 30) |
| SDL 3.4.18 source | GitHub `libsdl-org/SDL` release, the tarball `linux_sdl3.py` already pins | `libSDL3.so` + the Java glue |
| JDK | the player's, or a portable Temurin/Microsoft JDK 17+ in `toolchain/` | `javac`, `keytool`, and to run `d8`/`apksigner`. This PC has Microsoft JDK 25 |
| adb | platform-tools | this PC has it (winget). The player's tool can fetch `platform-tools-latest-windows.zip` |

Everything goes in `build/downloads/android` for development, and in `toolchain/android/` once it
is part of the player tooling. Nothing is installed system-wide, and no Android Studio or Gradle
is needed.

## Milestones

1. **M1, link. Done** (see "The Android build").
2. **M2, hello APK. Built**; the run on a phone is left: `adb install` and
   `adb logcat -s CrashRecomp`, with the user's OK. Devices: Galaxy S25 Ultra (Adreno 830), Galaxy
   S20 FE 5G SM-G781B (Adreno 650), Galaxy Tab S10 Ultra (Mali-G720 Immortalis). The GPUs are
   inferred from the models: the probe confirms them.
3. **M3, boot.** Push the dump to `Android/data/<pkg>/files/game` (with the user's OK). Needs:
   - `PPC_MM_STORE_*` routed to the register hooks, and the ARM64 store decoder and fault info
     as the fallback for the GPU/XMA traps;
   - the 16 KB-safe decommit;
   - the lifecycle basics.
   
   Done when it reaches the main menu on the phone (`--skip-intros`).
4. **M4, renderer on the device.** The PC-made shader cache (M4a), the GS-free rect lists and
   HUD, the BC decode fallback (if the device lacks BC), and DXC for Android (M4b).
5. **M5, input.** The touch overlay; gamepad tests.
6. **M6, audio.** FFmpeg wmapro for arm64.
7. **M7, player tooling.** `toolchain.py fetch --android`, the `android_build.py` flow from the
   installer or launcher, SAF import, docs.
8. **M8, performance.** The GPU command processor thread on a big core, FPCR switch cost, NEON
   hot spots, tile-friendly render passes, thermal.

## Risks

- **Mali** (no geometry shaders, no BC) needs both fallbacks before anything draws correctly.
  Adreno is the easier first target.
- **CPU speed:** the PC's bottleneck is one thread (the GPU command processor, ~500 fps in the
  village unlimited on a desktop core). A phone big core is roughly 2–3× slower per thread, so
  30 fps looks reachable, but nothing is measured yet.
- **Memory:** 4 GB of guest address space (reserved, not committed), 512 MB physical section,
  textures. Phones with 6 GB+ RAM. The low-memory killer may stop the game in the background.
- **Weak memory ordering** bugs show up as rare hangs. Fences on `sync`/`lwsync`/`eieio`, plus
  acquire loads where the runtime reads guest-written state (the ring buffer write pointer), are
  the first suspects.
- **Shader coverage** without DXC on the device (vertex shaders never recorded).
