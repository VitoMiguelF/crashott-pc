// The game window on SDL3 (Linux / Steam Deck, later Android): the same behaviour as
// window_win32.cpp. Keys reach input::pc as VK_* codes (the [controls] format), mapped from SDL
// scancodes by position (a US layout's VK for each key), so bindings and the keyboard prompts
// work unchanged.
#include "window.h"
#include "vulkan_loader.h"
#include "../diagnostics.h"
#include "../dev/dev_console.h"
#include "../kernel/hid_pads.h"
#include "../kernel/input.h"
#include "../kernel/pc_input.h"
#include "../kernel/xinput_backend.h"
#include "../platform/keys.h"
#include "../platform/keys_sdl.h"
#include "../platform/platform.h"
#include "../platform/text.h"
#include "../settings/settings.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace gpu::window
{
    namespace
    {
        std::atomic<SDL_Window*> g_window;
        std::atomic<uint32_t> g_clientWidth;   // pixels (the swap chain)
        std::atomic<uint32_t> g_clientHeight;
        std::atomic<int> g_pointWidth;         // window coordinates (the mouse)
        std::atomic<int> g_pointHeight;
        std::atomic<bool> g_focused;
        std::atomic<bool> g_screenshotRequested;
        std::atomic<uint32_t> g_overlayToggles;
        std::atomic<uint32_t> g_markRequests;
        std::atomic<uint32_t> g_recordToggles;
        std::atomic<uint32_t> g_lightingToggles;
        std::atomic<settings::WindowMode> g_mode{ settings::WindowMode::Windowed };
        uint32_t g_setModeEvent;  // SDL_RegisterEvents: SetMode, PostKey, PostChar from another thread
        constexpr int32_t SET_MODE = 0, POST_KEY = 1, POST_CHAR = 2;
        SDL_Rect g_windowedRect{};  // the framed window's position before leaving windowed mode
        std::mutex g_placementMutex;
        input::pc::CursorMode g_cursorMode = input::pc::CursorMode::Visible;
        bool g_textInput = false;
        uint32_t g_lastScan = 0;  // the set-1 scan code of the last key down, for the next text event
        float g_rawX = 0, g_rawY = 0;  // fractional mouse motion carried to the next event

        uint32_t VirtualKey(SDL_Scancode scan)
        {
            return plat::VirtualKeyFromScancode(scan);
        }

        // The WM_KEYDOWN-style lParam the developer console reads: bit 30 = auto-repeat, bits
        // 16-23 = the set-1 scan code (only the console's toggle key, the one left of 1, matters).
        intptr_t KeyParam(SDL_Scancode scan, bool repeat)
        {
            uint32_t set1 = scan == SDL_SCANCODE_GRAVE ? 0x29 : 0;
            return intptr_t((set1 << 16) | (repeat ? 1u << 30 : 0u));
        }

        void UpdateSizes(SDL_Window* window)
        {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            g_clientWidth = uint32_t(std::max(w, 0));
            g_clientHeight = uint32_t(std::max(h, 0));
            SDL_GetWindowSize(window, &w, &h);
            g_pointWidth = w;
            g_pointHeight = h;
        }

        void ApplyMode(SDL_Window* window, settings::WindowMode mode)
        {
            bool full = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
            if (mode == settings::WindowMode::Windowed)
            {
                if (full)
                    SDL_SetWindowFullscreen(window, false);
                return;
            }
            if (!full)
            {
                std::lock_guard lock(g_placementMutex);
                SDL_GetWindowPosition(window, &g_windowedRect.x, &g_windowedRect.y);
                SDL_GetWindowSize(window, &g_windowedRect.w, &g_windowedRect.h);
            }
            // Borderless and fullscreen are both the desktop-sized fullscreen window here; the
            // renderer's exclusive fullscreen is a Windows-only extension.
            SDL_SetWindowFullscreenMode(window, nullptr);
            SDL_SetWindowFullscreen(window, true);
        }

        // Shows / hides the cursor and keeps it in the window while the mouse turns the camera.
        void UpdateCursor(SDL_Window* window)
        {
            input::pc::CursorMode mode = input::pc::WantedCursor();
            if (!g_focused && mode == input::pc::CursorMode::Captured)
                mode = input::pc::CursorMode::Hidden;
            if (mode != g_cursorMode)
            {
                SDL_SetWindowRelativeMouseMode(window, mode == input::pc::CursorMode::Captured);
                if (mode == input::pc::CursorMode::Visible)
                    SDL_ShowCursor();
                else
                    SDL_HideCursor();
                g_cursorMode = mode;
            }
            // Text events only while the developer console types (the Deck shows its keyboard for them).
            bool text = dev::console::IsOpen();
            if (text != g_textInput)
            {
                if (text)
                    SDL_StartTextInput(window);
                else
                    SDL_StopTextInput(window);
                g_textInput = text;
            }
        }

        void OnKeyDown(SDL_Window* window, const SDL_KeyboardEvent& key)
        {
            uint32_t vk = VirtualKey(key.scancode);
            g_lastScan = key.scancode == SDL_SCANCODE_GRAVE ? 0x29 : 0;
            // The developer console (dev/dev_console.h) takes the keyboard while it is open.
            if (vk && dev::console::OnKeyDown(vk, KeyParam(key.scancode, key.repeat)))
                return;
            if (!vk)
                return;
            if (!key.repeat)
                input::pc::OnKey(vk, true);
            if (vk == VK_F12)
                g_screenshotRequested = true;
            if (key.repeat)
                return;
            if (vk == VK_RETURN && (key.mod & SDL_KMOD_ALT))
                SetMode(g_mode == settings::WindowMode::Windowed ? settings::WindowMode::Borderless : settings::WindowMode::Windowed);
            else if (vk == VK_F1)
                g_overlayToggles++;
            else if (vk == VK_F2)
                g_markRequests++;
            else if (vk == VK_F5)
                g_recordToggles++;
            else if (vk == VK_F8)
                g_lightingToggles++;
            (void)window;
        }

        void OnEvent(SDL_Window* window, const SDL_Event& event)
        {
            switch (event.type)
            {
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                g_focused = true;
                input::pc::OnFocus(true);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                g_focused = false;
                input::pc::OnFocus(false);
                UpdateCursor(window);
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED:
                UpdateSizes(window);
                break;
            case SDL_EVENT_MOUSE_MOTION:
            {
                input::pc::OnCursor(int(event.motion.x), int(event.motion.y), g_pointWidth, g_pointHeight);
                g_rawX += event.motion.xrel;
                g_rawY += event.motion.yrel;
                int dx = int(std::trunc(g_rawX)), dy = int(std::trunc(g_rawY));
                g_rawX -= float(dx), g_rawY -= float(dy);
                if (dx || dy)
                    input::pc::OnRawMouse(dx, dy);
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            {
                bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                uint32_t key = 0;
                switch (event.button.button)
                {
                case SDL_BUTTON_LEFT: key = VK_LBUTTON; break;
                case SDL_BUTTON_RIGHT: key = VK_RBUTTON; break;
                case SDL_BUTTON_MIDDLE: key = VK_MBUTTON; break;
                case SDL_BUTTON_X1: key = VK_XBUTTON1; break;
                case SDL_BUTTON_X2: key = VK_XBUTTON2; break;
                }
                input::pc::OnCursor(int(event.button.x), int(event.button.y), g_pointWidth, g_pointHeight);
                if (key && (!down || !dev::console::IsOpen()))  // the developer console has the mouse
                    input::pc::OnKey(key, down);
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
            {
                int notches = int(event.wheel.integer_y);
                if (notches && !dev::console::OnWheel(notches))
                    input::pc::OnWheel(notches);
                break;
            }
            case SDL_EVENT_KEY_DOWN:
                OnKeyDown(window, event.key);
                break;
            case SDL_EVENT_KEY_UP:
                if (uint32_t vk = VirtualKey(event.key.scancode))
                    input::pc::OnKey(vk, false);
                break;
            case SDL_EVENT_TEXT_INPUT:
                for (wchar_t c : plat::WidenUtf8(event.text.text))
                    dev::console::OnChar(c, intptr_t(g_lastScan << 16), false);
                g_lastScan = 0;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
                input::xinput::DevicesChanged();
                input::hid::Rescan();
                break;
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                input::StopRecording();  // a recording in progress keeps everything up to here
                Log("[gpu] window closed, exiting");
                plat::ExitProcess(0);
                break;
            default:
                if (event.type == g_setModeEvent && event.user.code == SET_MODE)
                    ApplyMode(window, settings::WindowMode(uintptr_t(event.user.data1)));
                else if (event.type == g_setModeEvent && event.user.code == POST_KEY)
                {
                    // A scripted key: the console first, like OnKeyDown, then a press and release.
                    uint32_t vk = uint32_t(uintptr_t(event.user.data1));
                    intptr_t lParam = intptr_t(uintptr_t(event.user.data2));
                    if (!dev::console::OnKeyDown(vk, lParam) && vk)
                    {
                        input::pc::OnKey(vk, true);
                        input::pc::OnKey(vk, false);
                    }
                }
                else if (event.type == g_setModeEvent && event.user.code == POST_CHAR)
                    dev::console::OnChar(wchar_t(uintptr_t(event.user.data1)), 0, false);
                break;
            }
        }
    }

    void* Handle() { return g_window.load(); }

    bool ControlHeld()
    {
        return (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
    }

    std::string ClipboardText()
    {
        std::string text;
        if (char* clipboard = SDL_GetClipboardText())
        {
            text = clipboard;
            SDL_free(clipboard);
        }
        return text;
    }

    void FlushDeadKey() {}

    void PostKey(uint32_t vk, uint32_t scan, bool extended)
    {
        if (!g_window.load() || !g_setModeEvent)
            return;
        if (!vk && scan == 0x29)
            vk = VK_OEM_3;  // the console's toggle key
        SDL_Event event{};
        event.type = g_setModeEvent;
        event.user.code = POST_KEY;
        event.user.data1 = reinterpret_cast<void*>(uintptr_t(vk));
        event.user.data2 = reinterpret_cast<void*>(uintptr_t((scan << 16) | (extended ? 1u << 24 : 0u)));
        SDL_PushEvent(&event);
    }

    void PostChar(wchar_t c)
    {
        if (!g_window.load() || !g_setModeEvent)
            return;
        SDL_Event event{};
        event.type = g_setModeEvent;
        event.user.code = POST_CHAR;
        event.user.data1 = reinterpret_cast<void*>(uintptr_t(c));
        SDL_PushEvent(&event);
    }

    bool IsFocused()
    {
        return g_window.load() && g_focused;
    }

    std::vector<const char*> VulkanSurfaceExtensions()
    {
        Uint32 count = 0;
        const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
        if (!names)
            return {};
        return std::vector<const char*>(names, names + count);
    }

    bool CreateVulkanSurface(void* instance, void* surface)
    {
        SDL_Window* window = g_window.load();
        if (!window || !SDL_Vulkan_CreateSurface(window, VkInstance(instance), nullptr, static_cast<VkSurfaceKHR*>(surface)))
        {
            Log("[gpu] SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
            return false;
        }
        return true;
    }

    void* Create(uint32_t clientWidth, uint32_t clientHeight, const wchar_t* title)
    {
        std::mutex mutex;
        std::condition_variable ready;
        bool done = false;

        // The window lives on its own thread like on Windows, which SDL allows on X11 and Wayland
        // (all video calls stay on that one thread). Android needs it on the main thread instead.
        std::thread([&, clientWidth, clientHeight, title] {
            SDL_Window* window = nullptr;
            if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
                Log("[gpu] SDL_Init failed: %s", SDL_GetError());
            else
            {
                g_setModeEvent = SDL_RegisterEvents(1);
                // The requested size is in 96-DPI units: scale by the display's content scale.
                float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
                if (scale <= 0.0f)
                    scale = 1.0f;
                int width = int(float(clientWidth) * scale), height = int(float(clientHeight) * scale);
                bool forced = false;
                if (const char* size = getenv("CRASH_WINDOW_SIZE"))
                {
                    int w, h;
                    if (sscanf(size, "%dx%d", &w, &h) == 2 && w >= 160 && h >= 120)
                        width = w, height = h, forced = true;
                }
                // A wide picture can be wider than the screen: fit it in the usable area, same aspect.
                SDL_Rect usable;
                if (!forced && SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable))
                {
                    float fit = std::min({ 1.0f, float(usable.w) * 0.95f / float(width), float(usable.h) * 0.9f / float(height) });
                    if (fit < 1.0f && fit > 0.1f)
                        width = int(float(width) * fit), height = int(float(height) * fit);
                }
                std::string fullTitle = plat::NarrowUtf8(title) + InstanceTitleSuffix();
                Log("[gpu] window title: %s", fullTitle.c_str());
                window = SDL_CreateWindow(fullTitle.c_str(), width, height,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
                if (!window)
                    Log("[gpu] SDL_CreateWindow failed: %s", SDL_GetError());
            }
            if (window)
            {
                // Wayland with scaling: the size above was pixels, the window takes points.
                float density = SDL_GetWindowPixelDensity(window);
                if (density > 1.0f)
                {
                    int w, h;
                    SDL_GetWindowSize(window, &w, &h);
                    SDL_SetWindowSize(window, int(float(w) / density), int(float(h) / density));
                }
                // After a restart from the settings screen: where the previous process had it.
                if (const char* placement = getenv("CRASH_RESTART_WINDOW"))
                {
                    int px, py, pw, ph;
                    if (sscanf(placement, "%d,%d,%d,%d", &px, &py, &pw, &ph) == 4 && pw >= 160 && ph >= 120)
                    {
                        SDL_SetWindowPosition(window, px, py);
                        SDL_SetWindowSize(window, pw, ph);
                    }
                }
                SDL_ShowWindow(window);
                UpdateSizes(window);
                SDL_GetWindowPosition(window, &g_windowedRect.x, &g_windowedRect.y);
                SDL_GetWindowSize(window, &g_windowedRect.w, &g_windowedRect.h);
                g_focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
                Log("[gpu] window: content scale %.2f, density %.2f, client %ux%u", SDL_GetWindowDisplayScale(window), density,
                    g_clientWidth.load(), g_clientHeight.load());
                SDL_StopTextInput(window);
                if (g_mode != settings::WindowMode::Windowed)
                    ApplyMode(window, g_mode);
            }
            {
                std::lock_guard lock(mutex);
                g_window = window;
                done = true;
            }
            ready.notify_one();
            if (!window)
                return;

            uint64_t nextCursor = 0;
            for (;;)
            {
                SDL_Event event;
                if (SDL_WaitEventTimeout(&event, 100))
                {
                    do
                        OnEvent(window, event);
                    while (SDL_PollEvent(&event));
                }
                uint64_t now = SDL_GetTicks();
                if (now >= nextCursor)
                {
                    UpdateCursor(window);
                    nextCursor = now + 100;
                }
            }
        }).detach();

        std::unique_lock lock(mutex);
        ready.wait(lock, [&] { return done; });
        return g_window.load();
    }

    void GetClientSize(uint32_t& width, uint32_t& height)
    {
        width = g_clientWidth;
        height = g_clientHeight;
    }

    bool ConsumeScreenshotRequest()
    {
        return g_screenshotRequested.exchange(false);
    }

    uint32_t ConsumeOverlayToggles()
    {
        return g_overlayToggles.exchange(0);
    }

    uint32_t ConsumeMarkRequests()
    {
        return g_markRequests.exchange(0);
    }

    uint32_t ConsumeRecordToggles()
    {
        return g_recordToggles.exchange(0);
    }

    uint32_t ConsumeLightingToggles()
    {
        return g_lightingToggles.exchange(0);
    }

    void SetMode(settings::WindowMode mode)
    {
        g_mode = mode;
        if (g_window.load() && g_setModeEvent)
        {
            SDL_Event event{};
            event.type = g_setModeEvent;
            event.user.code = SET_MODE;
            event.user.data1 = reinterpret_cast<void*>(uintptr_t(mode));
            SDL_PushEvent(&event);
        }
    }

    settings::WindowMode Mode()
    {
        return g_mode;
    }

    bool Placement(int& x, int& y, int& width, int& height)
    {
        SDL_Window* window = g_window.load();
        if (!window || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED))
            return false;
        std::lock_guard lock(g_placementMutex);
        SDL_Rect r = g_windowedRect;
        if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN))
        {
            SDL_GetWindowPosition(window, &r.x, &r.y);
            SDL_GetWindowSize(window, &r.w, &r.h);
        }
        x = r.x, y = r.y, width = r.w, height = r.h;
        return width > 0 && height > 0;
    }
}
