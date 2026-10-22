// Write watch on guest physical memory for the texture cache (texture_watch.h).

#include "texture_watch.h"
#include "../diagnostics.h"
#include "../kernel/memory.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t PAGE = 0x1000;
    constexpr uint32_t PAGES = PHYSICAL_SIZE / PAGE;
    // A fault unprotects this many bytes around the write (aligned): code that fills a buffer
    // (a level load reusing texture memory) faults once per block instead of once per page.
    constexpr uint32_t UNPROTECT_BLOCK = 0x10000;

    bool g_enabled = false;
    // 1 = read-only since a Protect, no write since (both views). Changed under g_lock only.
    std::atomic<uint8_t> g_clean[PAGES];
    // Protect and the handler both change protection and g_clean together: otherwise a fault
    // handled between Protect's marking and its VirtualProtect could leave a page writable but
    // marked clean.
    SRWLOCK g_lock = SRWLOCK_INIT;
    std::atomic<uint64_t> g_faults;

    void SetProtection(uint32_t physical, uint32_t size, DWORD protection)
    {
        DWORD old;
        VirtualProtect(g_memory.Translate(PHYSICAL_VIEW_A + physical), size, protection, &old);
        VirtualProtect(g_memory.Translate(PHYSICAL_VIEW_C + physical), size, protection, &old);
    }

    LONG CALLBACK WriteFaultHandler(PEXCEPTION_POINTERS info)
    {
        auto* record = info->ExceptionRecord;
        if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || record->ExceptionInformation[0] != 1)
            return EXCEPTION_CONTINUE_SEARCH;
        auto* host = reinterpret_cast<void*>(record->ExceptionInformation[1]);
        if (!g_memory.IsInGuestRange(host))
            return EXCEPTION_CONTINUE_SEARCH;
        uint32_t guest = g_memory.MapVirtual(host);
        if (guest < PHYSICAL_VIEW_A || guest >= PHYSICAL_VIEW_E)
            return EXCEPTION_CONTINUE_SEARCH;
        // Both physical views are committed read-write except for what Protect made read-only,
        // so a write fault here is always one of ours (even if the page is no longer marked
        // clean: Protect may have just been undone by another thread's fault on the same block).
        uint32_t block = (guest & (PHYSICAL_SIZE - 1)) & ~(UNPROTECT_BLOCK - 1);
        AcquireSRWLockExclusive(&g_lock);
        for (uint32_t page = block / PAGE; page < (block + UNPROTECT_BLOCK) / PAGE; page++)
            g_clean[page].store(0, std::memory_order_relaxed);
        SetProtection(block, UNPROTECT_BLOCK, PAGE_READWRITE);
        ReleaseSRWLockExclusive(&g_lock);
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
}

namespace gpu::watch
{
    bool Init()
    {
        const char* setting = getenv("CRASH_TEXTURE_WATCH");
        if (setting && !strcmp(setting, "0"))
        {
            Log("[gpu] texture write watch off (CRASH_TEXTURE_WATCH=0): every texture is hashed every frame");
            return false;
        }
        if (!g_memory.IsPhysicalView(PHYSICAL_VIEW_A))
        {
            Log("[gpu] texture write watch off: physical memory is not aliased");
            return false;
        }
        AddVectoredExceptionHandler(1, WriteFaultHandler);
        g_enabled = true;
        Log("[gpu] texture write watch on: stable textures are not hashed again until written");
        return true;
    }

    bool Enabled()
    {
        return g_enabled;
    }

    void Protect(uint32_t physical, uint32_t size)
    {
        if (!g_enabled || !size || physical >= PHYSICAL_SIZE)
            return;
        size = std::min<uint32_t>(size, PHYSICAL_SIZE - physical);
        uint32_t first = physical / PAGE, last = (physical + size - 1) / PAGE;
        AcquireSRWLockExclusive(&g_lock);
        for (uint32_t page = first; page <= last; page++)
            g_clean[page].store(1, std::memory_order_relaxed);
        SetProtection(first * PAGE, (last - first + 1) * PAGE, PAGE_READONLY);
        ReleaseSRWLockExclusive(&g_lock);
    }

    bool Clean(uint32_t physical, uint32_t size)
    {
        if (!g_enabled || !size || physical >= PHYSICAL_SIZE)
            return false;
        size = std::min<uint32_t>(size, PHYSICAL_SIZE - physical);
        for (uint32_t page = physical / PAGE, last = (physical + size - 1) / PAGE; page <= last; page++)
        {
            if (!g_clean[page].load(std::memory_order_acquire))
                return false;
        }
        return true;
    }

    uint64_t Faults()
    {
        return g_faults.load(std::memory_order_relaxed);
    }
}
