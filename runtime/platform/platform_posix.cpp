// POSIX implementation of platform.h (Linux / Steam Deck; Android later). Not built on Windows.
#include "platform.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#if defined(__x86_64__)
#define CRASH_FAULT_X86_64 1
#endif

namespace plat
{
    // ---- Time -------------------------------------------------------------------------------
    uint64_t PerfCounter()
    {
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return uint64_t(now.tv_sec) * 1000000000ull + uint64_t(now.tv_nsec);
    }

    uint64_t PerfFrequency()
    {
        return 1000000000ull;
    }

    uint64_t TickCountMs()
    {
        return PerfCounter() / 1000000ull;
    }

    uint64_t SystemTimeFileTime()
    {
        constexpr uint64_t UNIX_EPOCH_AS_FILETIME = 116444736000000000ull;
        timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        return UNIX_EPOCH_AS_FILETIME + uint64_t(now.tv_sec) * 10000000ull + uint64_t(now.tv_nsec) / 100;
    }

    void SleepMs(uint32_t milliseconds)
    {
        timespec duration{ time_t(milliseconds / 1000), long(milliseconds % 1000) * 1000000L };
        while (nanosleep(&duration, &duration) != 0 && errno == EINTR)
        {
        }
    }

    void YieldThread()
    {
        sched_yield();
    }

    void RaiseTimerResolution()
    {
        // Linux timers already have (sub-)millisecond resolution.
    }

    // Condition variable waits use hrtimers on Linux: no separate timer needed.
    struct TimerWait
    {
    };

    TimerWait* CreateTimerWait()
    {
        return nullptr;
    }

    void WaitTimer(TimerWait*, int64_t)
    {
    }

    void WakeTimer(TimerWait*)
    {
    }

    // ---- Threads ----------------------------------------------------------------------------
    namespace
    {
        // Win32-style suspended creation: the thread waits on its gate until ResumeThread.
        struct PosixThread
        {
            pthread_t thread{};
            ThreadEntry entry{};
            void* param{};
            pid_t tid{};
            std::mutex mutex;
            std::condition_variable resumed;
            uint32_t suspendCount{};
            std::atomic<bool> started{};
        };

        thread_local PosixThread* t_thread;

        void* ThreadProc(void* param)
        {
            auto* thread = static_cast<PosixThread*>(param);
            t_thread = thread;
            {
                std::unique_lock lock(thread->mutex);
                thread->tid = pid_t(syscall(SYS_gettid));
                thread->started = true;
                thread->resumed.notify_all();
                thread->resumed.wait(lock, [&] { return thread->suspendCount == 0; });
            }
            uint32_t exitCode = thread->entry(thread->param);
            return reinterpret_cast<void*>(uintptr_t(exitCode));
        }

        // Nice values for the calling thread: Linux has per-thread nice levels (setpriority on the
        // tid). Raising priority needs CAP_SYS_NICE or an rlimit; failure is fine.
        void ApplyPriority(pid_t tid, ThreadPriority priority)
        {
            int nice = priority == ThreadPriority::Highest ? -10 : priority == ThreadPriority::AboveNormal ? -5
                     : priority == ThreadPriority::Lowest ? 10 : 0;
            setpriority(PRIO_PROCESS, id_t(tid), nice);
        }
    }

    ThreadHandle CreateThread(ThreadEntry entry, void* param, size_t stackSize, bool suspended, uint32_t* threadId)
    {
        auto* thread = new PosixThread();
        thread->entry = entry;
        thread->param = param;
        thread->suspendCount = suspended ? 1 : 0;

        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        if (stackSize)
            pthread_attr_setstacksize(&attributes, stackSize);
        int result = pthread_create(&thread->thread, &attributes, ThreadProc, thread);
        pthread_attr_destroy(&attributes);
        if (result != 0)
        {
            delete thread;
            errno = result;
            return nullptr;
        }

        // The id is the kernel tid, known once the thread runs.
        std::unique_lock lock(thread->mutex);
        thread->resumed.wait(lock, [&] { return thread->started.load(); });
        if (threadId)
            *threadId = uint32_t(thread->tid);
        return thread;
    }

