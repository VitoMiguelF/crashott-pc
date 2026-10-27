#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace settings { enum class WindowMode : uint8_t; }

// The game window: created and pumped on its own thread so guest threads never have to.
// Closing it ends the process. F12 requests a screenshot, F1 toggles the overlay and F2 marks the
// current frame (overlay.h).
namespace gpu::window
{
    // Returns the native window (HWND / SDL_Window*, as void*) or null.
    void* Create(uint32_t clientWidth, uint32_t clientHeight, const wchar_t* title);
    void* Handle();  // the native window (HWND / SDL_Window*), null before Create
    bool IsFocused(); // the game window is the foreground window
    // The app in the background (Android: SDL_EVENT_WILL/DID_ENTER_BACKGROUND until
    // DID_ENTER_FOREGROUND; never on a PC): frames aren't shown (the window's Vulkan surface goes
    // and a new one is made on return) and the game is held at its next VdSwap, the audio and
    // vblank threads with it (gpu.h, HoldWhileSuspended).
    bool Suspended();
    void WaitWhileSuspended();  // returns at once when not suspended

    // Vulkan presentation, done by the window backend: the instance extensions the window's
    // surface needs (VK_KHR_surface + the platform's), and that surface for `instance` (a
    // VkInstance), written to `surface` (a VkSurfaceKHR*).
    std::vector<const char*> VulkanSurfaceExtensions();
    bool CreateVulkanSurface(void* instance, void* surface);
    void GetClientSize(uint32_t& width, uint32_t& height);
    bool ConsumeScreenshotRequest();
    uint32_t ConsumeOverlayToggles(); // F1 presses since the last call (each one: next overlay mode)
    uint32_t ConsumeMarkRequests();   // F2 presses since the last call
    uint32_t ConsumeRecordToggles();  // F5 presses since the last call (start/stop recording)
    uint32_t ConsumeLightingToggles(); // F8 presses since the last call (lighting remaster on/off, lighting.h)

    // Windowed (the normal frame), borderless or fullscreen (a borderless window covering the
    // monitor; the renderer also switches the swap chain to exclusive fullscreen). Applied on the
    // window thread; the client size follows through WM_SIZE.
    void SetMode(settings::WindowMode mode);
    settings::WindowMode Mode();

    // The framed window's rectangle (screen pixels; the one it goes back to when it leaves
    // borderless/fullscreen), for a restart (settings.h, Restart). The next process reads it from
    // CRASH_RESTART_WINDOW="x,y,width,height" when creating its window.
    bool Placement(int& x, int& y, int& width, int& height);

    // Keyboard bits for the developer console (on the window thread, inside its key handlers):
    // Ctrl held, the clipboard's text (UTF-8), and dropping a pending dead key (US-International `).
    bool ControlHeld();
    std::string ClipboardText();
    void FlushDeadKey();
    // Synthetic typing (console scripts), handled like a real keyboard on the window thread. `vk`
    // 0 = the key at set-1 scan code `scan` in the current layout.
    void PostKey(uint32_t vk, uint32_t scan, bool extended);
    void PostChar(wchar_t c);

    // " | run <folder> | session <id> | PID n" (UTF-8) for the title: who started this instance
    // (window_title.cpp, shared by the backends).
    std::string InstanceTitleSuffix();
}
