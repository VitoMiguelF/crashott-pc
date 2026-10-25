#include "cpu/ppc_registers.h"
#include "diagnostics.h"
#include "paths.h"
#include "symbols.h"
#include "kernel/imports.h"
#include "kernel/memory.h"
#include "platform/platform.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>


namespace
{
    std::mutex g_logMutex;

    // Every thread running guest code, for the watchdog; the faulting thread's own context is
    // reported on a crash.
    struct GuestThreadInfo
    {
        PPCContext* ctx;
        HANDLE handle;
        DWORD id;
    };
    std::mutex g_threadsMutex;
    std::vector<GuestThreadInfo> g_guestThreads;
    thread_local PPCContext* t_diagnosticContext;
    std::vector<void (*)()> g_reporters;
    std::vector<std::pair<uintptr_t, uint32_t>> g_hostToGuest;

    std::atomic<void (*)(const char*)> g_logObserver{ nullptr };
    FILE* g_logFile = nullptr;  // SetLogFile, under g_logMutex

    void VLog(const char* prefix, const char* format, va_list args)
    {
        void (*observer)(const char*) = g_logObserver.load();
        char line[1024];
        if (observer)
        {
            va_list copy;
            va_copy(copy, args);
            size_t length = size_t(snprintf(line, sizeof(line), "%s", prefix));
            vsnprintf(line + length, sizeof(line) - length, format, copy);
            va_end(copy);
        }
        {
            std::lock_guard lock(g_logMutex);
            if (g_logFile)
            {
                va_list copy;
                va_copy(copy, args);
                fputs(prefix, g_logFile);
                vfprintf(g_logFile, format, copy);
                fputc('\n', g_logFile);
                fflush(g_logFile);
                va_end(copy);
            }
            fputs(prefix, stdout);
            vfprintf(stdout, format, args);
            fputc('\n', stdout);
            fflush(stdout);
        }
        if (observer)
            observer(line);
    }

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

    void LogGuestRegisters(const PPCContext* ctx)
    {
        Log("guest lr=0x%08X r1=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X ctr=0x%08X r13=0x%08X",
            GuestLR(*ctx), ctx->r1.u32, ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, GuestCTR(*ctx), ctx->r13.u32);

        // Guest backtrace: each frame's back chain is at 0(r1), and the caller's saved LR at
        // back chain - 8 (the prologues store it there before stwu). Stacks live in the runtime heap.
        char trace[512];
        int length = 0;
        uint32_t sp = ctx->r1.u32;
        for (int depth = 0; depth < 16 && sp >= KERNEL_HEAP_BEGIN && sp < KERNEL_HEAP_END; depth++)
        {
            uint32_t next = LoadBE32(sp);
            if (next <= sp || next >= KERNEL_HEAP_END)
                break;
            length += snprintf(trace + length, sizeof(trace) - length, " %08X@%08X", LoadBE32(next - 8), next);
            if (length >= int(sizeof(trace)) - 12)
                break;
            sp = next;
        }
        if (length)
            Log("guest backtrace (return address@caller frame):%s", trace);

        // Same frames with the function each return address is in (names: symbols.h).
        char where[512];
        Log("guest lr in %s", DescribeGuestCode(GuestLR(*ctx), where, sizeof(where)));
        sp = ctx->r1.u32;
        for (int depth = 0; depth < 16 && sp >= KERNEL_HEAP_BEGIN && sp < KERNEL_HEAP_END; depth++)
        {
            uint32_t next = LoadBE32(sp);
            if (next <= sp || next >= KERNEL_HEAP_END)
                break;
            Log("  #%-2d %s", depth, DescribeGuestCode(LoadBE32(next - 8), where, sizeof(where)));
            sp = next;
        }
    }

    // CRASH_DUMP=<guest address>,<bytes>: hex dump printed with the report (debugging aid).
    void DumpRequestedMemory()
    {
        const char* spec = getenv("CRASH_DUMP");
        while (spec && *spec)
        {
        uint32_t address = uint32_t(strtoul(spec, nullptr, 16));
        const char* comma = strchr(spec, ',');
        const char* next = strchr(spec, ';');
        uint32_t size = comma && (!next || comma < next) ? uint32_t(strtoul(comma + 1, nullptr, 0)) : 64;
        spec = next ? next + 1 : nullptr;
        for (uint32_t offset = 0; offset < size; offset += 16)
        {
            char line[128];
            int length = snprintf(line, sizeof(line), "[dump] %08X:", address + offset);
            for (uint32_t i = 0; i < 16; i += 4)
                length += snprintf(line + length, sizeof(line) - length, " %08X", LoadBE32(address + offset + i));
            Log("%s", line);
        }
        }
    }

