#pragma once

// The local connection to the Discord client (discord_presence.cpp): a byte stream to the named pipe
// \\.\pipe\discord-ipc-N on Windows (discord_ipc_win32.cpp), the Unix socket discord-ipc-N in
// $XDG_RUNTIME_DIR (or TMPDIR / /tmp, also the Flatpak and Snap folders) elsewhere (discord_ipc_posix.cpp).
// N = 0..9: the first one that answers. Only the presence thread uses it.

#include <cstddef>
#include <cstdint>

namespace discord::ipc
{
    bool Open();   // false: no Discord client running (nothing logged)
    void Close();
    bool IsOpen();
    // The whole buffer, or false (the connection is closed then).
    bool Write(const void* data, size_t size);
    // Bytes available now without waiting (0 = none yet); -1 = the connection broke (closed then).
    long Available();
    // Exactly `size` bytes (call after Available said they are there, or to finish a frame); false = broken.
    bool Read(void* data, size_t size);
}
