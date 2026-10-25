# NVIDIA DLSS (NGX SDK)

Subset of the official NVIDIA DLSS SDK, https://github.com/NVIDIA/DLSS, tag `v310.9.16`
(released 2026-10-16), downloaded 2026-10-20:

- `include/`: `nvsdk_ngx.h`, `nvsdk_ngx_defs.h`, `nvsdk_ngx_params.h` and the Vulkan ones,
  `nvsdk_ngx_vk.h`, `nvsdk_ngx_defs_vk.h`, `nvsdk_ngx_helpers_vk.h` (unchanged; the Vulkan headers
  added 2026-10-23);
- `lib/nvsdk_ngx_s.lib`: `lib/Windows_x86_64/x64/nvsdk_ngx_s.lib` (static CRT, like this project);
- `bin/nvngx_dlss.dll`: `lib/Windows_x86_64/rel/nvngx_dlss.dll`, DLSS Super Resolution / DLAA 310.9.16;
- `LICENSE.txt`: the NVIDIA RTX SDKs license.

Only Super Resolution / DLAA is used, through NGX's Vulkan API; the frame generation and ray
reconstruction DLLs (`nvngx_dlssg.dll`, `nvngx_dlssd.dll`) were not taken.

The CMake build links the static library into **`CrashDlss.dll`** (target `CrashDlss`:
`runtime/gpu/dlss_module/crash_dlss.cpp`, our code behind a small C interface, + `ngx_msvc_compat.cpp`),
which `CrashRecomp.exe` (`runtime/gpu/upscaler.cpp`) and the launcher / installer (`launcher/gpu_probe.inc`,
NGX's capability check) load at run time, and copies `nvngx_dlss.dll` next to `CrashRecomp.exe`.

Distribution: the license lets an application ship the SDK's materials "as incorporated in object
code format into a software application" (section 1.c, requirements in section 2) and limits DLSS to
systems with NVIDIA GPUs (supplement, section 1). So the single-file installer
(`tools/make_installer.py`) carries `bin/nvngx_dlss.dll` and our prebuilt `bin/CrashDlss.dll` (the
static library inside it, in object code form) and **no header and no `.lib`**: CMake copies the
prebuilt DLL when `include/` + `lib/` are missing. A git clone downloads the SDK files above with
`tools/setup.py` (pinned by SHA-256). On a PC whose GPU can't run DLSS the installer leaves both DLLs
out (`setup.py --no-dlss`).
