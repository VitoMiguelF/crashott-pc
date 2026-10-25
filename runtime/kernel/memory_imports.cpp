// xboxkrnl memory management: NtAllocateVirtualMemory/NtFreeVirtualMemory/NtQueryVirtualMemory on
// the virtual heap and MmAllocatePhysicalMemoryEx & co. on the physical heap. Semantics follow
// Windows (reserve/commit/decommit/release), with the Xbox 360 page-size flags as Xenia's
// xboxkrnl_memory.cc interprets them.

#include "kernel.h"
#include "memory.h"
#include "page_heap.h"

#include "../platform/platform.h"

#include <algorithm>

namespace
{
    // Xbox 360 allocation-type / protect flags.
    constexpr uint32_t X_MEM_COMMIT = 0x00001000;
    constexpr uint32_t X_MEM_RESERVE = 0x00002000;
    constexpr uint32_t X_MEM_DECOMMIT = 0x00004000;
    constexpr uint32_t X_MEM_RELEASE = 0x00008000;
    constexpr uint32_t X_MEM_FREE = 0x00010000;
    constexpr uint32_t X_MEM_PRIVATE = 0x00020000;
    constexpr uint32_t X_MEM_IMAGE = 0x01000000;
    constexpr uint32_t X_MEM_TOP_DOWN = 0x00100000;
    constexpr uint32_t X_MEM_LARGE_PAGES = 0x20000000; // 64 KB pages
    constexpr uint32_t X_MEM_16MB_PAGES = 0x80000000;

    constexpr uint32_t X_PAGE_READWRITE = 0x04;

    constexpr uint32_t ALLOCATION_GRANULARITY = 0x10000;

    // Title virtual heap, below the runtime's own kernel heap (memory.h).
    PageHeap g_virtualHeap("virtual", 0x40000000, KERNEL_HEAP_BEGIN);

    // Physical memory, mapped once at 0xA0000000: 512 MB, so `address & 0x1FFFFFFF` is a unique
    // physical address. The console also maps it at 0xC0000000 (16 MB pages) and 0xE0000000
    // (4 KB pages); every physical allocation goes to the 0xA0000000 view for now.
    constexpr uint32_t PHYSICAL_BASE = PHYSICAL_VIEW_A;
    PageHeap g_physicalHeap("physical", PHYSICAL_BASE, PHYSICAL_BASE + PHYSICAL_SIZE);

    // Addresses in the 0xC0000000 view are the same physical memory as 0xA0000000 (memory.h).
    uint32_t CanonicalPhysical(uint32_t guest)
    {
        return guest >= PHYSICAL_VIEW_C && guest < PHYSICAL_VIEW_E ? guest - (PHYSICAL_VIEW_C - PHYSICAL_VIEW_A) : guest;
    }

    PageHeap* FindHeap(uint32_t& guest)
    {
        guest = CanonicalPhysical(guest);
        if (g_virtualHeap.Contains(guest))
            return &g_virtualHeap;
        if (g_physicalHeap.Contains(guest))
            return &g_physicalHeap;
        return nullptr;
    }

    uint32_t PageSizeForFlags(uint32_t flags)
    {
        if (flags & X_MEM_16MB_PAGES)
            return 16 * 1024 * 1024;
        if (flags & X_MEM_LARGE_PAGES)
            return 64 * 1024;
        return PageHeap::PAGE_SIZE;
    }

    uint32_t StateToMem(PageHeap::State state)
    {
        switch (state)
        {
        case PageHeap::State::Committed: return X_MEM_COMMIT;
        case PageHeap::State::Reserved: return X_MEM_RESERVE;
        default: return X_MEM_FREE;
        }
    }