    uint32_t ResumeThread(ThreadHandle handle)
    {
        auto* thread = static_cast<PosixThread*>(handle);
        std::lock_guard lock(thread->mutex);
        uint32_t previous = thread->suspendCount;
        if (thread->suspendCount && --thread->suspendCount == 0)
            thread->resumed.notify_all();
        return previous;
    }

    void JoinThread(ThreadHandle handle)
    {
        pthread_join(static_cast<PosixThread*>(handle)->thread, nullptr);
    }

    void ExitThread(uint32_t exitCode)
    {
        pthread_exit(reinterpret_cast<void*>(uintptr_t(exitCode)));
    }

    void SetThreadPriority(ThreadHandle handle, ThreadPriority priority)
    {
        ApplyPriority(static_cast<PosixThread*>(handle)->tid, priority);
    }

    void SetCurrentThreadPriority(ThreadPriority priority)
    {
        ApplyPriority(pid_t(syscall(SYS_gettid)), priority);
    }

    void SetCurrentThreadName(const char* name)
    {
        char shortName[16]; // the kernel keeps 15 characters
        snprintf(shortName, sizeof(shortName), "%s", name);
        pthread_setname_np(pthread_self(), shortName);
    }

    uint32_t CurrentThreadId()
    {
        return uint32_t(syscall(SYS_gettid));
    }

    // ---- Fibers -----------------------------------------------------------------------------
    // ucontext: glibc (x86-64, aarch64) has it; Android's bionic does not, so Android needs a
    // small assembly switch instead (see linux_port.md). swapcontext saves the FPU control words,
    // like FIBER_FLAG_FLOAT_SWITCH.
    namespace
    {
        struct PosixFiber
        {
            ucontext_t context{};
            void* stack{};
            size_t stackSize{};
            void (*entry)(void*){};
            void* param{};
        };

        thread_local PosixFiber* t_currentFiber;

        void FiberMain(unsigned high, unsigned low)
        {
            auto* fiber = reinterpret_cast<PosixFiber*>((uintptr_t(high) << 32) | uintptr_t(low));
            fiber->entry(fiber->param);
            std::abort(); // fiber routines never return (kernel/fibers.cpp)
        }
    }

    FiberHandle CurrentThreadFiber()
    {
        if (!t_currentFiber)
            t_currentFiber = new PosixFiber(); // its context is filled by the first switch away
        return t_currentFiber;
    }

