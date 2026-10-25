#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Thin OS layer: the runtime calls these instead of Win32 or POSIX directly, so the same code
// builds for Windows (platform_win32.cpp), Linux / Steam Deck and later Android
// (platform_posix.cpp). Window, input and audio sit behind their own interfaces (gpu/window.h,
// kernel/input.h, audio/audio_output.h) with a Win32 and an SDL3 backend.
// See docs/internals/linux_port.md for what is ported and what remains.
namespace plat
{
    // ---- Time -------------------------------------------------------------------------------
    uint64_t PerfCounter();        // high-resolution monotonic ticks (QueryPerformanceCounter)
    uint64_t PerfFrequency();      // PerfCounter ticks per second
    uint64_t TickCountMs();        // monotonic milliseconds (GetTickCount64)
    uint64_t SystemTimeFileTime(); // UTC as an NT FILETIME: 100 ns units since 1601-01-01
    void SleepMs(uint32_t milliseconds);
    void YieldThread();            // SwitchToThread / sched_yield
    // 1 ms scheduler/timer granularity for the whole process (timeBeginPeriod(1)); no-op where the
    // OS already has it.
    void RaiseTimerResolution();

    // Wakeable sub-millisecond wait (the vblank thread, gpu/gpu.cpp): a high-resolution waitable
    // timer plus an event on Windows. CreateTimerWait returns nullptr where a condition variable
    // wait is already that precise (POSIX), and the caller uses one.
    struct TimerWait;
    TimerWait* CreateTimerWait();
    void WaitTimer(TimerWait* timer, int64_t nanoseconds); // returns early on WakeTimer
    void WakeTimer(TimerWait* timer);

    inline double PerfSeconds(uint64_t from, uint64_t to)
    {
        return double(int64_t(to - from)) / double(PerfFrequency());
    }

    // Spin-wait hint (YieldProcessor): x86 `pause`, ARM `yield`.
    inline void CpuRelax()
    {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield");
#endif
    }

    // ---- Threads ----------------------------------------------------------------------------
    using ThreadHandle = void*;
    using ThreadEntry = uint32_t (*)(void* param);
    enum class ThreadPriority { Lowest, Normal, AboveNormal, Highest };

    // `stackSize` is the reservation (recompiled code nests one host frame per guest call, so
    // guest threads ask for 64 MB). A suspended thread starts with ResumeThread.
    ThreadHandle CreateThread(ThreadEntry entry, void* param, size_t stackSize, bool suspended, uint32_t* threadId);
    // Returns the previous suspend count (0 if it was running).
    uint32_t ResumeThread(ThreadHandle thread);
    void JoinThread(ThreadHandle thread);
    [[noreturn]] void ExitThread(uint32_t exitCode);
    void SetThreadPriority(ThreadHandle thread, ThreadPriority priority);
    void SetCurrentThreadPriority(ThreadPriority priority);
    void SetCurrentThreadName(const char* name);
    uint32_t CurrentThreadId();

    // ---- Fibers (host side of the guest's XAPI fibers, kernel/fibers.cpp) --------------------
    using FiberHandle = void*;
    // The calling thread as a fiber (converted on the first call). Floating-point state is
    // switched with the fiber.
    FiberHandle CurrentThreadFiber();
    FiberHandle CreateFiber(size_t stackSize, void (*entry)(void* param), void* param);
    void SwitchToFiber(FiberHandle fiber);
    void DeleteFiber(FiberHandle fiber);

    // ---- Virtual memory ---------------------------------------------------------------------
    enum class PageAccess { None, ReadOnly, ReadWrite };
    enum class PageState { Free, Reserved, Committed };

