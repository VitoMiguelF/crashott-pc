#include "cpu/ppc_registers.h"
#include "diagnostics.h"
#include "paths.h"
#include "symbols.h"
#include "kernel/imports.h"
#include "kernel/memory.h"
#include "diagnostics_host.h"
#include "platform/platform.h"
#include "platform/text.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>  // stdout goes nowhere in an app: the log is logcat's (tag CrashRecomp)
#endif

namespace
{
    std::mutex g_logMutex;

    // Every thread running guest code, for the watchdog; the faulting thread's own context is
    // reported on a crash.
    struct GuestThreadInfo
    {
        PPCContext* ctx;
        void* handle;  // diag::OpenCurrentThread
        uint32_t id;
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
#if defined(__ANDROID__)
            va_list copy;
            va_copy(copy, args);
            char text[1024];
            size_t length = size_t(snprintf(text, sizeof(text), "%s", prefix));
            vsnprintf(text + length, sizeof(text) - length, format, copy);
            va_end(copy);
            __android_log_write(ANDROID_LOG_INFO, "CrashRecomp", text);
#endif
            fputs(prefix, stdout);
            vfprintf(stdout, format, args);
            fputc('\n', stdout);
            fflush(stdout);
        }
        if (observer)
            observer(line);
    }

}

namespace diag
{
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

}

namespace
{
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
}

namespace diag
{
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
}

namespace
{
    uint32_t WatchdogThread(void* param)
    {
        uint32_t seconds = uint32_t(reinterpret_cast<uintptr_t>(param));
        plat::SleepMs(seconds * 1000);
        Log("[watchdog] still running after %u s, stopping", seconds);
        {
            std::unique_lock lock(g_threadsMutex, std::try_to_lock);
            for (auto& thread : g_guestThreads)
            {
                uintptr_t rip;
                if (diag::SampleThread(thread.handle, rip))
                {
                    char location[1024];
                    diag::DescribeHostAddress(rip, location, sizeof(location));
                    Log("[watchdog] guest thread %u (r13=0x%08X) at host rip %p, %s", thread.id, thread.ctx->r13.u32,
                        reinterpret_cast<void*>(rip), location);
                }
                else
                    Log("[watchdog] guest thread %u (r13=0x%08X)", thread.id, thread.ctx->r13.u32);
                diag::LogGuestRegisters(thread.ctx);
            }
        }
        diag::ReportGuestState("timeout");
        fflush(stdout);
        diag::Terminate(2);
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
#ifdef _WIN32
    g_logFile = _wfopen(path, L"w");
#else
    g_logFile = fopen(plat::NarrowUtf8(path).c_str(), "w");
#endif
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
    diag::ReportGuestState("fatal");
    fflush(stdout);
    diag::Terminate(1);
}

void InstallCrashHandler()
{
    for (size_t i = 0; PPCFuncMappings[i].host != nullptr; i++)
        g_hostToGuest.emplace_back(reinterpret_cast<uintptr_t>(PPCFuncMappings[i].host), uint32_t(PPCFuncMappings[i].guest));

    std::sort(g_hostToGuest.begin(), g_hostToGuest.end());

    // Function names for the reports (tools/rtti_scan.py output; optional).
    const char* forced = getenv("CRASH_SYMBOLS");
    std::string symbols = forced ? forced : (paths::Data() / "symbols" / "functions.csv").string();
    if (symbols != "0")
    {
        if (size_t count = LoadGuestSymbols(symbols.c_str()))
            Log("[symbols] %zu function names from %s", count, symbols.c_str());
        else
            Log("[symbols] %s not found: run `python tools/rtti_scan.py` for class::method names in reports", symbols.c_str());
    }

    // Registered last so the demand-commit handler (first in the list) runs before it.
    diag::InstallHostCrashHandlers();
}

void StartWatchdog()
{
    const char* value = getenv("CRASH_TIMEOUT");
    if (!value)
        return;

    uintptr_t seconds = strtoul(value, nullptr, 10);
    if (seconds == 0)
        return;

    plat::CreateThread(WatchdogThread, reinterpret_cast<void*>(seconds), 0, false, nullptr);
}

void AddDiagnosticReporter(void (*reporter)())
{
    g_reporters.push_back(reporter);
}

void SetDiagnosticContext(PPCContext* ctx)
{
    t_diagnosticContext = ctx;

    void* handle = diag::OpenCurrentThread();

    std::lock_guard lock(g_threadsMutex);
    g_guestThreads.push_back({ ctx, handle, plat::CurrentThreadId() });
}

void ClearDiagnosticContext()
{
    t_diagnosticContext = nullptr;

    std::lock_guard lock(g_threadsMutex);
    uint32_t id = plat::CurrentThreadId();
    for (auto it = g_guestThreads.begin(); it != g_guestThreads.end(); ++it)
    {
        if (it->id == id)
        {
            diag::CloseThread(it->handle);
            g_guestThreads.erase(it);
            break;
        }
    }
}

void UpdateDiagnosticContext(PPCContext* ctx)
{
    t_diagnosticContext = ctx;

    std::lock_guard lock(g_threadsMutex);
    uint32_t id = plat::CurrentThreadId();
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
