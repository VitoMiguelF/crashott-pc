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
#include "platform/platform.h"
#include "platform/crt.h"

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
            plat::MemoryRegion info = plat::QueryMemory(g_memory.base + address);
            if (!info.size)
                break;
            uint64_t regionEnd = std::min<uint64_t>(end, uint64_t(static_cast<uint8_t*>(info.base) - g_memory.base) + info.size);
            if (info.readable)
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
            FILE* out = plat::OpenCFile(g_directory / name, "wb");
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
                    uint32_t address = __builtin_bswap32(uint32_t(now.guest + i));
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
