# Vulkan headers (Khronos)

The C headers of the Vulkan API, from the Khronos [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers)
(header version 1.4.363, as shipped by the LunarG Vulkan SDK 1.4.363.0), license **Apache-2.0 OR MIT**
(the SPDX line at the top of each file).

- `include/vulkan/`: `vulkan.h`, `vk_platform.h`, `vulkan_core.h` and the platform headers the
  renderer may use (`vulkan_win32.h`; `vulkan_wayland.h`, `vulkan_xlib.h`, `vulkan_xcb.h` for a native
  Linux build later);
- `include/vk_video/`: the video codec headers `vulkan_core.h` includes.

Only headers: `runtime/gpu/vulkan_loader.cpp` loads the system's Vulkan loader (`vulkan-1.dll`) at
run time, so nothing links against a Vulkan library and the game still starts (on Direct3D 12)
where no Vulkan driver is installed.