    FiberHandle CreateFiber(size_t stackSize, void (*entry)(void* param), void* param)
    {
        auto* fiber = new PosixFiber();
        fiber->entry = entry;
        fiber->param = param;
        fiber->stackSize = stackSize;
        // Reserved, not committed: Linux maps stack pages on first touch.
        fiber->stack = mmap(nullptr, stackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (fiber->stack == MAP_FAILED)
        {
            delete fiber;
            return nullptr;
        }
        getcontext(&fiber->context);
        fiber->context.uc_stack.ss_sp = fiber->stack;
        fiber->context.uc_stack.ss_size = stackSize;
        fiber->context.uc_link = nullptr;
        uintptr_t pointer = reinterpret_cast<uintptr_t>(fiber);
        makecontext(&fiber->context, reinterpret_cast<void (*)()>(FiberMain), 2, unsigned(pointer >> 32), unsigned(pointer));
        return fiber;
    }

    void SwitchToFiber(FiberHandle handle)
    {
        PosixFiber* from = static_cast<PosixFiber*>(CurrentThreadFiber());
        auto* to = static_cast<PosixFiber*>(handle);
        t_currentFiber = to;
        swapcontext(&from->context, &to->context);
        // Back on `from`, maybe on another thread: that thread's t_currentFiber was set by the
        // switch that resumed us.
    }

    void DeleteFiber(FiberHandle handle)
    {
        auto* fiber = static_cast<PosixFiber*>(handle);
        if (fiber->stack)
            munmap(fiber->stack, fiber->stackSize);
        delete fiber;
    }

    // ---- Virtual memory ---------------------------------------------------------------------
    // Linux has no reserved/committed distinction to query, so the reservations made here keep a
    // committed-page bitmap (1 bit per 4 KB page) for QueryMemory.
    namespace
    {
        constexpr size_t PAGE = 0x1000;

        struct Reservation
        {
            uint8_t* base{};
            size_t size{};
            std::vector<std::atomic<uint64_t>>* committed{};
            std::vector<std::pair<size_t, size_t>> views; // always committed [offset, size)
        };

        std::mutex g_reservationsMutex;
        std::vector<Reservation> g_reservations;

        Reservation* FindReservation(const void* address)
        {
            auto* p = static_cast<const uint8_t*>(address);
            for (Reservation& r : g_reservations)
            {
                if (p >= r.base && p < r.base + r.size)
                    return &r;
            }
            return nullptr;
        }

        void MarkCommitted(void* address, size_t size, bool committed)
        {
            Reservation* r = FindReservation(address);
            if (!r || !size)
                return;
            size_t first = size_t(static_cast<uint8_t*>(address) - r->base) / PAGE;
            size_t last = (size_t(static_cast<uint8_t*>(address) - r->base) + size - 1) / PAGE;
            for (size_t page = first; page <= last; page++)
            {
                uint64_t bit = 1ull << (page & 63);
                if (committed)
                    (*r->committed)[page / 64].fetch_or(bit, std::memory_order_relaxed);
                else
                    (*r->committed)[page / 64].fetch_and(~bit, std::memory_order_relaxed);
            }
        }

        bool IsCommitted(const Reservation& r, size_t page)
        {
            for (auto [offset, size] : r.views)
            {
                if (page * PAGE >= offset && page * PAGE < offset + size)
                    return true;
            }
            return ((*r.committed)[page / 64].load(std::memory_order_relaxed) >> (page & 63)) & 1;
        }

        uint8_t* MapReservation(size_t size, uintptr_t preferred)
        {
            void* p = MAP_FAILED;
#ifdef MAP_FIXED_NOREPLACE
            if (preferred)
                p = mmap(reinterpret_cast<void*>(preferred), size, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
#endif
            if (p == MAP_FAILED)
                p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
        }

        void Register(uint8_t* base, size_t size, std::vector<std::pair<size_t, size_t>> views)
        {
            std::lock_guard lock(g_reservationsMutex);
            Reservation r;
            r.base = base;
            r.size = size;
            r.committed = new std::vector<std::atomic<uint64_t>>((size / PAGE + 63) / 64);
            r.views = std::move(views);
            g_reservations.push_back(std::move(r));
        }

        int NativeAccess(PageAccess access)
        {
            switch (access)
            {
            case PageAccess::ReadOnly: return PROT_READ;
            case PageAccess::ReadWrite: return PROT_READ | PROT_WRITE;
            default: return PROT_NONE;
            }
        }
    }

    uint8_t* ReserveMemory(size_t size, uintptr_t preferred)
    {
        uint8_t* base = MapReservation(size, preferred);
        if (base)
            Register(base, size, {});
        return base;
    }

    uint8_t* ReserveMemoryWithViews(size_t size, uintptr_t preferred, size_t sectionSize, const size_t* viewOffsets, size_t viewCount)
    {
        int section = memfd_create("crashrecomp-physical", MFD_CLOEXEC);
        if (section < 0)
            return nullptr;
        if (ftruncate(section, off_t(sectionSize)) != 0)
        {
            close(section);
            return nullptr;
        }
        uint8_t* base = MapReservation(size, preferred);
        bool ok = base != nullptr;
        std::vector<std::pair<size_t, size_t>> views;
        for (size_t i = 0; ok && i < viewCount; i++)
        {
            ok = mmap(base + viewOffsets[i], sectionSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, section, 0) != MAP_FAILED;
            views.push_back({ viewOffsets[i], sectionSize });
        }
        close(section); // the mappings keep it alive
        if (!ok)
        {
            if (base)
                munmap(base, size);
            return nullptr;
        }
        Register(base, size, std::move(views));
        return base;
    }

    bool CommitMemory(void* address, size_t size)
    {
        if (mprotect(address, size, PROT_READ | PROT_WRITE) != 0)
            return false;
        MarkCommitted(address, size, true);
        return true;
    }

    bool DecommitMemory(void* address, size_t size)
    {
        // Drop the pages (the next touch reads zeros) and make them inaccessible again.
        madvise(address, size, MADV_DONTNEED);
        if (mprotect(address, size, PROT_NONE) != 0)
            return false;
        MarkCommitted(address, size, false);
        return true;
    }

    bool ProtectMemory(void* address, size_t size, PageAccess access)
    {
        return mprotect(address, size, NativeAccess(access)) == 0;
    }

    MemoryRegion QueryMemory(const void* address)
    {
        MemoryRegion region;
        std::lock_guard lock(g_reservationsMutex);
        Reservation* r = FindReservation(address);
        if (!r)
        {
            region.base = reinterpret_cast<void*>(uintptr_t(address) & ~uintptr_t(PAGE - 1));
            region.size = PAGE;
            region.state = PageState::Free;
            return region;
        }
        size_t page = size_t(static_cast<const uint8_t*>(address) - r->base) / PAGE;
        bool committed = IsCommitted(*r, page);
        size_t last = page + 1;
        while (last < r->size / PAGE && IsCommitted(*r, last) == committed)
            last++;
        region.base = r->base + page * PAGE;
        region.size = (last - page) * PAGE;
        region.state = committed ? PageState::Committed : PageState::Reserved;
        return region;
    }

    // ---- Fault handling ---------------------------------------------------------------------
#if CRASH_FAULT_X86_64
    namespace
    {
        // x86 encoding order -> glibc's mcontext gregs index.
        constexpr int GREG_OF_X86[16] = { REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                          REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15 };

        greg_t* Gregs(void* native)
        {
            return static_cast<ucontext_t*>(native)->uc_mcontext.gregs;
        }
    }

    uint64_t FaultInfo::InstructionPointer() const
    {
        return uint64_t(Gregs(native)[REG_RIP]);
    }

    void FaultInfo::SetInstructionPointer(uint64_t ip)
    {
        Gregs(native)[REG_RIP] = greg_t(ip);
    }

    uint64_t FaultInfo::Register(unsigned x86Index) const
    {
        return uint64_t(Gregs(native)[GREG_OF_X86[x86Index & 15]]);
    }

    void FaultInfo::RequestSingleStep()
    {
        Gregs(native)[REG_EFL] |= 0x100; // trap flag: SIGTRAP after the next instruction
    }
#else
    // ARM64 (Android): no single-step from user space and a different store decoder; the MMIO
    // traps need another design there (linux_port.md).
    uint64_t FaultInfo::InstructionPointer() const { return 0; }
    void FaultInfo::SetInstructionPointer(uint64_t) {}
    uint64_t FaultInfo::Register(unsigned) const { return 0; }
    void FaultInfo::RequestSingleStep() {}
#endif

    namespace
    {
        constexpr size_t MAX_HANDLERS = 16;
        std::array<std::atomic<FaultHandler>, MAX_HANDLERS> g_handlers;
        std::atomic<size_t> g_handlerCount;
        struct sigaction g_previousSegv, g_previousBus, g_previousTrap;

        void Chain(int signal, siginfo_t* info, void* context, const struct sigaction& previous)
        {
            if (previous.sa_flags & SA_SIGINFO)
            {
                if (previous.sa_sigaction)
                {
                    previous.sa_sigaction(signal, info, context);
                    return;
                }
            }
            else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN)
            {
                previous.sa_handler(signal);
                return;
            }
            // Default action: restore it and return, so the fault repeats and kills the process.
            sigaction(signal, &previous, nullptr);
        }

        void SignalHandler(int signal, siginfo_t* info, void* context)
        {
            FaultInfo fault;
            fault.code = uint32_t(signal);
            fault.native = context;
            if (signal == SIGTRAP)
            {
                fault.kind = FaultKind::SingleStep;
            }
            else
            {
                fault.kind = FaultKind::AccessViolation;
                fault.address = info->si_addr;
#if CRASH_FAULT_X86_64
                fault.write = (Gregs(context)[REG_ERR] & 2) != 0; // page-fault error code: W bit
#endif
            }

            for (size_t i = g_handlerCount.load(); i-- > 0;)
            {
                if (g_handlers[i].load()(fault))
                    return;
            }
            Chain(signal, info, context, signal == SIGSEGV ? g_previousSegv : signal == SIGBUS ? g_previousBus : g_previousTrap);
        }

        void InstallSignalHandlers()
        {
            // Faults can happen on any guest thread, deep in recompiled code: an alternate stack
            // isn't needed (64 MB thread stacks) but the handlers must be async-signal-safe-ish:
            // they only touch the handler table and mprotect.
            struct sigaction action{};
            action.sa_sigaction = SignalHandler;
            action.sa_flags = SA_SIGINFO | SA_NODEFER;
            sigemptyset(&action.sa_mask);
            sigaction(SIGSEGV, &action, &g_previousSegv);
            sigaction(SIGBUS, &action, &g_previousBus);
            sigaction(SIGTRAP, &action, &g_previousTrap);
        }
    }

    void AddFaultHandler(FaultHandler handler)
    {
        static std::once_flag s_installed;
        static std::mutex s_mutex;
        std::lock_guard lock(s_mutex);
        size_t index = g_handlerCount.load();
        if (index == MAX_HANDLERS)
            return;
        g_handlers[index] = handler;
        g_handlerCount = index + 1;
        std::call_once(s_installed, InstallSignalHandlers);
    }

    // ---- Dynamic libraries ------------------------------------------------------------------
    namespace
    {
        thread_local std::string t_dlError;
    }

    void* OpenLibrary(const std::filesystem::path& path)
    {
        void* library = nullptr;
        if (!path.has_parent_path())
        {
            // Next to the exe first (the bundled FFmpeg/DXC/FSR .so files), then the loader path.
            std::filesystem::path local = ExecutablePath().parent_path() / path;
            std::error_code error;
            if (std::filesystem::exists(local, error))
                library = dlopen(local.c_str(), RTLD_NOW | RTLD_LOCAL);
        }
        if (!library)
            library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!library)
        {
            const char* error = dlerror();
            t_dlError = error ? error : "dlopen failed";
        }
        return library;
    }

