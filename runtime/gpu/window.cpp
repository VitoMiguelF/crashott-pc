#include "window.h"
#include "../diagnostics.h"
#include "../dev/dev_console.h"
#include "../kernel/hid_pads.h"
#include "../kernel/input.h"
#include "../kernel/pc_input.h"
#include "../settings/settings.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace gpu::window
{
    namespace
    {
        int GET_X_LPARAM_(LPARAM lParam) { return int(short(LOWORD(lParam))); }
        int GET_Y_LPARAM_(LPARAM lParam) { return int(short(HIWORD(lParam))); }

        std::atomic<HWND> g_window;
        std::atomic<uint32_t> g_clientWidth;
        std::atomic<uint32_t> g_clientHeight;
        std::atomic<bool> g_screenshotRequested;
        // CRASH_WINDOW_SIZE=WxH (tests, captures): the client in screen pixels, even past the monitor.
        bool g_forcedSize = false;
        std::atomic<uint32_t> g_overlayToggles;
        std::atomic<uint32_t> g_markRequests;
        std::atomic<uint32_t> g_recordToggles;
        std::atomic<uint32_t> g_lightingToggles;
        std::atomic<settings::WindowMode> g_mode{ settings::WindowMode::Windowed };
        RECT g_windowedRect{};  // the framed window's position before leaving windowed mode
        constexpr UINT WM_APP_SET_MODE = WM_APP + 1;
        constexpr UINT_PTR CURSOR_TIMER = 1;
        bool g_cursorClipped = false;
        input::pc::CursorMode g_cursorMode = input::pc::CursorMode::Visible;
        constexpr DWORD WINDOWED_STYLE = WS_OVERLAPPEDWINDOW;

        // The window title says who started this instance: "<title> | <run folder or profile> |
        // session <id> | <tag> | PID n". The run folder comes from tools/run_game.py
        // (CRASH_SCREENSHOT_DIR = the run's folder, CRASH_PROFILE), the session from the Claude Code
        // session that launched it (CLAUDE_CODE_HOST_SESSION_ID, inherited), CRASH_WINDOW_TAG is free text.
        std::wstring InstanceTitle(const wchar_t* title)
        {
            std::string extra;
            auto add = [&](const std::string& part) {
                if (!part.empty())
                    extra += " | " + part;
            };
            std::string run;
            if (const char* dir = getenv("CRASH_SCREENSHOT_DIR"))
            {
                run = dir;
                while (!run.empty() && (run.back() == '\\' || run.back() == '/'))
                    run.pop_back();
                run = run.substr(run.find_last_of("\\/") + 1);
            }
            const char* profile = getenv("CRASH_PROFILE");
            if (!run.empty())
                add("run " + run);
            else if (profile && *profile)
                add(std::string("profile ") + profile);
            if (const char* session = getenv("CLAUDE_CODE_HOST_SESSION_ID"); session && *session)
            {
                std::string id = session;
                if (id.rfind("local_", 0) == 0)
                    id = id.substr(6);
                add("session " + id.substr(0, 8));
            }
            if (const char* tag = getenv("CRASH_WINDOW_TAG"))
                add(tag);
            add("PID " + std::to_string(GetCurrentProcessId()));
            std::wstring result = title;
            int length = MultiByteToWideChar(CP_UTF8, 0, extra.c_str(), int(extra.size()), nullptr, 0);
            std::wstring wide(size_t(length), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, extra.c_str(), int(extra.size()), wide.data(), length);
            return result + wide;
        }

        void ApplyMode(HWND hwnd, settings::WindowMode mode)
        {
            DWORD style = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE));
            bool framed = (style & WS_CAPTION) != 0;
            if (mode == settings::WindowMode::Windowed)
            {
                if (framed)
                    return;
                SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~WS_POPUP) | WINDOWED_STYLE);
                RECT& r = g_windowedRect;
                SetWindowPos(hwnd, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
                return;
            }
            if (framed)
                GetWindowRect(hwnd, &g_windowedRect);
            MONITORINFO monitor{ sizeof(monitor) };
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
            const RECT& r = monitor.rcMonitor;
            SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~WINDOWED_STYLE) | WS_POPUP | WS_VISIBLE);
            SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
        }

        // The key of a WM_KEY* message, with left and right Shift / Ctrl / Alt told apart.
        uint32_t KeyOf(WPARAM wParam, LPARAM lParam)
        {
            bool extended = (lParam & (1 << 24)) != 0;
            switch (wParam)
            {
            case VK_SHIFT: return MapVirtualKeyW((lParam >> 16) & 0xFF, MAPVK_VSC_TO_VK_EX);
            case VK_CONTROL: return extended ? VK_RCONTROL : VK_LCONTROL;
            case VK_MENU: return extended ? VK_RMENU : VK_LMENU;
            default: return uint32_t(wParam);
            }
        }

        // Shows / hides the cursor and keeps it inside the window while the mouse turns the camera.
        void UpdateCursor(HWND hwnd)
        {
            input::pc::CursorMode mode = input::pc::WantedCursor();
            if (mode == input::pc::CursorMode::Captured && GetForegroundWindow() == hwnd)
            {
                RECT client;
                GetClientRect(hwnd, &client);
                MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&client), 2);
                ClipCursor(&client);
                g_cursorClipped = true;
            }
            else if (g_cursorClipped)
            {
                ClipCursor(nullptr);
                g_cursorClipped = false;
            }
            if (mode != g_cursorMode)
            {
                g_cursorMode = mode;
                // Re-run WM_SETCURSOR now rather than at the next mouse move.
                POINT point;
                GetCursorPos(&point);
                SetCursorPos(point.x, point.y);
            }
        }

        LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            switch (message)
            {
            case WM_GETMINMAXINFO:
                if (g_forcedSize)
                {
                    reinterpret_cast<MINMAXINFO*>(lParam)->ptMaxTrackSize = { 16384, 16384 };
                    return 0;
                }
                break;
            case WM_CREATE:
            {
                // Raw mouse motion for the camera (pc_input.h): counts, not the clipped cursor.
                RAWINPUTDEVICE mouse{ 0x01, 0x02, 0, hwnd };
                RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
                SetTimer(hwnd, CURSOR_TIMER, 100, nullptr);
                return 0;
            }
            case WM_TIMER:
                if (wParam == CURSOR_TIMER)
                    UpdateCursor(hwnd);
                return 0;
            case WM_SETFOCUS:
                input::pc::OnFocus(true);
                return 0;
            case WM_KILLFOCUS:
                input::pc::OnFocus(false);
                UpdateCursor(hwnd);
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lParam) == HTCLIENT && g_cursorMode != input::pc::CursorMode::Visible)
                {
                    SetCursor(nullptr);
                    return TRUE;
                }
                return DefWindowProcW(hwnd, message, wParam, lParam);
            case WM_MOUSEACTIVATE:
                // The click that brings the window to the front does not reach the game (no attack).
                return LOWORD(lParam) == HTCLIENT ? MA_ACTIVATEANDEAT : MA_ACTIVATE;
            case WM_INPUT:
            {
                RAWINPUT raw;
                UINT size = sizeof(raw);
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
                    raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
                    input::pc::OnRawMouse(raw.data.mouse.lLastX, raw.data.mouse.lLastY);
                return DefWindowProcW(hwnd, message, wParam, lParam);
            }
            case WM_MOUSEMOVE:
                input::pc::OnCursor(GET_X_LPARAM_(lParam), GET_Y_LPARAM_(lParam), int(g_clientWidth.load()), int(g_clientHeight.load()));
                return 0;
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            {
                bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
                uint32_t key = message <= WM_LBUTTONUP ? VK_LBUTTON : message <= WM_RBUTTONUP ? VK_RBUTTON
                             : message <= WM_MBUTTONUP ? VK_MBUTTON : (HIWORD(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2);
                input::pc::OnCursor(GET_X_LPARAM_(lParam), GET_Y_LPARAM_(lParam), int(g_clientWidth.load()), int(g_clientHeight.load()));
                if (!down || !dev::console::IsOpen())  // the developer console has the mouse
                    input::pc::OnKey(key, down);
                if (down)
                    SetCapture(hwnd);
                else if (!(wParam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)))
                    ReleaseCapture();
                return message >= WM_XBUTTONDOWN ? TRUE : 0;
            }
            case WM_MOUSEWHEEL:
                if (dev::console::OnWheel(GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA))
                    return 0;
                input::pc::OnWheel(GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA);
                return 0;
            case WM_DEVICECHANGE:
                input::hid::Rescan();  // a pad may have arrived or left
                return TRUE;
            case WM_KEYUP:
                input::pc::OnKey(KeyOf(wParam, lParam), false);
                return 0;
            case WM_SYSKEYUP:
                input::pc::OnKey(KeyOf(wParam, lParam), false);
                // Alt or F10 alone would open the (absent) window menu and stall the message loop.
                if (wParam == VK_MENU || wParam == VK_F10)
                    return 0;
                return DefWindowProcW(hwnd, message, wParam, lParam);
            case WM_SYSCOMMAND:
                if ((wParam & 0xFFF0) == SC_KEYMENU)
                    return 0;
                return DefWindowProcW(hwnd, message, wParam, lParam);
            case WM_SIZE:
                g_clientWidth = LOWORD(lParam);
                g_clientHeight = HIWORD(lParam);
                return 0;
            case WM_DPICHANGED:
            {
                // Moved to a monitor with another scale: take the size Windows suggests for it.
                const RECT& r = *reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_CHAR:
            case WM_DEADCHAR:
                dev::console::OnChar(wchar_t(wParam), lParam, message == WM_DEADCHAR);
                return 0;
            case WM_KEYDOWN:
                // The developer console (dev/dev_console.h) takes the keyboard while it is open.
                if (dev::console::OnKeyDown(wParam, lParam))
                    return 0;
                if (!(lParam & (1 << 30)))
                    input::pc::OnKey(KeyOf(wParam, lParam), true);
                if (wParam == VK_F12)
                    g_screenshotRequested = true;
                // F1/F2/F5/F8 once per press (bit 30: the key was already down, an auto-repeat).
                if (!(lParam & (1 << 30)))
                {
                    if (wParam == VK_F1)
                        g_overlayToggles++;
                    else if (wParam == VK_F2)
                        g_markRequests++;
                    else if (wParam == VK_F5)
                        g_recordToggles++;
                    else if (wParam == VK_F8)
                        g_lightingToggles++;
                }
                return 0;
            case WM_APP_SET_MODE:
                ApplyMode(hwnd, settings::WindowMode(wParam));
                return 0;
            case WM_SYSKEYDOWN:
                if (wParam == VK_F10 && dev::console::OnKeyDown(wParam, lParam))
                    return 0;
                if (!(lParam & (1 << 30)))
                    input::pc::OnKey(KeyOf(wParam, lParam), true);
                if (wParam == VK_MENU || wParam == VK_F10)
                    return 0;
                // Alt+Enter: windowed <-> borderless.
                if (wParam == VK_RETURN && (lParam & (1 << 29)) && !(lParam & (1 << 30)))
                {
                    SetMode(g_mode == settings::WindowMode::Windowed ? settings::WindowMode::Borderless : settings::WindowMode::Windowed);
                    return 0;
                }
                return DefWindowProcW(hwnd, message, wParam, lParam);
            case WM_CLOSE:
                input::StopRecording();  // a recording in progress keeps everything up to here
                Log("[gpu] window closed, exiting");
                ExitProcess(0);
            default:
                return DefWindowProcW(hwnd, message, wParam, lParam);
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);  // the cases that break (WM_GETMINMAXINFO unforced)
        }
    }

    void* Handle() { return g_window.load(); }

    void* Create(uint32_t clientWidth, uint32_t clientHeight, const wchar_t* title)
    {
        std::mutex mutex;
        std::condition_variable ready;
        bool done = false;

        std::thread([&, clientWidth, clientHeight, title] {
            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = WindowProc;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
            windowClass.hbrBackground = HBRUSH(GetStockObject(BLACK_BRUSH));
            windowClass.lpszClassName = L"CrashRecompWindow";
            RegisterClassExW(&windowClass);

            // Per-monitor DPI aware: the swap chain gets the screen's real pixels. A DPI-unaware
            // window on a 4K screen at 200% scaling renders at 1920x1080 and Windows stretches it
            // 2x (blurry). The requested client size is in 96-DPI units, so it is scaled here.
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            UINT dpi = GetDpiForSystem();
            uint32_t scaledWidth = uint32_t(MulDiv(int(clientWidth), int(dpi), 96));
            uint32_t scaledHeight = uint32_t(MulDiv(int(clientHeight), int(dpi), 96));
            if (const char* forced = getenv("CRASH_WINDOW_SIZE"))
            {
                int forcedWidth, forcedHeight;
                if (sscanf(forced, "%dx%d", &forcedWidth, &forcedHeight) == 2 && forcedWidth >= 160 && forcedHeight >= 120)
                {
                    scaledWidth = uint32_t(forcedWidth), scaledHeight = uint32_t(forcedHeight);
                    g_forcedSize = true;
                }
            }
            DWORD style = WS_OVERLAPPEDWINDOW;
            // A wide picture (21:9, 32:9 at 720 lines and a high DPI) can be wider than the monitor:
            // shrink the window into the primary monitor's work area, same aspect, centred.
            MONITORINFO workArea{ sizeof(workArea) };
            bool fitted = false;
            if (!g_forcedSize && GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &workArea))
            {
                RECT frame{ 0, 0, 0, 0 };
                AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
                const RECT& work = workArea.rcWork;
                float room = float(work.right - work.left - (frame.right - frame.left)) * 0.95f;
                float roomHeight = float(work.bottom - work.top - (frame.bottom - frame.top)) * 0.95f;
                float fit = std::min({ 1.0f, room / float(scaledWidth), roomHeight / float(scaledHeight) });
                if (fit < 1.0f && fit > 0.1f)
                {
                    scaledWidth = uint32_t(float(scaledWidth) * fit);
                    scaledHeight = uint32_t(float(scaledHeight) * fit);
                    fitted = true;
                }
            }
            RECT rect{ 0, 0, LONG(scaledWidth), LONG(scaledHeight) };
            AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);
            int x = CW_USEDEFAULT, y = CW_USEDEFAULT, width = rect.right - rect.left, height = rect.bottom - rect.top;
            if (fitted)
            {
                const RECT& work = workArea.rcWork;
                x = work.left + (work.right - work.left - width) / 2;
                y = work.top + (work.bottom - work.top - height) / 2;
            }
            // After a restart from the settings screen: where the previous process had it.
            if (const char* placement = getenv("CRASH_RESTART_WINDOW"))
            {
                int px, py, pw, ph;
                if (sscanf(placement, "%d,%d,%d,%d", &px, &py, &pw, &ph) == 4 && pw >= 160 && ph >= 120)
                {
                    RECT wanted{ px, py, px + pw, py + ph };
                    if (MonitorFromRect(&wanted, MONITOR_DEFAULTTONULL))  // still on a screen
                        x = px, y = py, width = pw, height = ph;
                }
            }
            std::wstring fullTitle = InstanceTitle(title);
            Log("[gpu] window title: %ls", fullTitle.c_str());
            HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, fullTitle.c_str(), style, x, y, width, height, nullptr, nullptr,
                                        windowClass.hInstance, nullptr);
            if (hwnd)
            {
                RECT client{};
                GetClientRect(hwnd, &client);
                g_clientWidth = uint32_t(client.right - client.left);
                g_clientHeight = uint32_t(client.bottom - client.top);
                Log("[gpu] window: %u dpi (%u%% scaling), client %ux%u", dpi, dpi * 100 / 96, g_clientWidth.load(), g_clientHeight.load());
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                GetWindowRect(hwnd, &g_windowedRect);
                if (g_mode != settings::WindowMode::Windowed)
                    ApplyMode(hwnd, g_mode);
            }
            {
                std::lock_guard lock(mutex);
                g_window = hwnd;
                done = true;
            }
            ready.notify_one();
            if (!hwnd)
                return;

            MSG message;
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
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
        if (HWND hwnd = g_window.load())
            PostMessageW(hwnd, WM_APP_SET_MODE, WPARAM(mode), 0);
    }

    settings::WindowMode Mode()
    {
        return g_mode;
    }

    bool Placement(int& x, int& y, int& width, int& height)
    {
        HWND hwnd = g_window.load();
        if (!hwnd)
            return false;
        RECT r = g_windowedRect;
        if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION)
        {
            WINDOWPLACEMENT placement{ sizeof(placement) };
            if (!GetWindowPlacement(hwnd, &placement) || placement.showCmd == SW_SHOWMINIMIZED)
                return false;
            ::GetWindowRect(hwnd, &r);
            if (placement.showCmd == SW_SHOWMAXIMIZED)
                r = placement.rcNormalPosition;  // workspace coordinates; close enough for a restart
        }
        x = r.left, y = r.top, width = r.right - r.left, height = r.bottom - r.top;
        return width > 0 && height > 0;
    }
}
