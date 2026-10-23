# DirectX Shader Compiler (DXC)

The Vulkan renderer (`runtime/gpu/rhi_vulkan_shaders.cpp`) compiles the translated HLSL to SPIR-V
with Microsoft's [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler), loaded
at run time (`LoadLibrary("dxcompiler.dll")`), license **University of Illinois/NCSA Open Source
License** (`LICENSE.TXT` in that repository; the release zips carry it).

- `include/dxcapi.h`, `include/WinAdapter.h`: the API headers (in git), from the LunarG Vulkan SDK
  1.4.363.0;
- `bin/` (not in git): `dxcompiler.dll` **with SPIR-V code generation** and its licenses
  (`LICENSE-MS.txt`, `LICENSE-LLVM.txt`), which `tools/setup.py` takes from Microsoft's signed
  release `v1.9.2609` (`dxc_2026_09_29.zip`, `bin/x64/dxcompiler.dll`, pinned SHA-256). The Vulkan
  SDK's `Bin/dxcompiler.dll` works too; the Windows SDK's copy has no SPIR-V backend and won't do.
  CMake copies it next to `CrashRecomp.exe` (from here, or from `%VULKAN_SDK%\Bin` when this folder
  has none).

Without it the Vulkan renderer can only use SPIR-V already in `userdata\shadercache\*.spv`; the
Direct3D 12 renderer (FXC, `d3dcompiler_47.dll` from Windows) doesn't need it.
