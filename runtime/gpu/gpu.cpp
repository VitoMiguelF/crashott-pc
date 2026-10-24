#include "gpu.h"
#include "../settings/settings.h"
#include "renderer.h"
#include "../cpu/guest_thread.h"
#include "../diagnostics.h"
#include "../kernel/input.h"
#include "../kernel/memory.h"
#include "../kernel/threads.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace
{
    constexpr uint32_t TRAPPED_PAGE_REGISTERS = 0x1000 / 4;
    // The game presents every 2 vblanks (D3D presentation interval two), so the guest vblank rate
    // sets its frame cap: 60 Hz = the original 30 fps. The game times itself with real time, so a
    // faster vblank speeds up the frame rate, not the game (settings video.fps_limit, NOTES step 19).
    // CRASH_VBLANK_HZ overrides it.
    std::atomic<uint32_t> g_vblankHz{ 60 };
    // Frame limiter for the 60 and 120 fps limits (PaceFrame, in VdSwap): 0 = none.
    std::atomic<uint32_t> g_pacedFps{ 0 };

    std::chrono::nanoseconds VblankPeriod()
    {
        static const long forced = getenv("CRASH_VBLANK_HZ") ? std::max(1L, atol(getenv("CRASH_VBLANK_HZ"))) : 0;
        return std::chrono::nanoseconds(1'000'000'000 / (forced ? forced : g_vblankHz.load()));
    }

    constexpr auto STATS_PERIOD = std::chrono::seconds(5);  // 300 vblanks at 60 Hz
    constexpr uint32_t LOGGED_REGISTER_WRITES = 32;
    constexpr uint32_t LOGGED_SWAPS = 5;

    // Registers 0x000-0x3FF (the trapped first page of the window).
    std::atomic<uint32_t> g_trappedRegisters[TRAPPED_PAGE_REGISTERS];
    std::atomic<uint32_t> g_loggedRegisterWrites;

    // Guest address of the store being single-stepped on this thread, 0 if none.
    thread_local uint32_t t_pendingWrite;

    std::mutex g_interruptMutex;
    std::condition_variable g_interruptCondition;
    // The interrupt thread sleeps on a high-resolution waitable timer (the next vblank) and an
    // event (an interrupt request). condition_variable::wait_until rounds to whole milliseconds on
    // Windows, so with the sub-millisecond vblank (FAST_VBLANK_HZ) it returned at once and the
    // thread spun: ~70% of a core. Null handles (Windows before 10 1803): the condition variable.
    HANDLE g_interruptEvent;
    HANDLE g_vblankTimer;
    struct InterruptRequest
    {
        uint32_t source;
        uint32_t cpu;
        bool* done;  // set (under g_interruptMutex) once delivered, if someone waits
    };
    std::deque<InterruptRequest> g_interruptQueue;
    std::condition_variable g_interruptDelivered;
    std::atomic<uint32_t> g_interruptCallback;
    std::atomic<uint32_t> g_interruptUserData;
    std::atomic<uint64_t> g_vblanks;
    std::atomic<uint64_t> g_interruptsDelivered;

    std::atomic<uint64_t> g_vdSwaps;
    std::atomic<uint64_t> g_executedSwaps;

    void* TrappedPage()
    {
        return g_memory.Translate(GPU_MMIO_BEGIN);
    }

    void OnGuestRegisterWrite(uint32_t index, uint32_t value)
    {
        g_trappedRegisters[index] = value;

        if (index != gpu::REG_CP_RB_WPTR && g_loggedRegisterWrites.fetch_add(1) < LOGGED_REGISTER_WRITES)
            Log("[gpu] guest wrote register 0x%04X = 0x%08X", index, value);

        if (index == gpu::REG_CP_RB_WPTR)
            gpu::WakeCommandProcessor();
    }

    // Guest stores to the trapped page fault (it's read-only). The handler makes the page writable,
    // single-steps the store, then protects the page again and reports the value written. Another
    // thread writing while the page is briefly writable isn't seen; the command processor also
    // polls CP_RB_WPTR, so that only delays it.
    LONG CALLBACK MmioHandler(PEXCEPTION_POINTERS info)
    {
        auto* record = info->ExceptionRecord;

        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 &&
            record->ExceptionInformation[0] == 1)
        {
            auto* host = reinterpret_cast<void*>(record->ExceptionInformation[1]);
            if (!g_memory.IsInGuestRange(host))
                return EXCEPTION_CONTINUE_SEARCH;

            uint32_t guest = g_memory.MapVirtual(host);
            if (guest < GPU_MMIO_BEGIN || guest >= GPU_MMIO_BEGIN + 0x1000)
                return EXCEPTION_CONTINUE_SEARCH;

            t_pendingWrite = guest;
            DWORD oldProtect;
            VirtualProtect(TrappedPage(), 0x1000, PAGE_READWRITE, &oldProtect);
            info->ContextRecord->EFlags |= 0x100; // trap flag: single-step the store
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && t_pendingWrite)
        {
            uint32_t guest = t_pendingWrite & ~3u;
            t_pendingWrite = 0;

            DWORD oldProtect;
            VirtualProtect(TrappedPage(), 0x1000, PAGE_READONLY, &oldProtect);
            OnGuestRegisterWrite((guest - GPU_MMIO_BEGIN) / 4, LoadBE32(guest));
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    void LogStats()
    {
        Log("[gpu] vblank %llu: %llu VdSwap call(s), %llu swap packet(s) executed, %llu interrupt(s) delivered",
            (unsigned long long)g_vblanks.load(), (unsigned long long)g_vdSwaps.load(),
            (unsigned long long)g_executedSwaps.load(), (unsigned long long)g_interruptsDelivered.load());
        gpu::LogCommandProcessorStats();
        gpu::renderer::LogStats();
    }

    void DeliverInterrupt(PPCContext& ctx, uint32_t source, uint32_t cpu)
    {
        uint32_t callback = g_interruptCallback;
        if (!callback)
            return;

        // The callback runs as if on processor `cpu`.
        *g_memory.Translate(ctx.r13.u32 + 0x10C) = uint8_t(cpu);
        CallGuestFunction(ctx, callback, source, g_interruptUserData);
        g_interruptsDelivered++;
    }

    // CRASH_FPS_LIMIT_AT=<seconds>:<fps>: switches video.fps_limit once the replay's game clock (or,
    // without a replay, the input clock) reaches <seconds>. The benchmark (profile desempenho_vila,
    // tools/perf/bench.py) gets to the village at 30 fps, where the opening cutscene reads the
    // controller once per frame (so an unlocked replay would run ahead), and unlocks for gameplay.
    void CheckScheduledFrameLimit()
    {
        static double s_at = -1.0;
        static int s_fps = -1;
        static bool s_parsed = [] {
            if (const char* text = getenv("CRASH_FPS_LIMIT_AT"))
                if (sscanf(text, "%lf:%d", &s_at, &s_fps) != 2)
                    s_at = -1.0;
            return true;
        }();
        (void)s_parsed;
        if (s_at < 0.0 || s_fps < 0)
            return;
        input::RecordStatus status = input::Status();
        if (status.replaying && status.waiting)
            return;  // a SYNC/WAIT holds the replay's timeline: its time isn't the game's yet
        double now = status.replaying ? status.replaySeconds : double(input::ClockMs()) / 1000.0;
        if (now < s_at)
            return;
        Log("[gpu] CRASH_FPS_LIMIT_AT: fps limit %d at %.2f s (input clock %.2f s)", s_fps, now, double(input::ClockMs()) / 1000.0);
        gpu::SetFrameLimit(uint32_t(s_fps));
        s_at = -1.0;
    }

    void InterruptThread(PPCContext& ctx)
    {
        auto VBLANK_PERIOD = VblankPeriod();
        auto nextVblank = std::chrono::steady_clock::now() + VBLANK_PERIOD;
        auto nextStats = std::chrono::steady_clock::now() + STATS_PERIOD;

        g_vblankTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (g_vblankTimer)
            g_interruptEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Log("[gpu] interrupt thread waits on %s", g_vblankTimer && g_interruptEvent ? "a high-resolution timer" : "a condition variable");

        while (true)
        {
            if (g_vblankTimer && g_interruptEvent)
            {
                auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(nextVblank - std::chrono::steady_clock::now()).count();
                if (remaining > 0)
                {
                    LARGE_INTEGER due;
                    due.QuadPart = -std::max<LONGLONG>(1, remaining / 100);  // relative, 100 ns units
                    SetWaitableTimerEx(g_vblankTimer, &due, 0, nullptr, nullptr, nullptr, 0);
                    HANDLE handles[2] = { g_interruptEvent, g_vblankTimer };
                    WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                }
            }
            std::unique_lock lock(g_interruptMutex);
            if (!g_vblankTimer || !g_interruptEvent)
                g_interruptCondition.wait_until(lock, nextVblank, [] { return !g_interruptQueue.empty(); });

            while (!g_interruptQueue.empty())
            {
                InterruptRequest request = g_interruptQueue.front();
                g_interruptQueue.pop_front();
                lock.unlock();
                DeliverInterrupt(ctx, request.source, request.cpu);
                lock.lock();
                if (request.done)
                {
                    *request.done = true;
                    g_interruptDelivered.notify_all();
                }
            }
            lock.unlock();

            auto now = std::chrono::steady_clock::now();
            if (now < nextVblank)
                continue;

            VBLANK_PERIOD = VblankPeriod();
            nextVblank += VBLANK_PERIOD;
            if (now - nextVblank > VBLANK_PERIOD * 4)
                nextVblank = now + VBLANK_PERIOD; // fell behind (debugger, slow callback): resync

            ++g_vblanks;
            gpu::WriteRegister(gpu::REG_INTERRUPT_STATUS, 1);
            gpu::WriteRegister(gpu::REG_D1MODE_V_COUNTER, 0x2D0);
            DeliverInterrupt(ctx, gpu::INTERRUPT_SOURCE_VBLANK, 2);

            if (g_vblanks % 8 == 0)
                CheckScheduledFrameLimit();

            if (now >= nextStats)
            {
                nextStats = now + STATS_PERIOD;
                LogStats();
            }
        }
    }
}

namespace gpu
{
    void Init()
    {
        if (!g_memory.Commit(GPU_MMIO_BEGIN, GPU_MMIO_END - GPU_MMIO_BEGIN))
            Fatal("could not commit the GPU register window");

        // Values Xenia returns for registers titles read during init.
        WriteRegister(0x0F00, 0x08100748);         // RB_EDRAM_TIMING
        WriteRegister(0x0F01, 0x0000200E);         // RB_BC_CONTROL
        WriteRegister(REG_D1MODE_V_COUNTER, 0x2D0);
        WriteRegister(REG_INTERRUPT_STATUS, 1);    // vblank
        WriteRegister(0x1961, 0x050002D0);         // AVIVO_D1MODE_VIEWPORT_SIZE: 1280x720

        DWORD oldProtect;
        VirtualProtect(TrappedPage(), 0x1000, PAGE_READONLY, &oldProtect);
        AddVectoredExceptionHandler(1, MmioHandler);

        AddDiagnosticReporter(LogStats);
        SetFrameLimit(uint32_t(settings::Get().fpsLimit));
        renderer::Init();
        StartCommandProcessor();
        CreateRuntimeGuestThread("gpu interrupts", 2, InterruptThread);
        Log("[gpu] register window 0x%08X-0x%08X ready (first page trapped)", GPU_MMIO_BEGIN, GPU_MMIO_END);
    }

    uint32_t ReadRegister(uint32_t index)
    {
        if (index < TRAPPED_PAGE_REGISTERS)
            return g_trappedRegisters[index];
        return index < REGISTER_COUNT ? LoadBE32(GPU_MMIO_BEGIN + index * 4) : 0;
    }

    void WriteRegister(uint32_t index, uint32_t value)
    {
        if (index < TRAPPED_PAGE_REGISTERS)
            g_trappedRegisters[index] = value;
        else if (index < REGISTER_COUNT)
            StoreBE32(GPU_MMIO_BEGIN + index * 4, value);

        // Scratch registers are mirrored to memory when their SCRATCH_UMSK bit is set.
        if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8)
        {
            uint32_t scratch = index - REG_SCRATCH_REG0;
            if ((ReadRegister(REG_SCRATCH_UMSK) >> scratch) & 1)
                StoreBE32(GuestFromGpuAddress(ReadRegister(REG_SCRATCH_ADDR) + scratch * 4), value);
        }
    }

    void SetInterruptCallback(uint32_t callback, uint32_t userData)
    {
        g_interruptUserData = userData;
        g_interruptCallback = callback;
    }

    void RequestInterrupt(uint32_t source, uint32_t cpu, bool wait)
    {
        bool done = false;
        std::unique_lock lock(g_interruptMutex);
        g_interruptQueue.push_back({ source, cpu, wait ? &done : nullptr });
        g_interruptCondition.notify_one();
        if (g_interruptEvent)
            SetEvent(g_interruptEvent);
        if (wait)
            g_interruptDelivered.wait(lock, [&] { return done; });
    }

    void SetFrameLimit(uint32_t fps)
    {
        // 1. The swap (pure3d::xnDisplay::vf15, sub_8238EFE8) waits for 2 vblanks per frame. At 30
        //    fps (the original) that stays as on the console: a 60 Hz vblank. For 60 and 120 a
        //    vblank of 2x the limit quantized every frame to whole vblank pairs: one that took a bit
        //    more than 1/60 s waited for the next pair, 33.3 ms (measured: a third of the frames in
        //    the village on a CPU that makes ~65 fps unlocked, 56 fps on average). There the vblank
        //    runs at 1000 Hz, as without a limit, and VdSwap paces the frames (PaceFrame): a frame
        //    that is late costs only what it took. CRASH_FRAME_LIMITER=vblank: the old way.
        //    That fast vblank is 2000 Hz (FAST_VBLANK_HZ): at 1000 Hz the 2 vblanks per swap capped
        //    the frame rate at 500 fps and the command processor waited up to a vblank for D3D's
        //    swap callback at every frame (the game then waited for the command processor). With
        //    the single-pass scene the village runs ~460 fps: 1000 -> 2000 Hz gave 464 -> 494 fps,
        //    p99 3.7 -> 2.7 ms; 4000 Hz gave nothing more.
        static const bool vblankLimiter = [] {
            const char* text = getenv("CRASH_FRAME_LIMITER");
            return text && !strcmp(text, "vblank");
        }();
        bool paced = (fps == 60 || fps == 120) && !vblankLimiter;
        constexpr uint32_t FAST_VBLANK_HZ = 2000;
        uint32_t hz = fps && !paced ? fps * 2 : FAST_VBLANK_HZ;
        g_pacedFps = paced ? fps : 0;
        // 2. The main loop (CUberManager::vf10, sub_821FC198) adds up dt and only updates and draws
        //    once 1/60 s has gone by: the float at 0x8201EF70, read only there. Above 60 fps it
        //    becomes 1/fps (1/1000 s without a limit).
        constexpr uint32_t UPDATE_THRESHOLD = 0x8201EF70;
        constexpr float ORIGINAL_THRESHOLD = 1.0f / 60.0f;
        static const bool s_thresholdFound = [] {
            uint32_t bits = LoadBE32(UPDATE_THRESHOLD);
            float value;
            memcpy(&value, &bits, 4);
            if (value != ORIGINAL_THRESHOLD)
                Log("[gpu] frame limit: the update threshold at 0x%08X is %g, not 1/60; frame rate stays at most 60", UPDATE_THRESHOLD, value);
            return value == ORIGINAL_THRESHOLD;
        }();
        float threshold = fps > 60 ? 1.0f / float(fps) : fps ? ORIGINAL_THRESHOLD : 0.001f;
        if (s_thresholdFound)
        {
            uint32_t bits;
            memcpy(&bits, &threshold, 4);
            StoreBE32(UPDATE_THRESHOLD, bits);
        }
        if (g_vblankHz.exchange(hz) != hz)
            Log("[gpu] frame limit %s: guest vblank at %u Hz, update threshold %.4f s%s", fps ? (std::to_string(fps) + " fps").c_str() : "off",
                hz, double(s_thresholdFound ? threshold : ORIGINAL_THRESHOLD), paced ? ", paced in VdSwap" : "");
    }

    void PaceFrame()
    {
        uint32_t fps = g_pacedFps;
        static std::chrono::steady_clock::time_point s_next;
        auto now = std::chrono::steady_clock::now();
        if (!fps)
        {
            s_next = {};
            return;
        }
        auto period = std::chrono::nanoseconds(1'000'000'000 / fps);
        // Late by more than a frame (a load, a hitch, the limit just changed): start over from now
        // instead of letting the next frames run fast to catch up.
        if (s_next == std::chrono::steady_clock::time_point{} || now - s_next > period)
        {
            s_next = now + period;
            return;
        }
        if (now < s_next)
        {
            // Sleep (1 ms timer resolution, timeBeginPeriod in main) to ~1 ms before, then spin.
            auto sleep = s_next - now - std::chrono::microseconds(1200);
            if (sleep > std::chrono::nanoseconds(0))
                std::this_thread::sleep_for(sleep);
            while (std::chrono::steady_clock::now() < s_next)
                YieldProcessor();
        }
        s_next += period;
    }

    uint64_t VblankCount()
    {
        return g_vblanks;
    }

    void CountVdSwap(uint32_t frontBuffer, uint32_t width, uint32_t height)
    {
        uint64_t swap = ++g_vdSwaps;
        if (swap <= LOGGED_SWAPS || swap % 60 == 0)
        {
            Log("[gpu] VdSwap #%llu: front buffer 0x%08X, %ux%u (vblank %llu)",
                (unsigned long long)swap, frontBuffer, width, height, (unsigned long long)g_vblanks.load());
        }
    }

    void CountExecutedSwap()
    {
        g_executedSwaps++;
    }
}