    // X_MEMORY_BASIC_INFORMATION: 7 big-endian dwords.
    void WriteBasicInformation(uint8_t* base, uint32_t info, uint32_t regionBase, uint32_t allocationBase,
                               uint32_t allocationProtect, uint32_t size, uint32_t state, uint32_t protect, uint32_t type)
    {
        PPC_STORE_U32(info + 0x00, regionBase);
        PPC_STORE_U32(info + 0x04, allocationBase);
        PPC_STORE_U32(info + 0x08, allocationProtect);
        PPC_STORE_U32(info + 0x0C, size);
        PPC_STORE_U32(info + 0x10, state);
        PPC_STORE_U32(info + 0x14, protect);
        PPC_STORE_U32(info + 0x18, type);
    }
}

uint32_t AllocatePhysicalMemory(uint32_t size, uint32_t alignment, bool topDown)
{
    size = (size + PageHeap::PAGE_SIZE - 1) & ~(PageHeap::PAGE_SIZE - 1);
    uint32_t result = g_physicalHeap.Reserve(0, size, std::max(alignment, PageHeap::PAGE_SIZE), X_PAGE_READWRITE, topDown);
    if (result && !g_physicalHeap.Commit(result, size, X_PAGE_READWRITE))
    {
        g_physicalHeap.Release(result);
        result = 0;
    }
    return result;
}

// NTSTATUS NtAllocateVirtualMemory(void** BaseAddress, size_t* RegionSize, u32 AllocationType,
//                                  u32 Protect, u32 DebugMemory)
PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    KERNEL_IMPORT_ENTER(NtAllocateVirtualMemory);

    uint32_t basePtr = ctx.r3.u32;
    uint32_t sizePtr = ctx.r4.u32;
    uint32_t type = ctx.r5.u32;
    uint32_t protect = ctx.r6.u32;
    uint32_t requestedBase = PPC_LOAD_U32(basePtr);
    uint32_t requestedSize = PPC_LOAD_U32(sizePtr);

    auto finish = [&](uint32_t status, uint32_t allocBase, uint32_t allocSize)
    {
        if (status == X_STATUS_SUCCESS)
        {
            PPC_STORE_U32(basePtr, allocBase);
            PPC_STORE_U32(sizePtr, allocSize);
        }
        KERNEL_LOG("[kernel] NtAllocateVirtualMemory(base=0x%08X, size=0x%X, type=0x%08X, protect=0x%X) lr=0x%08X -> 0x%08X (0x%08X, 0x%X)",
                   requestedBase, requestedSize, type, protect, GuestLR(ctx), status, allocBase, allocSize);
        if (status != X_STATUS_SUCCESS && !logCall)
            Log("[kernel] NtAllocateVirtualMemory(base=0x%08X, size=0x%X, type=0x%08X) failed: 0x%08X", requestedBase, requestedSize, type, status);
        ctx.r3.u64 = status;
    };

    if (!(type & (X_MEM_COMMIT | X_MEM_RESERVE)) || requestedSize == 0)
        return finish(X_STATUS_INVALID_PARAMETER, 0, 0);

    uint32_t pageSize = PageSizeForFlags(type);
    uint32_t start = requestedBase & ~(pageSize - 1);
    uint64_t end = (uint64_t(requestedBase) + requestedSize + pageSize - 1) & ~uint64_t(pageSize - 1);
    uint32_t size = uint32_t(end - start);
    uint32_t pageProtect = protect & 0xFFFF;

    if (requestedBase && !g_virtualHeap.Contains(start))
        return finish(X_STATUS_INVALID_PARAMETER, 0, 0);

    if (type & X_MEM_RESERVE)
    {
        uint32_t alignment = std::max(pageSize, ALLOCATION_GRANULARITY);
        if (requestedBase)
            start &= ~(alignment - 1), size = uint32_t(end - start);

        uint32_t reserved = g_virtualHeap.Reserve(requestedBase ? start : 0, size, alignment, pageProtect, (type & X_MEM_TOP_DOWN) != 0);
        if (!reserved)
            return finish(requestedBase ? X_STATUS_CONFLICTING_ADDRESSES : X_STATUS_NO_MEMORY, 0, 0);
        start = reserved;
    }
    else if (!requestedBase)
    {
        // MEM_COMMIT alone with no base: reserve and commit, as Windows does.
        uint32_t alignment = std::max(pageSize, ALLOCATION_GRANULARITY);
        start = g_virtualHeap.Reserve(0, size, alignment, pageProtect, (type & X_MEM_TOP_DOWN) != 0);
        if (!start)
            return finish(X_STATUS_NO_MEMORY, 0, 0);
    }

    if ((type & X_MEM_COMMIT) && !g_virtualHeap.Commit(start, size, pageProtect))
    {
        if (type & X_MEM_RESERVE)
            g_virtualHeap.Release(start);
        return finish(X_STATUS_CONFLICTING_ADDRESSES, 0, 0);
    }

    finish(X_STATUS_SUCCESS, start, size);
}

