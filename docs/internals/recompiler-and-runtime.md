# Recompiler and runtime

Full write-ups: `runtime/NOTES.md` steps 1-17, GUIA "Funções engolidas", "Lendo o código do jogo",
"Correções locais no XenonRecomp".

## Toolchain (`toolchain\`, `tools/toolchain.py`, `tools/buildenv.py`)

- Default: the portable **llvm-mingw** (clang + lld + mingw-w64, UCRT, x86-64) + CMake + Ninja + the
  embeddable Python with numpy/Pillow, all in `toolchain\` (release zip; a clone runs
  `python tools/toolchain.py fetch`). `[build] toolchain = "msvc"` / `CRASH_TOOLCHAIN=msvc` = VS + clang-cl.
  A build folder remembers its toolchain (`.crashrecomp_toolchain`) and is reconfigured on a switch.
- mingw specifics in `CMakeLists.txt`: `-mavx`, `-O2` (= clang-cl `/O2`), `-static` (no libc++/libunwind/
  winpthreads DLLs), `_WIN32_WINNT=0x0A00` (mingw-w64 defaults to older APIs: `SetProcessInformation`,
  `GetDpiForSystem`...), `xaudio2_9`, `-gcodeview -Wl,--pdb=` + `-Wl,--Xlink=/map:` (the MSVC-style map
  `tools/perf` parses; names are Itanium-mangled, `profile.py` demangles both).
- **DLSS**: NVIDIA's static `nvsdk_ngx_s.lib` (MSVC, /MT) links unchanged under lld in mingw mode into
  `CrashDlss.dll` (not the exe since v1.05) with `/nodefaultlib:LIBCMT,OLDNAMES,libcpmt` and
  `runtime/gpu/dlss_module/ngx_msvc_compat.cpp`, which supplies the 12
  missing MSVC symbols (`_Init_thread_*`, `__security_cookie`, `__GSHandlerCheck[_EH]`, `_fltused`,
  MSVC-mangled `operator new/delete`, `std::_Xbad_alloc/_Xlength_error`). `__CxxFrameHandler3` and
  `_CxxThrowException` come from ucrtbase (`api-ms-win-crt-private`). No Streamline needed.
- XenonRecomp under llvm-mingw needs `-include cstdlib` (its `fmt` uses `malloc` without the header) and
  emits a byte-identical `generated/ppc`. FSR (DLLs loaded at run time) and FFmpeg (DLLs) needed nothing.
- Measured 2026-10-23 (5700X): clean build 46 s (mingw) vs 50 s (clang-cl); `desempenho_vila_direto`
  ~190-200 fps vs ~177-180 fps.

## XenonRecomp config (`config/Crash.toml`)

- Entry `_xstart` 0x82264F40. Helpers: savegprlr_14 0x823CEBE0, restgprlr_14 0x823CEC30, savefpr_14 0x823CFED0,
  restfpr_14 0x823CFF1C, longjmp 0x823D2EA0, setjmp 0x823D2FC0 (leaf, no .pdata). No VMX save/restore helpers.
- 380 jump tables (`config/Crash_switch_tables.toml`), 12689 .pdata functions, 20144 recompiled functions.
- The config has 68 **explicit functions**: leaf functions without .pdata sitting in gaps after .pdata
  entries. The linear scan cut them short, so their jump tables "jumped outside the function". Some explicit
  ranges were later split at inner entries that are referenced only from data (function-pointer tables);
  missing one shows up at run time as `indirect call to unmapped 0x...`.
- **Any new explicit range must be checked for data pointers into it.** `tools/find_swallowed.py` finds
  functions swallowed by explicit ranges (data pointers + lis/addi pairs + bl targets vs generated sizes).
- Grep `generated/ppc` for `// ERROR <addr>` when something silently doesn't happen.
- Recompiler flags: skip_lr / skip_msr / ctr / xer / reserved / cr_as_local are ON.
  `non_argument_as_local` and `non_volatile_as_local` **break the game**: SEH `__finally` funclets get the
  parent frame in r12 and use the parent's r14-r31.

## Local XenonRecomp patches (`patches/XenonRecomp.patch`)

Reapply if XenonRecomp is re-cloned (`tools/setup.py` does it; the release zip carries the source with
the patch already applied, stamped in `tools/XenonRecomp/.crashrecomp_source`):
- xex.cpp: basic-compression buffer sized max(blocks, imageSize) (was out of bounds).
- XenonAnalyse/function.cpp: conditional bclr/bcctr searched `SearchBlock(relative)` instead of
  `SearchBlock(base+rel)` -> infinite loop + memory growth (the hang at 0x8237FF6C).