    void ReportGuestState(const char* reason)
    {
        Log("========== %s ==========", reason);
        DumpRequestedMemory();
        if (auto* ctx = t_diagnosticContext)
            LogGuestRegisters(ctx);
        Log("last import called: %s", GetLastImportCalled());
        PrintImportSummary();
        for (auto* reporter : g_reporters)
            reporter();
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
                DescribeHostAddress(info->ContextRecord->Rip, location, sizeof(location));
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
        DescribeHostAddress(rip, location, sizeof(location));

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

        ReportGuestState("crash");
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 3);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    DWORD WINAPI WatchdogThread(LPVOID param)
    {
        DWORD seconds = DWORD(reinterpret_cast<uintptr_t>(param));
        plat::SleepMs(seconds * 1000);
        Log("[watchdog] still running after %lu s, stopping", seconds);
        {
            std::unique_lock lock(g_threadsMutex, std::try_to_lock);
            for (auto& thread : g_guestThreads)
            {
                if (SuspendThread(thread.handle) == DWORD(-1))
                    continue;

                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(thread.handle, &context))
                {
                    char location[MAX_PATH + 64];
                    DescribeHostAddress(context.Rip, location, sizeof(location));
                    Log("[watchdog] guest thread %lu (r13=0x%08X) at host rip %p, %s", thread.id, thread.ctx->r13.u32,
                        reinterpret_cast<void*>(context.Rip), location);
                    LogGuestRegisters(thread.ctx);
                }
            }
        }
        ReportGuestState("timeout");
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 2);
        return 0;
    }
}

void SetLogObserver(void (*observer)(const char* line))
{
    g_logObserver = observer;
}

void SetLogFile(const wchar_t* path)
{
    std::lock_guard lock(g_logMutex);
    if (g_logFile)
        fclose(g_logFile);
    g_logFile = _wfopen(path, L"w");
}

void Log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    VLog("", format, args);
    va_end(args);
}

void Fatal(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    VLog("[fatal] ", format, args);
    va_end(args);
    ReportGuestState("fatal");
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 1);
    __builtin_unreachable();
}

void InstallCrashHandler()
{
    for (size_t i = 0; PPCFuncMappings[i].host != nullptr; i++)
        g_hostToGuest.emplace_back(reinterpret_cast<uintptr_t>(PPCFuncMappings[i].host), uint32_t(PPCFuncMappings[i].guest));

    std::sort(g_hostToGuest.begin(), g_hostToGuest.end());

    // Function names for the reports (tools/rtti_scan.py output; optional).
    char symbols[MAX_PATH]{};
    if (!GetEnvironmentVariableA("CRASH_SYMBOLS", symbols, sizeof(symbols)))
        snprintf(symbols, sizeof(symbols), "%s", (paths::Data() / "symbols" / "functions.csv").string().c_str());
    if (strcmp(symbols, "0") != 0)
    {
        if (size_t count = LoadGuestSymbols(symbols))
            Log("[symbols] %zu function names from %s", count, symbols);
        else
            Log("[symbols] %s not found: run `python tools/rtti_scan.py` for class::method names in reports", symbols);
    }

    // Registered last so the demand-commit handler (first in the list) runs before it.
    AddVectoredExceptionHandler(0, CrashHandler);
    SetUnhandledExceptionFilter(UnhandledCrashFilter);
}

void StartWatchdog()
{
    char value[32]{};
    if (!GetEnvironmentVariableA("CRASH_TIMEOUT", value, sizeof(value)))
        return;

    uintptr_t seconds = strtoul(value, nullptr, 10);
    if (seconds == 0)
        return;

    CreateThread(nullptr, 0, WatchdogThread, reinterpret_cast<LPVOID>(seconds), 0, nullptr);
}

void AddDiagnosticReporter(void (*reporter)())
{
    g_reporters.push_back(reporter);
}

void SetDiagnosticContext(PPCContext* ctx)
{
    t_diagnosticContext = ctx;

    HANDLE handle{};
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle, 0, FALSE, DUPLICATE_SAME_ACCESS);

    std::lock_guard lock(g_threadsMutex);
    g_guestThreads.push_back({ ctx, handle, GetCurrentThreadId() });
}

void ClearDiagnosticContext()
{
    t_diagnosticContext = nullptr;

    std::lock_guard lock(g_threadsMutex);
    DWORD id = GetCurrentThreadId();
    for (auto it = g_guestThreads.begin(); it != g_guestThreads.end(); ++it)
    {
        if (it->id == id)
        {
            CloseHandle(it->handle);
            g_guestThreads.erase(it);
            break;
        }
    }
}

void UpdateDiagnosticContext(PPCContext* ctx)
{
    t_diagnosticContext = ctx;

    std::lock_guard lock(g_threadsMutex);
    DWORD id = GetCurrentThreadId();
    for (auto& info : g_guestThreads)
    {
        if (info.id == id)
            info.ctx = ctx;
    }
}

uint32_t FindGuestFunction(uintptr_t hostAddress)
{
    // Nearest mapped function at or below the address. Recompiled functions are emitted in
    // guest-address order within each translation unit, so this is the containing function
    // unless the fault is in CRT/runtime code.
    auto it = std::upper_bound(g_hostToGuest.begin(), g_hostToGuest.end(), std::make_pair(hostAddress, UINT32_MAX));
    if (it == g_hostToGuest.begin())
        return 0;

    return std::prev(it)->second;
}

bool IsImportLogForced(const char* name)
{
    const char* list = getenv("CRASH_LOG_IMPORTS");
    if (!list)
        return false;
    size_t length = strlen(name);
    for (const char* p = list; (p = strstr(p, name)) != nullptr; p += length)
    {
        bool startOk = p == list || p[-1] == ',';
        bool endOk = p[length] == '\0' || p[length] == ',';
        if (startOk && endOk)
            return true;
    }
    return false;
}