// NTSTATUS NtFreeVirtualMemory(void** BaseAddress, size_t* RegionSize, u32 FreeType, u32 DebugMemory)
PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    KERNEL_IMPORT_ENTER(NtFreeVirtualMemory);

    uint32_t basePtr = ctx.r3.u32;
    uint32_t sizePtr = ctx.r4.u32;
    uint32_t type = ctx.r5.u32;
    uint32_t requestedBase = PPC_LOAD_U32(basePtr);
    uint32_t requestedSize = sizePtr ? PPC_LOAD_U32(sizePtr) : 0;

    uint32_t status = X_STATUS_SUCCESS;
    uint32_t start = requestedBase & ~(PageHeap::PAGE_SIZE - 1);
    uint32_t size = 0;

    if (!g_virtualHeap.Contains(requestedBase) || !(type & (X_MEM_DECOMMIT | X_MEM_RELEASE)))
    {
        status = X_STATUS_INVALID_PARAMETER;
    }
    else if (type & X_MEM_RELEASE)
    {
        size = g_virtualHeap.Release(start);
        if (!size)
            status = X_STATUS_MEMORY_NOT_ALLOCATED;
    }
    else
    {
        if (requestedSize)
        {
            uint64_t end = (uint64_t(requestedBase) + requestedSize + PageHeap::PAGE_SIZE - 1) & ~uint64_t(PageHeap::PAGE_SIZE - 1);
            size = uint32_t(end - start);
        }
        else
        {
            // Size 0: from the base to the end of its reservation.
            uint32_t allocationBase = g_virtualHeap.Query(start).allocationBase;
            uint32_t cursor = start;
            while (allocationBase && g_virtualHeap.Contains(cursor))
            {
                auto region = g_virtualHeap.Query(cursor);
                if (region.allocationBase != allocationBase)
                    break;
                cursor += region.size;
            }
            size = cursor - start;
        }

        if (!size || !g_virtualHeap.Decommit(start, size))
            status = X_STATUS_MEMORY_NOT_ALLOCATED;
    }

    if (status == X_STATUS_SUCCESS)
    {
        PPC_STORE_U32(basePtr, start);
        if (sizePtr)
            PPC_STORE_U32(sizePtr, size);
    }

    KERNEL_LOG("[kernel] NtFreeVirtualMemory(base=0x%08X, size=0x%X, type=0x%08X) lr=0x%08X -> 0x%08X",
               requestedBase, requestedSize, type, GuestLR(ctx), status);
    ctx.r3.u64 = status;
}

