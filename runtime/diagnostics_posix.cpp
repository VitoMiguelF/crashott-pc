// The POSIX side of diagnostics.cpp (diagnostics_host.h): crash reports from the platform fault
// handler (SIGSEGV / SIGBUS), dladdr for host addresses. No thread sampling for the watchdog yet
// (it would take a signal to the thread and its ucontext).
#include "diagnostics.h"
#include "diagnostics_host.h"
#include "symbols.h"
#include "kernel/memory.h"
#include "platform/platform.h"

#include <dlfcn.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace
{
    bool CrashHandler(plat::FaultInfo& fault)
    {
        if (fault.kind != plat::FaultKind::AccessViolation)
            return false;
        uintptr_t rip = fault.InstructionPointer();
        char location[1024];
        diag::DescribeHostAddress(rip, location, sizeof(location));
        char thread[17] = {};
        prctl(PR_GET_NAME, thread, 0, 0, 0);
        Log("[crash] access violation at host rip %p, %s, thread %s", reinterpret_cast<void*>(rip), location, thread);
        void* host = reinterpret_cast<void*>(fault.address);
        const char* op = fault.write ? "write" : "read";
        if (g_memory.IsInGuestRange(host))
        {
            uint32_t guest = g_memory.MapVirtual(host);
            Log("[crash] %s of guest address 0x%08X%s", op, guest,
                guest >= KERNEL_HEAP_BEGIN && guest < KERNEL_HEAP_END ? " (runtime heap guard: probably a guest stack overflow)" : "");
        }
        else
            Log("[crash] %s of host address %p (outside guest memory)", op, host);
        diag::ReportGuestState("crash");
        fflush(stdout);
#if defined(__ANDROID__)
        // Android's own report next (debuggerd: the host backtrace in logcat, tag DEBUG, and a
        // tombstone), which the driver crashes need: the fault goes on to its handler.
        Log("[crash] Android's crash report follows (logcat DEBUG)");
        return false;
#else
        diag::Terminate(3);
#endif
    }
}

namespace diag
{
    void DescribeHostAddress(uintptr_t address, char* buffer, size_t size)
    {
        Dl_info info{};
        Dl_info self{};
        bool known = dladdr(reinterpret_cast<void*>(address), &info) != 0;
        dladdr(reinterpret_cast<void*>(&DescribeHostAddress), &self);
        if (known && info.dli_fbase == self.dli_fbase)
        {
            char function[512];
            snprintf(buffer, size, "nearest guest function %s", DescribeGuestCode(FindGuestFunction(address), function, sizeof(function)));
            return;
        }
        const char* path = known && info.dli_fname ? info.dli_fname : "unknown module";
        const char* name = strrchr(path, '/');
        snprintf(buffer, size, "%s+0x%zX (outside the recompiled code)", name ? name + 1 : path,
                 address - reinterpret_cast<uintptr_t>(known ? info.dli_fbase : nullptr));
    }

    void InstallHostCrashHandlers()
    {
        plat::SetUnhandledFaultHandler(CrashHandler);
    }

    void Terminate(uint32_t code)
    {
        _exit(int(code));
    }

    void* OpenCurrentThread()
    {
        return nullptr;
    }

    void CloseThread(void*) {}

    bool SampleThread(void*, uintptr_t&)
    {
        return false;
    }
}
