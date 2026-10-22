#include "memory.h"
#include "../diagnostics.h"

#include <windows.h>

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

    // Placeholder APIs (Windows 10 1803+), looked up at run time so no extra import library is needed.
    constexpr ULONG X_MEM_RESERVE_PLACEHOLDER = 0x00040000;
    constexpr ULONG X_MEM_REPLACE_PLACEHOLDER = 0x00004000;
    constexpr ULONG X_MEM_PRESERVE_PLACEHOLDER = 0x00000002;
    using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
    using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);

    bool g_physicalAliased;

    // Reserves the 4 GB guest space with the physical views aliased: one pagefile-backed section
    // of PHYSICAL_SIZE bytes (committed up front, zero-filled on first touch) mapped at
    // PHYSICAL_VIEW_A and PHYSICAL_VIEW_C. The rest is a normal reservation.
    uint8_t* ReserveAliased(uintptr_t preferred)
    {
        HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
        auto virtualAlloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernelBase, "VirtualAlloc2"));
        auto mapViewOfFile3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernelBase, "MapViewOfFile3"));
        if (!virtualAlloc2 || !mapViewOfFile3)
            return nullptr;

        auto* base = static_cast<uint8_t*>(virtualAlloc2(nullptr, reinterpret_cast<void*>(preferred), GUEST_MEMORY_SIZE,
                                                         MEM_RESERVE | X_MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!base)
            return nullptr;

        // Split the placeholder into [0, A), [A, C), [C, E), [E, 4 GB).
        bool ok = VirtualFree(base, PHYSICAL_VIEW_A, MEM_RELEASE | X_MEM_PRESERVE_PLACEHOLDER) &&
                  VirtualFree(base + PHYSICAL_VIEW_A, PHYSICAL_SIZE, MEM_RELEASE | X_MEM_PRESERVE_PLACEHOLDER) &&
                  VirtualFree(base + PHYSICAL_VIEW_C, PHYSICAL_SIZE, MEM_RELEASE | X_MEM_PRESERVE_PLACEHOLDER);

        HANDLE section = ok ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, PHYSICAL_SIZE, nullptr) : nullptr;
        ok = section &&
             virtualAlloc2(nullptr, base, PHYSICAL_VIEW_A, MEM_RESERVE | X_MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) &&
             mapViewOfFile3(section, GetCurrentProcess(), base + PHYSICAL_VIEW_A, 0, PHYSICAL_SIZE, X_MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) &&
             mapViewOfFile3(section, GetCurrentProcess(), base + PHYSICAL_VIEW_C, 0, PHYSICAL_SIZE, X_MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) &&
             virtualAlloc2(nullptr, base + PHYSICAL_VIEW_E, GUEST_MEMORY_SIZE - PHYSICAL_VIEW_E, MEM_RESERVE | X_MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);

        if (!ok)
        {
            Log("[memory] aliased physical views failed (error %lu)", GetLastError());
            return nullptr; // the half-built reservation is leaked; Init falls back to a flat one
        }
        return base;
    }

    LONG CALLBACK DemandCommitHandler(PEXCEPTION_POINTERS info)
    {
        auto* record = info->ExceptionRecord;
        if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2)
            return EXCEPTION_CONTINUE_SEARCH;

        auto* host = reinterpret_cast<uint8_t*>(record->ExceptionInformation[1]);
        if (!g_memory.IsInGuestRange(host))
            return EXCEPTION_CONTINUE_SEARCH;

        uint32_t guest = g_memory.MapVirtual(host);
        // The E0000000 physical view isn't mapped (see memory.h): accesses there must fault, not
        // read fresh zero pages.
        if (guest < GUEST_NULL_GUARD || (guest >= KERNEL_HEAP_BEGIN && guest < KERNEL_HEAP_END) ||
            (guest >= GPU_MMIO_BEGIN && guest < GPU_MMIO_END) || (guest >= XMA_MMIO_BEGIN && guest < XMA_MMIO_END) ||
            (g_physicalAliased && guest >= PHYSICAL_VIEW_E))
            return EXCEPTION_CONTINUE_SEARCH;

        uint32_t page = guest & ~0xFFFu;
        if (!VirtualAlloc(g_memory.Translate(page), 0x1000, MEM_COMMIT, PAGE_READWRITE))
            return EXCEPTION_CONTINUE_SEARCH;

        if (++g_demandCommits <= MAX_LOGGED_DEMAND_COMMITS)
        {
            Log("[memory] committed guest page 0x%08X on demand (%s of 0x%08X, host rip %p)",
                page, record->ExceptionInformation[0] == 1 ? "write" : "read", guest,
                reinterpret_cast<void*>(info->ContextRecord->Rip));
        }

        return EXCEPTION_CONTINUE_EXECUTION;
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
        base = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(0x100000000ull), GUEST_MEMORY_SIZE, MEM_RESERVE, PAGE_READWRITE));
        if (!base)
            base = static_cast<uint8_t*>(VirtualAlloc(nullptr, GUEST_MEMORY_SIZE, MEM_RESERVE, PAGE_READWRITE));
    }

    if (!base)
        return false;

    AddVectoredExceptionHandler(1, DemandCommitHandler);
    return true;
}

bool Memory::Commit(uint32_t guest, size_t size)
{
    // The physical views are a committed section; VirtualAlloc can't commit inside a mapped view.
    if (IsPhysicalView(guest))
        return true;
    return VirtualAlloc(base + guest, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
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
        Fatal("GuestAlloc: VirtualAlloc commit failed at guest 0x%08X", guest);

    g_heapTop = uint32_t(guest + size);
    return guest;
}