- recompiler.cpp: skips zero-sized functions; LRU-hint VMX variants (lvxl/lvlxl/lvrxl/stvxl/stvlxl/stvrxl
  + 128) share the base handlers; new handlers vandc, dcbst (no-op), fcmpo (= fcmpu), frsqrte, mulhdu, mcrf.
- Jump-table switches are emitted on `.u32`, not `.u64` (the upper half can be garbage, e.g. -1+1 =
  0x1_00000000; sub_822AA070 hit this).
- stderr `[phase]` / `[analyse]` progress markers.

## Runtime basics

- 152 function imports (C++-mangled `__imp__*`, weak stubs overridden by a strong `PPC_FUNC(__imp__X)`) and 10
  xboxkrnl **variable** imports: these are not symbols; IAT slots 0x82000490-0x820005B0 hold byte-swapped
  import records that the loader patches.
- Hooking a recompiled function: the generated code calls `sub_X` with **C++ linkage** (a weak alias of the C-linkage
  `__imp__sub_X`). A strong `PPC_FUNC(sub_X)` overrides it; declaring it `extern "C"` first makes a separate
  C symbol that the game never calls (the hook silently does nothing). Declare guest functions you call as plain
  `PPC_FUNC(sub_X);` and originals as `extern "C" PPC_FUNC(__imp__sub_X);`.
- Memory: virtual heap 0x40000000-0x7FC00000; physical 0xA0000000-0xC0000000, aliased at 0xA0000000 and
  0xC0000000 (0xE0000000 unmapped; the console offsets it by 0x1000). Critical sections owner = KTHREAD; TLS in
  the guest PCR array. KeSetAffinityThread sets PCR+0x10C.
- The old "heap recursion" at boot was the CRT's `_getptd` failing with stub TLS -> `_amsg_exit` loop.
- Dispatcher objects: the header WaitListHead holds an RCMP signature + host index.
- **Fibers**: the loader runs on XAPI guest fibers; `SwitchToFiber` (asm at 0x823E9C10) had been emitted as an
  empty function. `kernel/fibers.cpp` maps guest fibers to host fibers (this unblocked boot "state 2").
- `VdGetSystemCommandBuffer` must zero 0x94 bytes. `PM4_INTERRUPT` is synchronous. `VdSwap` writes exactly
  64 dwords (the D3D cursor = last written dword).
- `GetLastError` must be real (`RtlNtStatusToDosError`), or missing-file paths (the Sierra video that a dump
  may not have) go wrong. XMP (Xbox Music Player) drives the menu music (`kernel/xmsg_imports.cpp`).
- `timeBeginPeriod(1)` is required: the vblank timer otherwise ticks on 15.6 ms and the game stutters.
- VFS: `D:/game:` -> the game folder (read-only) with the `mods\` overlay + `load_order.txt`;
  `hdd:/cache:` -> `userdata\`. The image is decrypted from `<game>\default.xex` at startup
  (`runtime/xex_image/`); `image.bin` is only for the tools.

## GPU and audio basics

- GPU MMIO window 0x7FC80000 (first page write-trapped via single-step); PM4 command-processor thread; vblank
  interrupt thread.
- `PM4_EVENT_WRITE_EXT` is 0x5A, not 0x59: without the screen extents D3D bins every draw to tile 1 and the
  screen is cut at x=416.
- More renderer facts: [graphics.md](graphics.md).
- XMA registers at 0x7FEA0000 are **little-endian** (0x600 context array physical address read once at init,
  0x650 kick / 0x690 lock / 0x6A0 clear bitmasks). The page is read-only; the x86 `mov` store is decoded in
  the fault handler. XMA frames are parsed in the runtime and fed one per synthetic packet to FFmpeg's WMA Pro
  decoder (flags 0x10D6), because FFmpeg's xma2 holds back 4096 samples; frame end must use the packet
  header's frame count (a frame length field can straddle packets).
- Game shader HLSL sources ship with the game in `shaders\*.updb` (XML) — useful when reading shaders.

## Symbols and reading the code

- `tools/rtti_scan.py` parses MSVC RTTI (2300 classes, 2247 vtables) -> `private/symbols/{rtti.json,
  functions.csv}`; ~5600 of 20144 functions get names. `runtime/symbols.cpp` prints them in crash reports.
- `_purecall` 0x823CF1F0, `operator delete` 0x821FDB28, `_amsg_exit` 0x823D6698.
- The `rea` MCP / Ghidra on Windows cannot analyze PPC/XEX; read `generated/ppc` with the symbols instead.
