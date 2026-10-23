# NVIDIA DLSS (NGX SDK)

Subset of the official NVIDIA DLSS SDK, https://github.com/NVIDIA/DLSS, tag `v310.9.16`
(released 2026-10-16), downloaded 2026-10-20:

- `include/`: `nvsdk_ngx.h`, `nvsdk_ngx_defs.h`, `nvsdk_ngx_params.h` and the Vulkan ones,
  `nvsdk_ngx_vk.h`, `nvsdk_ngx_defs_vk.h`, `nvsdk_ngx_helpers_vk.h` (unchanged; the Vulkan headers
  added 2026-10-23);
- `lib/nvsdk_ngx_s.lib`: `lib/Windows_x86_64/x64/nvsdk_ngx_s.lib` (static CRT, like this project);
- `bin/nvngx_dlss.dll`: `lib/Windows_x86_64/rel/nvngx_dlss.dll`, DLSS Super Resolution / DLAA 310.9.16;
- `LICENSE.txt`: the NVIDIA RTX SDKs license.

Only Super Resolution / DLAA is used, through NGX's Vulkan API (`runtime/gpu/upscaler.cpp`); the frame generation and ray
reconstruction DLLs (`nvngx_dlssg.dll`, `nvngx_dlssd.dll`) were not taken. The CMake build links
the static library and copies the DLL next to `CrashRecomp.exe`; without this folder the runtime
builds without DLSS. The license allows shipping the DLL inside an application (section 2) and
limits DLSS to systems with NVIDIA GPUs (supplement, section 1).
