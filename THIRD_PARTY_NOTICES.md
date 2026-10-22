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
| [NVIDIA DLSS](https://github.com/NVIDIA/DLSS) 310.9.16 (`nvngx_dlss.dll`, NGX SDK headers and `nvsdk_ngx_s.lib`) | DLSS Super Resolution / DLAA | NVIDIA RTX SDKs License (`thirdparty/dlss/LICENSE.txt`) | tag `v310.9.16` |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) 2.3.0 (FSR 3.1 upscaler, signed DLLs + API headers) | FSR 3.1 | AMD license at the top of `thirdparty/fsr/license.md` (binary redistribution with notice) | tag `v2.3.0` |
| [SMAA](https://github.com/iryoku/smaa) | SMAA anti-aliasing | MIT (`thirdparty/smaa/LICENSE.txt`) | commit `71c806a838bdd7d517df19192a20f0c61b3ca29d` (in git) |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.91.8 | The launcher and the installer | MIT (`thirdparty/imgui/LICENSE.txt`) | tag `v1.91.8` (in git) |

Optional tools used only by the asset exporter (`tools/assettool`, built on your machine): FFmpeg
(above) and, for Bink videos to MP4, Cisco's OpenH264 when present in the FFmpeg build.

## The game

Crash of the Titans is © Activision / Radical Entertainment. This project contains no game files,
no game code and no game assets. You need your own legally obtained copy of the Xbox 360 game;
everything game-specific is produced on your machine from that copy. The optional release assets
(HD AI Remaster, subtitles) are fan-made derivative content, distributed separately from the code
so they can be removed on request without touching the repository.