// NTSTATUS NtQueryVirtualMemory(void* BaseAddress, MEMORY_BASIC_INFORMATION* Info)
PPC_FUNC(__imp__NtQueryVirtualMemory)
{
    KERNEL_IMPORT_ENTER(NtQueryVirtualMemory);

    uint32_t address = ctx.r3.u32;
    uint32_t info = ctx.r4.u32;
    uint32_t status = X_STATUS_SUCCESS;

    if (PageHeap* heap = FindHeap(address))
    {
        auto region = heap->Query(address);
        WriteBasicInformation(base, info, region.base, region.allocationBase, region.allocationProtect, region.size,
                              StateToMem(region.state), region.protect, region.state == PageHeap::State::Free ? 0 : X_MEM_PRIVATE);
    }
    else if (address >= 0x10000)
    {
        // Image, runtime-owned memory (stacks, kernel objects) or untracked space: describe what
        // the host has mapped, as one region bounded by the host's own region.
        plat::MemoryRegion region = plat::QueryMemory(g_memory.Translate(address));
        uint32_t regionBase = g_memory.MapVirtual(region.base);
        uint32_t state = region.state == plat::PageState::Committed ? X_MEM_COMMIT
                       : region.state == plat::PageState::Reserved ? X_MEM_RESERVE : X_MEM_FREE;
        uint32_t protect = state == X_MEM_COMMIT ? X_PAGE_READWRITE : 0;
        uint32_t type = address >= 0x82000000 && address < 0x90000000 ? X_MEM_IMAGE : X_MEM_PRIVATE;
        WriteBasicInformation(base, info, regionBase, regionBase, X_PAGE_READWRITE, uint32_t(region.size),
                              state, protect, state == X_MEM_FREE ? 0 : type);
    }
    else
    {
        status = X_STATUS_INVALID_PARAMETER;
    }

    KERNEL_LOG("[kernel] NtQueryVirtualMemory(0x%08X) lr=0x%08X -> 0x%08X", address, GuestLR(ctx), status);
    ctx.r3.u64 = status;
}

// void* MmAllocatePhysicalMemoryEx(u32 Flags, u32 Size, u32 Protect, u32 MinAddress,
//                                  u32 MaxAddress, u32 Alignment)
PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    KERNEL_IMPORT_ENTER(MmAllocatePhysicalMemoryEx);

    uint32_t flags = ctx.r3.u32;
    uint32_t requestedSize = ctx.r4.u32;
    uint32_t protect = ctx.r5.u32;
    uint32_t minAddress = ctx.r6.u32;
    uint32_t maxAddress = ctx.r7.u32;
    uint32_t alignment = ctx.r8.u32;

    uint32_t pageSize = PageSizeForFlags(protect);
    uint32_t size = uint32_t((uint64_t(requestedSize) + pageSize - 1) & ~uint64_t(pageSize - 1));
    alignment = std::max(alignment, pageSize);

    uint32_t low = PHYSICAL_BASE + std::min(minAddress, PHYSICAL_SIZE - 1);
    uint32_t high = PHYSICAL_BASE + uint32_t(std::min<uint64_t>(uint64_t(maxAddress) + 1, PHYSICAL_SIZE));

    uint32_t result = 0;
    if (size && (alignment & (alignment - 1)) == 0)
    {
        result = g_physicalHeap.Reserve(0, size, alignment, protect & 0xFFFF, false, low, high);
        if (result && !g_physicalHeap.Commit(result, size, protect & 0xFFFF))
        {
            g_physicalHeap.Release(result);
            result = 0;
        }
    }

    KERNEL_LOG("[kernel] MmAllocatePhysicalMemoryEx(flags=0x%X, size=0x%X, protect=0x%08X, min=0x%08X, max=0x%08X, align=0x%X) lr=0x%08X -> 0x%08X",
               flags, requestedSize, protect, minAddress, maxAddress, ctx.r8.u32, GuestLR(ctx), result);
    if (!result && !logCall)
        Log("[kernel] MmAllocatePhysicalMemoryEx(size=0x%X, protect=0x%08X) failed", requestedSize, protect);
    ctx.r3.u64 = result;
}

// void MmFreePhysicalMemory(u32 Type, void* Address)
PPC_FUNC(__imp__MmFreePhysicalMemory)
{
    KERNEL_IMPORT_ENTER(MmFreePhysicalMemory);

    uint32_t address = ctx.r4.u32;
    uint32_t freed = g_physicalHeap.Contains(address) ? g_physicalHeap.Release(address) : 0;
    KERNEL_LOG("[kernel] MmFreePhysicalMemory(type=0x%X, 0x%08X) lr=0x%08X -> freed 0x%X",
               ctx.r3.u32, address, GuestLR(ctx), freed);
    if (!freed)
        Log("[kernel] MmFreePhysicalMemory: 0x%08X is not a physical allocation", address);
}

