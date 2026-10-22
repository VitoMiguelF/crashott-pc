// Debugging aid: guest memory snapshots and diffs on the input clock, to find where the game keeps
// a value (e.g. an option of its own menus).
//
// CRASH_MEMDIFF_TIMES="20,22,24.5" (seconds of the input clock, like CRASH_SCREENSHOT_TIMES): at
// each time the committed guest memory (the title's heaps below 0x7F000000, the image and its data
// at 0x80000000-0xA0000000 and the 512 MB of physical memory) is copied; from the second time on,
// every 32-bit word that changed since the previous snapshot is written to
// <CRASH_SCREENSHOT_DIR>\memdiff_<n>.bin as big-endian records {u32 guest address, u32 old, u32 new}.
// Comparing intervals where only the option changed with intervals where nothing did (a Python
// script, see runtime/NOTES.md) leaves the candidates.

#include "diagnostics.h"
#include "kernel/input.h"
#include "kernel/memory.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

namespace
{
    struct Region
    {
        uint32_t guest;
        std::vector<uint8_t> bytes;
    };

    std::vector<int64_t> g_times;
    size_t g_next = 0;
    int g_snapshot = 0;
    std::vector<Region> g_previous;
    std::filesystem::path g_directory;
    bool g_initialized = false;

    // Committed ranges of [begin, end).
    void AddCommitted(std::vector<Region>& regions, uint64_t begin, uint64_t end)
    {
        uint64_t address = begin;
        while (address < end)
        {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(g_memory.base + address, &info, sizeof(info)))
                break;
            uint64_t regionEnd = std::min<uint64_t>(end, uint64_t(static_cast<uint8_t*>(info.BaseAddress) - g_memory.base) + info.RegionSize);
            if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            {
                Region region{ uint32_t(address), {} };
                region.bytes.assign(g_memory.base + address, g_memory.base + regionEnd);
                regions.push_back(std::move(region));
            }
            address = regionEnd;
        }
    }

    void Snapshot()
    {
        std::vector<Region> regions;
        AddCommitted(regions, 0x10000, KERNEL_HEAP_BEGIN);
        AddCommitted(regions, 0x80000000ull, PHYSICAL_VIEW_A);
        AddCommitted(regions, PHYSICAL_VIEW_A, uint64_t(PHYSICAL_VIEW_A) + PHYSICAL_SIZE);
        size_t total = 0;
        for (const Region& r : regions)
            total += r.bytes.size();

        size_t changed = 0;
        if (g_snapshot > 0)
        {
            char name[64];
            snprintf(name, sizeof(name), "memdiff_%d.bin", g_snapshot);
            FILE* out = _wfopen((g_directory / name).c_str(), L"wb");
            for (const Region& now : regions)
            {
                auto previous = std::find_if(g_previous.begin(), g_previous.end(), [&](const Region& r) {
                    return r.guest <= now.guest && now.guest < r.guest + r.bytes.size();
                });
                if (previous == g_previous.end())
                    continue;
                size_t offset = now.guest - previous->guest;
                size_t length = std::min(now.bytes.size(), previous->bytes.size() - offset) & ~size_t(3);
                const uint8_t* a = previous->bytes.data() + offset;
                const uint8_t* b = now.bytes.data();
                for (size_t i = 0; i < length; i += 4)
                {
                    if (memcmp(a + i, b + i, 4) == 0)
                        continue;
                    uint32_t address = _byteswap_ulong(uint32_t(now.guest + i));
                    if (out)
                    {
                        fwrite(&address, 4, 1, out);
                        fwrite(a + i, 4, 1, out);
                        fwrite(b + i, 4, 1, out);
                    }
                    changed++;
                }
            }
            if (out)
                fclose(out);
        }
        Log("[memdiff] snapshot %d at %.2f s: %zu regions, %.0f MB; %zu words changed since the previous one", g_snapshot,
            double(input::ClockMs()) / 1000.0, regions.size(), double(total) / 1048576.0, changed);
        g_previous = std::move(regions);
        g_snapshot++;
    }
}

