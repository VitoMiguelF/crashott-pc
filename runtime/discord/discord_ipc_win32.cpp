// Discord IPC on Windows: the named pipe \\.\pipe\discord-ipc-N (discord_ipc.h).

#include "discord_ipc.h"
#include "../diagnostics.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace discord::ipc
{
    namespace
    {
        HANDLE g_pipe = INVALID_HANDLE_VALUE;
    }

    bool Open()
    {
        Close();
        for (int i = 0; i < 10; i++)
        {
            wchar_t name[64];
            swprintf(name, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
            HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE)
            {
                g_pipe = pipe;
                return true;
            }
            if (GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeW(name, 1000))
            {
                pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (pipe != INVALID_HANDLE_VALUE)
                {
                    g_pipe = pipe;
                    return true;
                }
            }
        }
        return false;
    }

    void Close()
    {
        if (g_pipe != INVALID_HANDLE_VALUE)
            CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }

    bool IsOpen() { return g_pipe != INVALID_HANDLE_VALUE; }

    bool Write(const void* data, size_t size)
    {
        const char* p = static_cast<const char*>(data);
        while (IsOpen() && size)
        {
            DWORD written = 0;
            if (!WriteFile(g_pipe, p, DWORD(size), &written, nullptr) || !written)
                break;
            p += written, size -= written;
        }
        if (size)
            Close();
        return !size;
    }

    long Available()
    {
        DWORD available = 0;
        if (!IsOpen() || !PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &available, nullptr))
        {
            Close();
            return -1;
        }
        return long(available);
    }

    void RegisterLaunch(const std::string& applicationId)
    {
        wchar_t exe[MAX_PATH];
        DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return;
        std::wstring id(applicationId.begin(), applicationId.end());  // digits
        std::wstring key = L"Software\\Classes\\discord-" + id;
        std::wstring command = L"\"" + std::wstring(exe) + L"\"";
        // Already there, pointing at this exe: nothing to write.
        wchar_t current[MAX_PATH + 8];
        DWORD size = sizeof(current);
        if (RegGetValueW(HKEY_CURRENT_USER, (key + L"\\shell\\open\\command").c_str(), nullptr, RRF_RT_REG_SZ, nullptr, current, &size) ==
                ERROR_SUCCESS &&
            command == current)
            return;
        auto set = [](const std::wstring& path, const wchar_t* name, const std::wstring& value) {
            HKEY handle;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &handle, nullptr) != ERROR_SUCCESS)
                return false;
            LONG result = RegSetValueExW(handle, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), DWORD((value.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(handle);
            return result == ERROR_SUCCESS;
        };
        bool ok = set(key, nullptr, L"URL:Run game " + id + L" protocol") && set(key, L"URL Protocol", L"") &&
                  set(key + L"\\DefaultIcon", nullptr, exe) && set(key + L"\\shell\\open\\command", nullptr, command);
        Log("[discord] %s the discord-%s protocol (Discord starts this exe for a friend's Join)", ok ? "registered" : "could not register",
            applicationId.c_str());
    }

    bool Read(void* data, size_t size)
    {
        char* p = static_cast<char*>(data);
        while (IsOpen() && size)
        {
            DWORD got = 0;
            if (!ReadFile(g_pipe, p, DWORD(size), &got, nullptr) || !got)
                break;
            p += got, size -= got;
        }
        if (size)
            Close();
        return !size;
    }
}