    void* FindSymbol(void* library, const char* name)
    {
        return dlsym(library, name);
    }

    void CloseLibrary(void* library)
    {
        dlclose(library);
    }

    std::string LastErrorText()
    {
        if (!t_dlError.empty())
        {
            std::string text = std::move(t_dlError);
            t_dlError.clear();
            return text;
        }
        return strerror(errno);
    }

    // ---- Process ----------------------------------------------------------------------------
    std::filesystem::path ExecutablePath()
    {
        std::error_code error;
        return std::filesystem::read_symlink("/proc/self/exe", error);
    }

    uint32_t CurrentProcessId()
    {
        return uint32_t(getpid());
    }

    std::vector<std::string> CommandLineArguments()
    {
        std::vector<std::string> arguments;
        FILE* file = fopen("/proc/self/cmdline", "rb");
        if (!file)
            return arguments;
        std::string current;
        bool first = true;
        for (int c; (c = fgetc(file)) != EOF;)
        {
            if (c != 0)
            {
                current += char(c);
                continue;
            }
            if (!first)
                arguments.push_back(current);
            first = false;
            current.clear();
        }
        fclose(file);
        return arguments;
    }

    uint32_t StartSelf(const std::vector<std::string>& arguments)
    {
        std::string exe = ExecutablePath().string();
        std::vector<char*> argv;
        argv.push_back(exe.data());
        std::vector<std::string> copies = arguments;
        for (std::string& argument : copies)
            argv.push_back(argument.data());
        argv.push_back(nullptr);
        pid_t pid = fork();
        if (pid == 0)
        {
            execv(exe.c_str(), argv.data());
            _exit(127);
        }
        return pid > 0 ? uint32_t(pid) : 0;
    }

