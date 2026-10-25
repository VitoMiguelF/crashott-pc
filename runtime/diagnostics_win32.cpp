// The Windows side of diagnostics.cpp (diagnostics_host.h): vectored exception handler +
// unhandled-exception filter for crash reports, module names for host addresses, and
// SuspendThread / GetThreadContext for the watchdog.
#include "diagnostics.h"
#include "diagnostics_host.h"
#include "symbols.h"
#include "kernel/memory.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace
{
    const char* ExceptionName(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
        case EXCEPTION_BREAKPOINT: return "breakpoint";
        case STATUS_HEAP_CORRUPTION: return "heap corruption";
        case STATUS_STACK_BUFFER_OVERRUN: return "stack buffer overrun (/GS or fastfail)";
        default: return nullptr;
        }
    }

    LONG CALLBACK UnhandledCrashFilter(PEXCEPTION_POINTERS info);

    LONG CALLBACK CrashHandler(PEXCEPTION_POINTERS info)
    {
        auto* record = info->ExceptionRecord;
        const char* name = ExceptionName(record->ExceptionCode);
        if (!name)
            return EXCEPTION_CONTINUE_SEARCH;

        // Host code that is not the game's (the GPU driver, NGX/DLSS, FidelityFX) may fault on
        // purpose inside its own __try (e.g. probing reads of 0xFFFFFFFFFFFFFFFF). A vectored
        // handler sees those first, so only faults on guest memory are reported here; the rest
        // go on to the normal SEH search, and UnhandledCrashFilter reports them if nothing
        // handles them (recompiled code has no SEH frames, so its crashes still get there).
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 &&
            !g_memory.IsInGuestRange(reinterpret_cast<void*>(record->ExceptionInformation[1])))
        {
            static std::atomic<int> logged{ 0 };
            if (logged++ < 8)
            {
                char location[MAX_PATH + 64];
                diag::DescribeHostAddress(info->ContextRecord->Rip, location, sizeof(location));
                Log("[diag] first-chance access violation at %s (host address %p): left to the code's own exception handling",
                    location, reinterpret_cast<void*>(record->ExceptionInformation[1]));
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
        return UnhandledCrashFilter(info);
    }

    LONG CALLBACK UnhandledCrashFilter(PEXCEPTION_POINTERS info)
    {
        auto* record = info->ExceptionRecord;
        const char* name = ExceptionName(record->ExceptionCode);
        if (!name)
            return EXCEPTION_CONTINUE_SEARCH;

        // Faults the demand-commit handler (registered first) did not resolve end up here.
        uintptr_t rip = info->ContextRecord->Rip;
        char location[MAX_PATH + 64];
        diag::DescribeHostAddress(rip, location, sizeof(location));

        Log("[crash] %s (0x%08lX) at host rip %p, %s", name, record->ExceptionCode, reinterpret_cast<void*>(rip), location);

        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        {
            auto* host = reinterpret_cast<void*>(record->ExceptionInformation[1]);
            const char* op = record->ExceptionInformation[0] == 1 ? "write" : record->ExceptionInformation[0] == 8 ? "execute" : "read";
            if (g_memory.IsInGuestRange(host))
            {
                uint32_t guest = g_memory.MapVirtual(host);
                Log("[crash] %s of guest address 0x%08X%s", op, guest,
                    guest >= KERNEL_HEAP_BEGIN && guest < KERNEL_HEAP_END ? " (runtime heap guard: probably a guest stack overflow)" : "");
            }
            else
                Log("[crash] %s of host address %p (outside guest memory)", op, host);
        }

        diag::ReportGuestState("crash");
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 3);
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

namespace diag
{
    // "guest function Class::vfNN (sub_XXXXXXXX)" for addresses in the executable (name from
    // <data>/symbols/functions.csv, see symbols.h), "<module>+0xOFFSET" otherwise.
    void DescribeHostAddress(uintptr_t address, char* buffer, size_t size)
    {
        HMODULE module{};
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(address), &module);

        if (module == GetModuleHandleA(nullptr))
        {
            char function[512];
            snprintf(buffer, size, "nearest guest function %s", DescribeGuestCode(FindGuestFunction(address), function, sizeof(function)));
            return;
        }

        char path[MAX_PATH] = "unknown module";
        if (module)
            GetModuleFileNameA(module, path, sizeof(path));

        const char* name = strrchr(path, '\\');
        snprintf(buffer, size, "%s+0x%zX (outside the recompiled code)", name ? name + 1 : path, address - reinterpret_cast<uintptr_t>(module));
    }

    void InstallHostCrashHandlers()
    {
        AddVectoredExceptionHandler(0, CrashHandler);
        SetUnhandledExceptionFilter(UnhandledCrashFilter);
    }

    void Terminate(uint32_t code)
    {
        TerminateProcess(GetCurrentProcess(), code);
        __builtin_unreachable();
    }

    void* OpenCurrentThread()
    {
        HANDLE handle{};
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
        return handle;
    }

    void CloseThread(void* thread)
    {
        if (thread)
            CloseHandle(thread);
    }

    bool SampleThread(void* thread, uintptr_t& rip)
    {
        if (SuspendThread(thread) == DWORD(-1))
            return false;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(thread, &context))
            return false;
        rip = context.Rip;
        return true;
    }
}
