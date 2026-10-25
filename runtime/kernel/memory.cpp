#include "memory.h"
#include "../diagnostics.h"

#include "../platform/platform.h"

#include <mutex>

Memory g_memory;

namespace
{
    constexpr uint64_t GUEST_MEMORY_SIZE = 0x100000000ull;
    constexpr uint32_t GUEST_NULL_GUARD = 0x10000;

    std::mutex g_heapMutex;
    uint32_t g_heapTop = KERNEL_HEAP_BEGIN;

    constexpr uint32_t MAX_LOGGED_DEMAND_COMMITS = 32;
    uint32_t g_demandCommits;

    bool g_physicalAliased;

    // Reserves the 4 GB guest space with the physical views aliased: one shared section of
    // PHYSICAL_SIZE bytes (zero-filled on first touch) mapped at PHYSICAL_VIEW_A and
    // PHYSICAL_VIEW_C. The rest is a normal reservation.
    uint8_t* ReserveAliased(uintptr_t preferred)
    {
        const size_t views[] = { PHYSICAL_VIEW_A, PHYSICAL_VIEW_C };
        uint8_t* base = plat::ReserveMemoryWithViews(GUEST_MEMORY_SIZE, preferred, PHYSICAL_SIZE, views, 2);
        if (!base)
            Log("[memory] aliased physical views failed (%s)", plat::LastErrorText().c_str());
        return base;
    }

    bool DemandCommitHandler(plat::FaultInfo& fault)
    {
        if (fault.kind != plat::FaultKind::AccessViolation)
            return false;

        auto* host = static_cast<uint8_t*>(fault.address);
        if (!g_memory.IsInGuestRange(host))
            return false;

        uint32_t guest = g_memory.MapVirtual(host);
        // The E0000000 physical view isn't mapped (see memory.h): accesses there must fault, not
        // read fresh zero pages.
        if (guest < GUEST_NULL_GUARD || (guest >= KERNEL_HEAP_BEGIN && guest < KERNEL_HEAP_END) ||
            (guest >= GPU_MMIO_BEGIN && guest < GPU_MMIO_END) || (guest >= XMA_MMIO_BEGIN && guest < XMA_MMIO_END) ||
            (g_physicalAliased && guest >= PHYSICAL_VIEW_E))
            return false;

        uint32_t page = guest & ~0xFFFu;
        if (!plat::CommitMemory(g_memory.Translate(page), 0x1000))
            return false;

        if (++g_demandCommits <= MAX_LOGGED_DEMAND_COMMITS)
        {
            Log("[memory] committed guest page 0x%08X on demand (%s of 0x%08X, host rip %p)",
                page, fault.write ? "write" : "read", guest, reinterpret_cast<void*>(fault.InstructionPointer()));
        }

        return true;
    }
}

bool Memory::Init()
{
    // Same preferred address as UnleashedRecomp; any 64 KB aligned address works.
    base = ReserveAliased(0x100000000ull);
    if (!base)
        base = ReserveAliased(0);
    g_physicalAliased = base != nullptr;

    if (!base)
    {
        Log("[memory] WARNING: physical views not aliased (0xC0000000 won't show 0xA0000000's contents)");
        base = plat::ReserveMemory(GUEST_MEMORY_SIZE, 0x100000000ull);
    }

    if (!base)
        return false;

    plat::AddFaultHandler(DemandCommitHandler);
    return true;
}

bool Memory::Commit(uint32_t guest, size_t size)
{
    // The physical views are a committed section; the host can't commit inside a mapped view.
    if (IsPhysicalView(guest))
        return true;
    return plat::CommitMemory(base + guest, size);
}

bool Memory::IsPhysicalView(uint32_t guest) const
{
    return g_physicalAliased && guest >= PHYSICAL_VIEW_A && guest < PHYSICAL_VIEW_E;
}

bool Memory::IsInGuestRange(const void* host) const
{
    auto* p = static_cast<const uint8_t*>(host);
    return p >= base && p < base + GUEST_MEMORY_SIZE;
}

uint32_t GuestAlloc(size_t size, size_t align, size_t guardSize)
{
    std::lock_guard lock(g_heapMutex);

    uint32_t guest = uint32_t((g_heapTop + guardSize + align - 1) & ~(align - 1));
    if (guest + size > KERNEL_HEAP_END)
        Fatal("GuestAlloc: kernel heap exhausted (0x%zX bytes requested)", size);

    if (!g_memory.Commit(guest, size))
        Fatal("GuestAlloc: host commit failed at guest 0x%08X", guest);

    g_heapTop = uint32_t(guest + size);
    return guest;
}
