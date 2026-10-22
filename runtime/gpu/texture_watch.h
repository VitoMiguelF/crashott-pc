#pragma once

#include <cstdint>

// Write watch on guest physical memory for the texture cache (runtime/NOTES.md, "Desempenho").
//
// The renderer hashes every texture a frame uses, once per frame, to see if the game changed it
// (~25 MB per frame in the village, the biggest cost left on the command processor thread). A
// texture whose content stayed the same for a few frames gets its pages made read-only in both
// views of physical memory (0xA0000000 and 0xC0000000); while they stay that way it can't have
// changed and isn't hashed. A write by the game (or the runtime) faults once: the handler makes
// the page block writable again and the texture is hashed at its next use, as before. Writes the
// kernel would make (ReadFile into guest memory) already go through a host buffer and a memcpy
// when they meet a page they can't write (kernel/file_imports.cpp HostTransfer).
namespace gpu::watch
{
    // Installs the fault handler. False (and nothing is ever watched) if physical memory isn't
    // aliased (kernel/memory.cpp) or CRASH_TEXTURE_WATCH=0.
    bool Init();
    bool Enabled();
    // Makes [physical, physical + size) read-only and marks its pages clean. Physical addresses
    // (0 to 512 MB), any alignment (whole pages are protected).
    void Protect(uint32_t physical, uint32_t size);
    // True if no byte of the range can have been written since its Protect.
    bool Clean(uint32_t physical, uint32_t size);
    // Faults handled since the start (for the log).
    uint64_t Faults();
}
