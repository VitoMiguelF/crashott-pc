#pragma once

#include <functional>

struct SDL_Window;

// Android only (android_main_android.cpp, docs/internals/android_port.md). SDL3's Java glue calls
// SDL_main on its own native thread, and SDL wants the window and its events there: SDL_main
// keeps that thread for the window (gpu/window_sdl.cpp) and runs the game on another one.
namespace plat::android
{
    // Runs `job` on the SDL_main thread (the window's loop never returns, so it is the last one).
    void RunOnMainThread(std::function<void()> job);

    // The Vulkan loader to use (a dlopen handle, opened once): the system's libvulkan.so, or on an
    // Adreno whose driver is older than 1.3 a copy of it that libadrenotools points at Mesa's
    // Turnip (libvulkan_freedreno.so in the APK). files/vulkan_driver.txt can force "system" or
    // "turnip" ("auto" otherwise). Surfaces must come from this loader too, not from SDL's.
    void* OpenVulkanLibrary();

    // vkCreateAndroidSurfaceKHR on the window's ANativeWindow, through `getInstanceProcAddr` (a
    // PFN_vkGetInstanceProcAddr of OpenVulkanLibrary's loader); `surface` is a VkSurfaceKHR*.
    bool CreateVulkanSurface(SDL_Window* window, void* instance, void* getInstanceProcAddr, void* surface);
}
