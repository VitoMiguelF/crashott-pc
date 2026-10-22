#pragma once

#include <cstdint>

// XMA decoder hardware (the audio block of the Xbox 360 southbridge): 320 decoder contexts in
// physical memory, a register window at XMA_MMIO_BEGIN (context array address, kick/lock/clear
// bitmasks) and a decoder that turns the XMA2 packets of kicked contexts into 16-bit PCM in their
// output ring buffers. See audio/xma.cpp.
namespace xma
{
    // Allocates the context array, sets up the register window and starts the decoder thread.
    // Call before the guest runs (the title reads the context array address once, at init).
    void Init();

    // XMACreateContext/XMAReleaseContext: a free context's guest address (zeroed), or 0.
    uint32_t AllocateContext();
    void ReleaseContext(uint32_t context);
}