    // Reserves `size` bytes of address space (no access), at `preferred` if possible.
    uint8_t* ReserveMemory(size_t size, uintptr_t preferred);
    // Same, with `viewCount` read-write views of one shared `sectionSize`-byte section at the
    // given offsets (sorted, non-overlapping): writes through one view show in the others, like
    // the console's physical memory views (kernel/memory.h). nullptr if the OS can't do it.
    uint8_t* ReserveMemoryWithViews(size_t size, uintptr_t preferred, size_t sectionSize, const size_t* viewOffsets, size_t viewCount);
    // Read-write, zero-filled on first touch. Must not be called on a view of a shared section.
    bool CommitMemory(void* address, size_t size);
    // Back to reserved; the next commit reads zeros.
    bool DecommitMemory(void* address, size_t size);
    bool ProtectMemory(void* address, size_t size, PageAccess access);
    struct MemoryRegion
    {
        void* base{};
        size_t size{};   // pages from `base` in the same state
        PageState state{};
    };
    MemoryRegion QueryMemory(const void* address);

    // ---- Fault handling ---------------------------------------------------------------------
    // Host faults the runtime turns into features: demand-committed guest pages, the GPU and XMA
    // register windows (write-trapped pages), texture invalidation. A handler returns true when it
    // fixed the cause and execution should resume; handlers run newest first.
    enum class FaultKind { AccessViolation, SingleStep, Other };
    struct FaultInfo
    {
        FaultKind kind{};
        void* address{};   // the faulting data address (AccessViolation)
        bool write{};
        uint32_t code{};   // OS exception code / signal number, for reports
        void* native{};    // CONTEXT* (Windows) or ucontext_t* (POSIX)

        // x86-64 only for now (ARM64 needs its own decoder; see linux_port.md):
        uint64_t InstructionPointer() const;
        void SetInstructionPointer(uint64_t ip);
        uint64_t Register(unsigned x86Index) const; // rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8..r15
        void RequestSingleStep();                   // trap after the faulting instruction runs
    };
    using FaultHandler = bool (*)(FaultInfo& fault);
    void AddFaultHandler(FaultHandler handler);

    // ---- Dynamic libraries ------------------------------------------------------------------
    // A full path, or a bare file name ("vulkan-1.dll", "libvulkan.so.1") looked up the OS way
    // (the exe's folder first on Windows; the exe's folder, then the loader path on POSIX). A
    // library's own dependencies are searched next to it. nullptr on failure (LastErrorText).
    void* OpenLibrary(const std::filesystem::path& path);
    void* FindSymbol(void* library, const char* name);
    void CloseLibrary(void* library);
    // The last OS error of this thread as text (GetLastError / errno / dlerror).
    std::string LastErrorText();

    // ---- Process ----------------------------------------------------------------------------
    std::filesystem::path ExecutablePath();
    uint32_t CurrentProcessId();
    // This process's arguments after argv[0], as UTF-8.
    std::vector<std::string> CommandLineArguments();
    // Starts this executable again with `arguments`, the same environment, console and working
    // directory (the settings screen's restart). Returns the new process id, 0 on failure.
    uint32_t StartSelf(const std::vector<std::string>& arguments);
    // Waits up to `timeoutMs` for process `pid` to exit. True if it has (or never existed).
    bool WaitForProcessExit(uint32_t pid, uint32_t timeoutMs);
    void SetEnvironment(const char* name, const char* value); // nullptr removes it
    // Keeps background windows at full speed (Windows 11 power throttling / EcoQoS). False if the
    // OS refused; true where there is nothing to do.
    bool DisablePowerThrottling();
    [[noreturn]] void ExitProcess(uint32_t exitCode);

    // ---- Dialogs (before the game window exists, or over it: `owner` is the native window) ----
    enum class MessageKind { Info, Warning, Error };
    void ShowMessageBox(const char* title, const char* text, MessageKind kind = MessageKind::Error, void* owner = nullptr);
    // The OS folder picker. Empty if cancelled or there is none (POSIX: zenity / kdialog).
    std::filesystem::path PickFolder(const char* title, void* owner = nullptr);

    // ---- Small libc differences ---------------------------------------------------------------
    int CompareNoCase(const char* a, const char* b); // _stricmp / strcasecmp (ASCII)
}
