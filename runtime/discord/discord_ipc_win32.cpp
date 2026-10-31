// Discord IPC on Windows: the named pipe \\.\pipe\discord-ipc-N (discord_ipc.h).

#include "discord_ipc.h"

#include <windows.h>

#include <cstdio>

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
