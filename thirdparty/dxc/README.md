# DirectX Shader Compiler (DXC)

The Vulkan renderer (`runtime/gpu/rhi_vulkan_shaders.cpp`) compiles the translated HLSL to SPIR-V
with Microsoft's [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler), loaded
at run time (`LoadLibrary("dxcompiler.dll")`), license **University of Illinois/NCSA Open Source
License** (`LICENSE.TXT` in that repository; the release zips carry it).

- `include/dxcapi.h`, `include/WinAdapter.h`: the API headers (in git), from the LunarG Vulkan SDK
  1.4.363.0;
- `bin/dxcompiler.dll` (not in git, `*.dll` is ignored): a DXC build **with SPIR-V code generation**,
  e.g. `bin/x64/dxcompiler.dll` of an official DirectXShaderCompiler release, or the Vulkan SDK's
  `Bin/dxcompiler.dll`. The Windows SDK's copy has no SPIR-V backend and won't do. CMake copies it
  next to `CrashRecomp.exe` (from here, or from `%VULKAN_SDK%\Bin` when this folder has none).

Without it the Vulkan renderer can only use SPIR-V already in `userdata\shadercache\*.spv`; the
Direct3D 12 renderer (FXC, `d3dcompiler_47.dll` from Windows) doesn't need it.
