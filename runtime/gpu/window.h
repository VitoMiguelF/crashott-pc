#pragma once

#include <cstdint>

namespace settings { enum class WindowMode : uint8_t; }

// The game window: created and pumped on its own thread so guest threads never have to.
// Closing it ends the process. F12 requests a screenshot, F1 toggles the overlay and F2 marks the
// current frame (overlay.h).
namespace gpu::window
{
    // Returns the HWND (as void*) or null.
    void* Create(uint32_t clientWidth, uint32_t clientHeight, const wchar_t* title);
    void* Handle();  // the HWND (as void*), null before Create
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
}
