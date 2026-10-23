# Third-party notices

CrashRecomp's own code is MIT (see `LICENSE`). It uses the components below, each under its own
license. None of their binaries are stored in git: `tools/setup.py` downloads them from the
official sources listed here (pinned versions, checked by SHA-256), and the release bundle carries
the same files next to their licenses (`dist\licencas\` in the play folder).

| Component | Used for | License | Source |
|---|---|---|---|
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (XenonAnalyse, XenonRecomp, XenonUtils) | Recompiling `default.xex` to C++; XenonUtils (XEX decryption/decompression) is compiled into `CrashRecomp.exe` | MIT | commit `ddd128bcca99fe8bfbb99bea583c972351fa6ace` + `patches/XenonRecomp.patch` |
| [SIMDe](https://github.com/simd-everywhere/simde) (XenonRecomp submodule) | Vector instructions of the recompiled code | MIT | XenonRecomp submodule |
| [libmspack](https://github.com/kyz/libmspack) `lzxd.c` (XenonRecomp submodule) | LZX decompression of `default.xex` (compiled into `CrashRecomp.exe`) | LGPL 2.1 | XenonRecomp submodule. The full source is public, so the exe can be relinked with a modified libmspack (build it yourself) |
| [tiny-AES-c](https://github.com/kokke/tiny-AES-c) (XenonRecomp submodule) | AES decryption of `default.xex` | Unlicense (public domain) | XenonRecomp submodule |
| fmt, toml++, xxHash, disasm, TinySHA1 (XenonRecomp submodules) | The recompiler tools only (not in the game exe) | MIT / BSD-2 / BSD-2 / GPL-compatible (see each) | XenonRecomp submodules |
| [FFmpeg](https://ffmpeg.org) 8.1 (`avcodec-62`, `avutil-60`, `swresample-6`), LGPL build by [BtbN](https://github.com/BtbN/FFmpeg-Builds) | XMA audio (WMA Pro decoder), loaded at run time with `LoadLibrary` | LGPL v3 (`thirdparty/ffmpeg/LICENSE.txt`) | `ffmpeg-n8.1.3-9-g29e619e767-win64-lgpl-shared-8.1.zip` (BtbN autobuild-2026-09-30-13-08); source: https://github.com/FFmpeg/FFmpeg/tree/n8.1.3 |
| [NVIDIA DLSS](https://github.com/NVIDIA/DLSS) 310.9.16 (`nvngx_dlss.dll`, NGX SDK headers incl. the Vulkan ones, and `nvsdk_ngx_s.lib`) | DLSS Super Resolution / DLAA (NGX Vulkan API) | NVIDIA RTX SDKs License (`thirdparty/dlss/LICENSE.txt`) | tag `v310.9.16` |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) 1.1.4 (FSR 3.1 upscaler: the signed `amd_fidelityfx_vk.dll` + `ffx_api` headers) | FSR 3.1 on Vulkan, loaded at run time | MIT (`thirdparty/fsr/license.md`) | tag `v1.1.4` (`PrebuiltSignedDLL/`, `ffx-api/include/ffx_api/`) |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) 1.4.363 | The Vulkan renderer (headers only; the system's `vulkan-1.dll` is loaded at run time) | Apache-2.0 OR MIT | from the LunarG Vulkan SDK 1.4.363.0 (in git, `thirdparty/vulkan`) |
| [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) (`dxcompiler.dll` with SPIR-V, `dxcapi.h`) | HLSL to SPIR-V for the Vulkan renderer, loaded at run time | Headers: University of Illinois/NCSA. The signed release binary: Microsoft DirectX Shader Compiler license (distributable code) + LLVM (`LICENSE-MS.txt`, `LICENSE-LLVM.txt` in `licencas\`) | headers in git (`thirdparty/dxc`); `bin/x64/dxcompiler.dll` of release `v1.9.2609` (`dxc_2026_09_29.zip`), downloaded by the installer |
| [SMAA](https://github.com/iryoku/smaa) | SMAA anti-aliasing | MIT (`thirdparty/smaa/LICENSE.txt`) | commit `71c806a838bdd7d517df19192a20f0c61b3ca29d` (in git) |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.91.8 | The launcher and the installer | MIT (`thirdparty/imgui/LICENSE.txt`) | tag `v1.91.8` (in git) |

## The build toolchain (`toolchain\` in the release zip)

The release zip carries the tools that build the game on the player's PC, unmodified except for
files removed to save space (other CPU targets, debuggers, manuals). They are not part of the game:
the installer runs them. Each folder keeps its own license files, listed here.
`tools/toolchain.py` downloads the same pinned archives (checked by SHA-256) for a git clone.

| Component | Used for | License | Source / license files |
|---|---|---|---|
| [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 20261006 (UCRT, x86-64): Clang/LLVM 23, LLD, libc++, libunwind, compiler-rt builtins | Compiling and linking everything (`clang`, `clang++`, `ld.lld`) | Apache 2.0 with LLVM Exceptions | `toolchain\llvm-mingw\LICENSE.TXT` |
| [mingw-w64](https://www.mingw-w64.org) headers, CRT, import libraries and winpthreads (part of llvm-mingw) | Windows headers and import libraries, the C runtime startup code | ZPL 2.1 / public domain (headers, CRT), MIT-style (winpthreads) | `toolchain\llvm-mingw\x86_64-w64-mingw32\share\mingw32\COPYING*` |
| [CMake](https://cmake.org) 4.4.4 (`cmake.exe` + its modules) | Configuring the builds | BSD 3-Clause; its bundled libraries (curl, libarchive, zlib, zstd, liblzma, libuv, nghttp2, rhash, cppdap) under their own licenses | `toolchain\cmake\doc\cmake\` |
| [Ninja](https://ninja-build.org) 1.13.2 | Running the builds | Apache 2.0 | `toolchain\ninja\COPYING.txt` |
| [Python](https://www.python.org) 3.13.17 (official embeddable package, amd64) | Running `tools\setup.py` and the asset tools | PSF License (and its bundled OpenSSL, libffi, SQLite, zlib...) | `toolchain\python\LICENSE.txt` |
| [NumPy](https://numpy.org) 2.5.4 (wheel, with its bundled OpenBLAS / gfortran runtime) | The asset export (textures, models) | BSD 3-Clause; bundled libraries under their own licenses | `toolchain\python\Lib\site-packages\numpy-2.5.4.dist-info\licenses\` |
| [Pillow](https://python-pillow.org) 13.0.0 (wheel) | The asset export (PNG textures) | MIT-CMU (HPND); bundled libraries (libjpeg-turbo, libpng, zlib, FreeType...) under their own licenses | `toolchain\python\Lib\site-packages\pillow-13.0.0.dist-info\licenses\` |

Programs built with this toolchain (`CrashRecomp.exe`, `CrashLauncher.exe`, `CrashSetup.exe`) link
libc++ and libunwind (LLVM Exceptions: no notice needed in binaries) and the mingw-w64 runtime
statically; the mingw-w64 / winpthreads notices above cover the prebuilt `CrashSetup.exe`.
`CrashRecomp.exe` links NVIDIA's `nvsdk_ngx_s.lib` (below) unchanged; `runtime/gpu/ngx_msvc_compat.cpp`
(our code) provides the few Visual C++ runtime functions that library expects.

Optional tools used only by the asset exporter (`tools/assettool`, built on your machine): FFmpeg
(above) and, for Bink videos to MP4, Cisco's OpenH264 when present in the FFmpeg build.

## The game

Crash of the Titans is © Activision / Radical Entertainment. This project contains no game files,
no game code and no game assets. You need your own legally obtained copy of the Xbox 360 game;
everything game-specific is produced on your machine from that copy. The optional release assets
(HD AI Remaster, subtitles) are fan-made derivative content, distributed separately from the code
so they can be removed on request without touching the repository.
