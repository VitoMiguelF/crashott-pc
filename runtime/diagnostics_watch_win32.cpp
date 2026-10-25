// CRASH_WATCH="0xA18BF5B0,..." (up to 4 guest addresses) from CRASH_WATCH_TIME seconds of the input
// clock (default 0): hardware write breakpoints (DR0-DR3) on every thread; each write logs the new
// value (as u32 and float), the guest function that wrote it and the guest functions found on the
// host stack (the callers), the first 12 times per address.
#include "diagnostics.h"
#include "kernel/input.h"
#include "kernel/memory.h"
#include "platform/platform.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
            uint32_t value = __builtin_bswap32(*reinterpret_cast<uint32_t*>(g_memory.base + g_watch[i]));
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
                    plat::SleepMs(100);
                Log("[watch] %u address(es) armed at %.1f s", g_watchCount, double(input::ClockMs()) / 1000.0);
                while (true)
                {
                    ArmAllThreads();
                    plat::SleepMs(1000);
                }
            }).detach();
        }
    } g_watchStarter;
}
