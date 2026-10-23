# AMD FSR 3.1 upscaler (FidelityFX API, Vulkan)

Subset of AMD's FidelityFX SDK, https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag
`v1.1.4`, downloaded 2026-10-23 (the newer SDK 2.x only ships DirectX 12 DLLs):

- `include/ffx_api/`: `ffx_api.h`, `ffx_api_types.h`, `ffx_upscale.h` and `vk/ffx_api_vk.h`, from
  `ffx-api/include/ffx_api/` (unchanged; they include each other relatively);
- `bin/amd_fidelityfx_vk.dll`: `PrebuiltSignedDLL/amd_fidelityfx_vk.dll`, signed by AMD, with the
  FSR 3.1.4 upscaler (and FSR 2.3.3);
- `license.md`: the SDK's `LICENSE.txt` (MIT).

`runtime/gpu/upscaler.cpp` loads the DLL at run time (nothing is linked) and asks for the FSR 3.1.x
upscaler explicitly, on the renderer's own Vulkan device. Frame generation is not used. The CMake
build copies the DLL next to `CrashRecomp.exe`; `tools/setup.py` downloads these files (pinned
SHA-256). Folders left by older checkouts (`Kits/`, the `*_dx12.dll`) are not used any more.
