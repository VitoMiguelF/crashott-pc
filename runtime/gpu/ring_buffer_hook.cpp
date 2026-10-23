// Bigger D3D command buffer (runtime/NOTES.md, "Desempenho").
//
// The title's D3D (statically linked) builds its command buffers in a "secondary" ring split into
// segments; before reusing a segment it spins until the GPU has written that segment's fence
// (sub_8226C958 / sub_8226D968). Crash passes zeros in D3DRING_BUFFER_PARAMETERS
// (D3DPRESENT_PARAMETERS + 72), so it gets D3D's defaults: 2 MB in 32 segments of 64 KB. On the
// console the GPU front end eats that faster than the CPU fills it; here the command processor is a
// host thread translating every draw to Vulkan, slower than the game's own submission, so 2 MB
// (less than one frame of the village: ~1,600 draws with their constants) kept the game thread
// waiting on it while rendering and left the command processor idle while the game updated the
// next frame. A larger ring lets the game run up to ~a frame ahead, overlapping its update with the
// command processor. Only the queue gets longer: what is drawn and when the game updates don't
// change (the swap still waits for the vblanks).
//
// sub_8226D300(device, D3DRING_BUFFER_PARAMETERS*): Flags +0, PrimarySize +4, pPrimary +8,
// SecondarySize +12, pSecondary +16, SegmentCount +20 (0 = defaults 32 KB / 2 MB / 32). Only the
// segment size is kept by D3D (device +14760), so more segments of the same size are fine.
// CRASH_D3D_RING_MB=<n>: secondary ring size (default 32; 0 = the game's own 6 MB).

#include "../diagnostics.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>

extern "C" PPC_FUNC(__imp__sub_8226D300);
extern "C" PPC_FUNC(__imp__sub_8226C958);

namespace
{
    constexpr uint32_t DEFAULT_SECONDARY = 2u << 20;
    constexpr uint32_t DEFAULT_SEGMENTS = 32;
    constexpr uint32_t DEFAULT_RING_MB = 32;
}

PPC_FUNC(sub_8226D300)
{
    uint32_t parameters = ctx.r4.u32;
    static const uint32_t ringMb = [] {
        const char* text = getenv("CRASH_D3D_RING_MB");
        return text ? uint32_t(atoi(text)) : DEFAULT_RING_MB;
    }();
    if (parameters && ringMb)
    {
        uint32_t secondarySize = LoadBE32(parameters + 12);
        uint32_t secondaryBuffer = LoadBE32(parameters + 16);
        uint32_t segments = LoadBE32(parameters + 20);
        uint32_t size = secondarySize ? secondarySize : DEFAULT_SECONDARY;
        uint32_t count = segments ? segments : DEFAULT_SEGMENTS;
        uint32_t wanted = ringMb << 20;
        // Only when D3D allocates the ring itself, and keeping its segment size.
        static const uint32_t segmentKb = [] {
            const char* text = getenv("CRASH_D3D_SEGMENT_KB");
            return text ? uint32_t(atoi(text)) : 0u;
        }();
        if (!secondaryBuffer && wanted > size && size % count == 0)
        {
            // CRASH_D3D_SEGMENT_KB=<n>: another segment size (the command processor gets each
            // segment when it is full or kicked; smaller = sooner, more kicks).
            uint32_t segmentSize = segmentKb ? segmentKb << 10 : size / count;
            uint32_t newCount = wanted / segmentSize;
            StoreBE32(parameters + 12, segmentSize * newCount);
            StoreBE32(parameters + 20, newCount);
            Log("[gpu] D3D command buffer: %u KB in %u segments -> %u KB in %u segments of %u KB (CRASH_D3D_RING_MB)",
                size >> 10, count, (segmentSize * newCount) >> 10, newCount, segmentSize >> 10);
        }
        else
        {
            Log("[gpu] D3D command buffer: the game's own (%u KB, %u segments, buffer 0x%08X)", size >> 10, count, secondaryBuffer);
        }
    }
    __imp__sub_8226D300(ctx, base);
}

// CRASH_D3D_WAIT_STATS=1: time the game spends in D3D's wait for a GPU fence (sub_8226C958:
// device r3, fence r4), per call site, logged every 10 s (diagnostics of the command buffer).
PPC_FUNC(sub_8226C958)
{
    static const bool enabled = getenv("CRASH_D3D_WAIT_STATS") != nullptr;
    if (!enabled)
    {
        __imp__sub_8226C958(ctx, base);
        return;
    }
    using Clock = std::chrono::steady_clock;
    struct Site
    {
        Clock::duration time{};
        uint64_t calls = 0, waited = 0;
        int64_t behindSum = 0;
    };
    static std::map<uint32_t, Site> s_sites;
    static Clock::time_point s_lastLog = Clock::now();
    uint32_t device = ctx.r3.u32, site = GuestLR(ctx);
    uint32_t latest = LoadBE32(device + 10780);
    uint32_t done = LoadBE32(LoadBE32(device + 10768));
    Clock::time_point start = Clock::now();
    __imp__sub_8226C958(ctx, base);
    Clock::time_point end = Clock::now();
    Site& entry = s_sites[site];
    entry.calls++;
    entry.time += end - start;
    if (end - start > std::chrono::microseconds(10))  // really waited
    {
        entry.waited++;
        entry.behindSum += int32_t(latest - done);
    }
    if (end - s_lastLog > std::chrono::seconds(10))
    {
        double seconds = std::chrono::duration<double>(end - s_lastLog).count();
        s_lastLog = end;
        for (auto& [address, value] : s_sites)
            Log("[gpu] D3D fence wait at 0x%08X: %.2f ms/s, %llu calls, %llu waited (latest - done fence %.1f on average)", address,
                std::chrono::duration<double, std::milli>(value.time).count() / seconds, (unsigned long long)value.calls,
                (unsigned long long)value.waited, value.waited ? double(value.behindSum) / double(value.waited) : 0.0);
        s_sites.clear();
    }
}

// One step of D3D's wait for the GPU (sub_8226D968: a short delay loop, then a check of the fence /
// progress; r3 = 1 while the caller must keep waiting). On the console it spins on its own hardware
// thread. Here that spin ran flat out on a host core while the command processor thread, which is
// what it waits for, worked on another: with the frame rate unlocked the game thread spends most of
// the frame in it, and the command processor took ~25% longer per frame than with a frame limit
// (where the game thread sleeps in the vblank wait instead): shared caches/memory bandwidth, the
// fence's cache line bouncing, boost clocks. Now each step that says "keep waiting" pauses ~2 us
// (_mm_pause) and every 32nd one gives the core away (SwitchToThread). The wait still ends within
// microseconds of the fence. CRASH_D3D_SPIN=busy: the original spin.
extern "C" PPC_FUNC(__imp__sub_8226D968);

#include <immintrin.h>

extern "C" __declspec(dllimport) int __stdcall SwitchToThread();

PPC_FUNC(sub_8226D968)
{
    static const bool busy = [] {
        const char* text = getenv("CRASH_D3D_SPIN");
        return text && !strcmp(text, "busy");
    }();
    __imp__sub_8226D968(ctx, base);
    if (busy || ctx.r3.u32 == 0)
        return;
    thread_local uint32_t t_steps;
    if (++t_steps % 32 == 0)
        SwitchToThread();
    else
        for (int i = 0; i < 16; i++)
            _mm_pause();
}