    bool WaitForProcessExit(uint32_t pid, uint32_t timeoutMs)
    {
        if (!pid)
            return true;
        uint64_t end = TickCountMs() + timeoutMs;
        while (kill(pid_t(pid), 0) == 0 || errno == EPERM)
        {
            if (TickCountMs() >= end)
                return false;
            SleepMs(20);
        }
        return true;
    }

    void SetEnvironment(const char* name, const char* value)
    {
        if (value)
            setenv(name, value, 1);
        else
            unsetenv(name);
    }

    bool DisablePowerThrottling()
    {
        return true; // nothing like EcoQoS for background windows
    }

    // ---- Dialogs ----------------------------------------------------------------------------
    // Until the SDL window exists there is no toolkit: zenity (GNOME, SteamOS desktop) or kdialog
    // (KDE), else stderr. The SDL backend can use SDL_ShowSimpleMessageBox once it is up.
    namespace
    {
        std::string ShellQuote(const std::string& text)
        {
            std::string quoted = "'";
            for (char c : text)
                quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
            return quoted + "'";
        }

        bool HasTool(const char* name)
        {
            std::string command = std::string("command -v ") + name + " >/dev/null 2>&1";
            return system(command.c_str()) == 0;
        }
    }

    void ShowMessageBox(const char* title, const char* text, MessageKind kind, void*)
    {
        fprintf(stderr, "[%s] %s: %s\n", kind == MessageKind::Error ? "error" : kind == MessageKind::Warning ? "warning" : "info",
                title ? title : "", text ? text : "");
        std::string command;
        if (HasTool("zenity"))
            command = std::string("zenity ") + (kind == MessageKind::Error ? "--error" : kind == MessageKind::Warning ? "--warning" : "--info") +
                      " --title=" + ShellQuote(title) + " --text=" + ShellQuote(text) + " >/dev/null 2>&1";
        else if (HasTool("kdialog"))
            command = std::string("kdialog ") + (kind == MessageKind::Error ? "--error " : kind == MessageKind::Warning ? "--sorry " : "--msgbox ") +
                      ShellQuote(text) + " --title " + ShellQuote(title) + " >/dev/null 2>&1";
        if (!command.empty())
            (void)system(command.c_str());
    }

    std::filesystem::path PickFolder(const char* title, void*)
    {
        std::string command;
        if (HasTool("zenity"))
            command = "zenity --file-selection --directory --title=" + ShellQuote(title) + " 2>/dev/null";
        else if (HasTool("kdialog"))
            command = "kdialog --getexistingdirectory ~ --title " + ShellQuote(title) + " 2>/dev/null";
        else
            return {};
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe)
            return {};
        std::string output;
        char buffer[512];
        while (fgets(buffer, sizeof(buffer), pipe))
            output += buffer;
        pclose(pipe);
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
            output.pop_back();
        return output.empty() ? std::filesystem::path() : std::filesystem::path(output);
    }

    int CompareNoCase(const char* a, const char* b)
    {
        return strcasecmp(a, b);
    }

    void ExitProcess(uint32_t exitCode)
    {
        fflush(nullptr);
        _exit(int(exitCode));
    }
}