void MemDiffTick()
{
    if (!g_initialized)
    {
        g_initialized = true;
        const char* times = getenv("CRASH_MEMDIFF_TIMES");
        if (!times)
            return;
        for (const char* p = times; *p;)
        {
            char* end;
            double value = strtod(p, &end);
            if (end == p)
                break;
            g_times.push_back(int64_t(value * 1000.0));
            p = *end ? end + 1 : end;
        }
        std::sort(g_times.begin(), g_times.end());
        const char* directory = getenv("CRASH_SCREENSHOT_DIR");
        g_directory = directory ? directory : std::filesystem::current_path();
        Log("[memdiff] %zu snapshot(s) planned", g_times.size());
    }
    if (g_next >= g_times.size() || input::ClockMs() < g_times[g_next])
        return;
    while (g_next < g_times.size() && input::ClockMs() >= g_times[g_next])
        g_next++;
    Snapshot();
}

// CRASH_WATCH="0xA18BF5B0,..." (up to 4 guest addresses) from CRASH_WATCH_TIME seconds of the input
// clock (default 0): hardware write breakpoints (DR0-DR3) on every thread; each write logs the new
// value (as u32 and float), the guest function that wrote it and the guest functions found on the
// host stack (the callers), the first 12 times per address.
#include <tlhelp32.h>
#include <thread>

namespace
{
    uint32_t g_watch[4];
    uint32_t g_watchCount = 0;
    std::atomic<uint32_t> g_watchHits[4];

    LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* info)
    {
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
            return EXCEPTION_CONTINUE_SEARCH;
        CONTEXT* c = info->ContextRecord;
        DWORD64 which = c->Dr6 & 0xF;
        if (!which)
            return EXCEPTION_CONTINUE_SEARCH;
        c->Dr6 = 0;
        for (uint32_t i = 0; i < g_watchCount; i++)
        {
            if (!(which & (1ull << i)) || g_watchHits[i].fetch_add(1) >= 12)
                continue;
            uint32_t value = _byteswap_ulong(*reinterpret_cast<uint32_t*>(g_memory.base + g_watch[i]));
            float f;
            memcpy(&f, &value, 4);
            char callers[512] = {};
            size_t used = 0;
            uint32_t last = FindGuestFunction(uintptr_t(c->Rip));
            const uint64_t* stack = reinterpret_cast<const uint64_t*>(c->Rsp);
            for (int s = 0, found = 0; s < 512 && found < 8; s++)
            {
                uint32_t guest = FindGuestFunction(uintptr_t(stack[s]));
                if (guest && guest != last)
                {
                    used += snprintf(callers + used, sizeof(callers) - used, " %08X", guest);
                    last = guest;
                    found++;
                }
            }
            Log("[watch] write to 0x%08X: 0x%08X (%g) by sub_%08X; stack:%s", g_watch[i], value, f, FindGuestFunction(uintptr_t(c->Rip)), callers);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void ArmAllThreads()
    {
        DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;
        THREADENTRY32 entry{ sizeof(entry) };
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
                continue;
            HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
            if (!thread)
                continue;
            SuspendThread(thread);
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &c))
            {
                DWORD64* dr[4] = { &c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3 };
                c.Dr7 = 0;
                for (uint32_t i = 0; i < g_watchCount; i++)
                {
                    *dr[i] = DWORD64(g_memory.base + g_watch[i]);
                    c.Dr7 |= (1ull << (i * 2)) | (1ull << (16 + i * 4)) | (3ull << (18 + i * 4));  // local, write, 4 bytes
                }
                SetThreadContext(thread, &c);
            }
            ResumeThread(thread);
            CloseHandle(thread);
        }
        CloseHandle(snapshot);
    }

    struct WatchStarter
    {
        WatchStarter()
        {
            const char* list = getenv("CRASH_WATCH");
            if (!list)
                return;
            for (const char* p = list; *p && g_watchCount < 4;)
            {
                char* end;
                uint32_t address = uint32_t(strtoul(p, &end, 16));
                if (end == p)
                    break;
                g_watch[g_watchCount++] = address & ~3u;
                p = *end ? end + 1 : end;
            }
            double start = getenv("CRASH_WATCH_TIME") ? atof(getenv("CRASH_WATCH_TIME")) : 0.0;
            AddVectoredExceptionHandler(1, WatchHandler);
            std::thread([start] {
                while (input::ClockMs() < int64_t(start * 1000.0))
                    Sleep(100);
                Log("[watch] %u address(es) armed at %.1f s", g_watchCount, double(input::ClockMs()) / 1000.0);
                while (true)
                {
                    ArmAllThreads();
                    Sleep(1000);
                }
            }).detach();
        }
    } g_watchStarter;
}
