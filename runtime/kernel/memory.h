#pragma once

#include <cstddef>
#include <cstdint>

// 4 GB of guest address space backed by one host reservation, as in UnleashedRecomp's
// kernel/memory.h. Guest address `g` lives at `base + g`. Pages are committed explicitly for the
// regions the runtime sets up (image, function table, kernel heap) and on demand, by a vectored
// exception handler, for anything else the guest touches (except the first 64 KB, so null
// pointer accesses still fault).
struct Memory
{
    uint8_t* base{};

    bool Init();
    bool Commit(uint32_t guest, size_t size);

    // True for addresses in the aliased physical views (always committed; see below).
    bool IsPhysicalView(uint32_t guest) const;
    bool IsInGuestRange(const void* host) const;

    uint8_t* Translate(uint32_t guest) const { return base + guest; }
    uint32_t MapVirtual(const void* host) const { return uint32_t(static_cast<const uint8_t*>(host) - base); }
};

extern Memory g_memory;

// Physical memory (512 MB) is visible through several views, as on the console: 0xA0000000
// (64 KB pages) and 0xC0000000 (16 MB pages) are two mappings of the same host section, so a
// pointer converted between them sees the same data. The console's third view, 0xE0000000 (4 KB
// pages), is offset by 0x1000 in physical memory, which can't be mapped at the host's 64 KB view
// granularity; it is left unmapped and accesses there fault instead of reading zeros.
constexpr uint32_t PHYSICAL_SIZE = 0x20000000;
constexpr uint32_t PHYSICAL_VIEW_A = 0xA0000000;
constexpr uint32_t PHYSICAL_VIEW_C = 0xC0000000;
constexpr uint32_t PHYSICAL_VIEW_E = 0xE0000000;

// Guest range owned by the runtime (thread blocks, stacks, kernel variables). Below the
// 0x82000000 image, above where the title's own NtAllocateVirtualMemory/MmAllocatePhysicalMemoryEx
// allocations will go once implemented. Never committed on demand, so a guest stack running into
// its guard pages faults instead of silently growing.
constexpr uint32_t KERNEL_HEAP_BEGIN = 0x7F000000;
constexpr uint32_t KERNEL_HEAP_END = 0x7FC00000;

// Xenos GPU register window (register N at GPU_MMIO_BEGIN + N * 4), owned by gpu/gpu.cpp.
// Committed and trapped by the GPU code, never committed on demand.
constexpr uint32_t GPU_MMIO_BEGIN = 0x7FC80000;
constexpr uint32_t GPU_MMIO_END = 0x7FD00000;

// XMA decoder (audio) register window, owned by audio/xma.cpp: register N at XMA_MMIO_BEGIN + N * 4,
// little-endian (the title uses lwbrx/stwbrx). Committed and trapped by the XMA code, never
// committed on demand.
constexpr uint32_t XMA_MMIO_BEGIN = 0x7FEA0000;
constexpr uint32_t XMA_MMIO_END = 0x7FEB0000;

// Physical memory for runtime code (same heap as MmAllocatePhysicalMemoryEx, 0xA0000000+).
// Returns a committed, zeroed guest address, or 0. `topDown` allocates from the top of the heap,
// out of the way of the title's own allocations.
uint32_t AllocatePhysicalMemory(uint32_t size, uint32_t alignment, bool topDown = false);

// Bump allocator for the runtime-owned range. Returns zeroed, committed guest memory.
// `guardSize` bytes below the block are left uncommitted (used for stacks).
uint32_t GuestAlloc(size_t size, size_t align = 16, size_t guardSize = 0);

inline uint32_t LoadBE32(uint32_t guest)
{
    return __builtin_bswap32(*reinterpret_cast<uint32_t*>(g_memory.Translate(guest)));
}

inline void StoreBE32(uint32_t guest, uint32_t value)
{
    *reinterpret_cast<uint32_t*>(g_memory.Translate(guest)) = __builtin_bswap32(value);
}

inline void StoreBE16(uint32_t guest, uint16_t value)
{
    *reinterpret_cast<uint16_t*>(g_memory.Translate(guest)) = __builtin_bswap16(value);
}

inline void StoreBE64(uint32_t guest, uint64_t value)
{
    *reinterpret_cast<uint64_t*>(g_memory.Translate(guest)) = __builtin_bswap64(value);
}