// u32 MmGetPhysicalAddress(void* Address)
PPC_FUNC(__imp__MmGetPhysicalAddress)
{
    KERNEL_IMPORT_ENTER(MmGetPhysicalAddress);

    uint32_t address = ctx.r3.u32;
    uint32_t physical = address & 0x1FFFFFFF;
    // The 4 KB page view at 0xE0000000 is offset by one page (as in Xenia).
    if (address >= 0xE0000000)
        physical += 0x1000;

    KERNEL_LOG("[kernel] MmGetPhysicalAddress(0x%08X) -> 0x%08X", address, physical);
    ctx.r3.u64 = physical;
}

// u32 MmQueryAddressProtect(void* Address)
PPC_FUNC(__imp__MmQueryAddressProtect)
{
    KERNEL_IMPORT_ENTER(MmQueryAddressProtect);

    uint32_t address = ctx.r3.u32;
    uint32_t protect;
    if (PageHeap* heap = FindHeap(address))
    {
        protect = heap->QueryProtect(address);
    }
    else
    {
        bool committed = plat::QueryMemory(g_memory.Translate(address)).state == plat::PageState::Committed;
        protect = address >= 0x10000 && committed ? X_PAGE_READWRITE : 0;
    }

    KERNEL_LOG("[kernel] MmQueryAddressProtect(0x%08X) lr=0x%08X -> 0x%X", address, GuestLR(ctx), protect);
    ctx.r3.u64 = protect;
}

// NTSTATUS MmQueryStatistics(MM_STATISTICS* Stats). Layout and placeholder values from Xenia.
PPC_FUNC(__imp__MmQueryStatistics)
{
    KERNEL_IMPORT_ENTER(MmQueryStatistics);

    constexpr uint32_t STATS_SIZE = 104;
    constexpr uint32_t TOTAL_PHYSICAL_PAGES = 0x20000; // 512 MB / 4 KB
    constexpr uint32_t KERNEL_PAGES = 0x300;

    uint32_t stats = ctx.r3.u32;
    uint32_t status = X_STATUS_SUCCESS;

    if (!stats || PPC_LOAD_U32(stats) != STATS_SIZE)
    {
        status = X_STATUS_INVALID_PARAMETER;
    }
    else
    {
        uint32_t usedPages = g_physicalHeap.ReservedPageCount() + g_virtualHeap.ReservedPageCount();
        uint32_t available = usedPages + KERNEL_PAGES < TOTAL_PHYSICAL_PAGES ? TOTAL_PHYSICAL_PAGES - KERNEL_PAGES - usedPages : 0;

        memset(g_memory.Translate(stats + 4), 0, STATS_SIZE - 4);
        PPC_STORE_U32(stats + 0x04, TOTAL_PHYSICAL_PAGES);
        PPC_STORE_U32(stats + 0x08, KERNEL_PAGES);

        // Title section (11 dwords at 0x0C).
        uint32_t title = stats + 0x0C;
        PPC_STORE_U32(title + 0x00, available);
        PPC_STORE_U32(title + 0x04, 0x2FFE0000); // total virtual memory bytes
        PPC_STORE_U32(title + 0x08, 0x00160000); // reserved virtual memory bytes
        PPC_STORE_U32(title + 0x0C, 0x00001000); // physical pages
        PPC_STORE_U32(title + 0x10, 0x00000010); // pool pages
        for (uint32_t offset = 0x14; offset <= 0x28; offset += 4)
            PPC_STORE_U32(title + offset, 0x100); // stack, image, heap, virtual, page table, cache pages

        // System section (11 dwords at 0x38) stays zeroed.
        PPC_STORE_U32(stats + 0x64, TOTAL_PHYSICAL_PAGES - 1); // highest physical page
    }

    KERNEL_LOG("[kernel] MmQueryStatistics(0x%08X) lr=0x%08X -> 0x%08X", stats, GuestLR(ctx), status);
    ctx.r3.u64 = status;
}
