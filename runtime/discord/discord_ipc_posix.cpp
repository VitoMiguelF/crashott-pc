// Discord IPC on Linux (and the Steam Deck): the Unix socket discord-ipc-N (discord_ipc.h). Android has
// no Discord client with IPC: Open always fails there (discord_presence.cpp doesn't start at all).

#include "discord_ipc.h"

#if defined(__ANDROID__)

namespace discord::ipc
{
    bool Open() { return false; }
    void Close() {}
    bool IsOpen() { return false; }
    bool Write(const void*, size_t) { return false; }
    long Available() { return -1; }
    bool Read(void*, size_t) { return false; }
}

#else

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace discord::ipc
{
    namespace
    {
        int g_socket = -1;

        bool Connect(const std::string& path)
        {
            sockaddr_un address{};
            if (path.size() >= sizeof(address.sun_path))
                return false;
            int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (s < 0)
                return false;
            address.sun_family = AF_UNIX;
            memcpy(address.sun_path, path.c_str(), path.size() + 1);
            if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            {
                close(s);
                return false;
            }
            g_socket = s;
            return true;
        }
    }

    bool Open()
    {
        Close();
        std::string base = "/tmp";
        for (const char* variable : { "XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP" })
            if (const char* value = getenv(variable); value && *value)
            {
                base = value;
                break;
            }
        // The native client, then the Flatpak and Snap packages' folders.
        for (const char* folder : { "", "/app/com.discordapp.Discord", "/snap.discord" })
            for (int i = 0; i < 10; i++)
                if (Connect(base + folder + "/discord-ipc-" + std::to_string(i)))
                    return true;
        return false;
    }

    void Close()
    {
        if (g_socket >= 0)
            close(g_socket);
        g_socket = -1;
    }

    bool IsOpen() { return g_socket >= 0; }

    bool Write(const void* data, size_t size)
    {
        const char* p = static_cast<const char*>(data);
        while (IsOpen() && size)
        {
            ssize_t sent = send(g_socket, p, size, MSG_NOSIGNAL);
            if (sent < 0 && errno == EINTR)
                continue;
            if (sent <= 0)
                break;
            p += sent, size -= size_t(sent);
        }
        if (size)
            Close();
        return !size;
    }

    long Available()
    {
        if (!IsOpen())
            return -1;
        pollfd fd{ g_socket, POLLIN, 0 };
        if (poll(&fd, 1, 0) < 0)
            return 0;
        if (!(fd.revents & (POLLIN | POLLHUP | POLLERR)))
            return 0;
        int available = 0;
        if (ioctl(g_socket, FIONREAD, &available) != 0 || available <= 0)
        {
            Close();  // readable with nothing to read: the other end closed
            return -1;
        }
        return available;
    }

    bool Read(void* data, size_t size)
    {
        char* p = static_cast<char*>(data);
        while (IsOpen() && size)
        {
            ssize_t got = recv(g_socket, p, size, 0);
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                break;
            p += got, size -= size_t(got);
        }
        if (size)
            Close();
        return !size;
    }
}

#endif
