# AMD FSR 3.1 upscaler (FidelityFX API)

Subset of the official AMD FSR SDK 2.3.0 "Redstone" (GPUOpen FidelityFX SDK),
https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag `v2.3.0` (released 2026-06-24),
downloaded 2026-10-20. Paths keep the SDK's layout (the headers include each other relatively):

- `Kits/FidelityFX/api/include/`: `ffx_api.h`, `ffx_api_types.h`, `ffx_api_loader.h`,
  `dx12/ffx_api_dx12.h`; `Kits/FidelityFX/upscalers/include/ffx_upscale.h` (unchanged);
- `bin/amd_fidelityfx_loader_dx12.dll`, `bin/amd_fidelityfx_upscaler_dx12.dll`: the signed DLLs of
  `Kits/FidelityFX/signedbin/` (the upscaler DLL holds FSR 3.1.5, plus FSR 4.1.1 for RDNA 4 GPUs);
- `license.md`: the SDK's license file. The files above are under AMD's license at its top
  (redistribution in binary form allowed if the copyright notice, permission notice and
  disclaimers go along; no reverse engineering); the samples are MIT.

`runtime/gpu/upscaler.cpp` loads the loader DLL at run time (nothing is linked) and asks for the
FSR 3.1.x upscaler explicitly. Frame generation (`amd_fidelityfx_framegeneration_dx12.dll`) is
not used and was not taken. The CMake build copies `bin/*.dll` next to `CrashRecomp.exe`.
