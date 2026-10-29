#include "imports.h"
#include "../cpu/ppc_registers.h"
#include "memory.h"
#include "../diagnostics.h"
#include "../diagnostics_host.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    struct ImportStats
    {
        const char* name;
        uint32_t calls;
        uint32_t firstCallIndex;
    };

    // Every call is logged up to this many calls in total; after that only the first few calls
    // of each import, so hot polling loops don't flood the log.
    constexpr uint32_t LOG_ALL_CALLS_LIMIT = 1000;
    constexpr uint32_t LOG_CALLS_PER_IMPORT = 5;

    std::mutex g_importMutex;
    std::vector<ImportStats*> g_firstCallOrder;
    uint32_t g_totalCalls;
    std::atomic<const char*> g_lastImport{ "(none)" };

    // Guest C string at `guest` for logging, or nullptr if it doesn't look like one.
    const char* GuestString(uint32_t guest)
    {
        if (guest < 0x10000)
            return nullptr;

        auto* s = reinterpret_cast<const char*>(g_memory.Translate(guest));
        return memchr(s, '\0', 512) ? s : nullptr;
    }

    void StubImport(ImportStats& stats, PPCContext& ctx)
    {
        g_lastImport = stats.name;

        uint32_t index;
        bool logCall;
        {
            std::lock_guard lock(g_importMutex);
            index = ++g_totalCalls;
            if (stats.calls++ == 0)
            {
                stats.firstCallIndex = index;
                g_firstCallOrder.push_back(&stats);
            }
            logCall = index <= LOG_ALL_CALLS_LIMIT || stats.calls <= LOG_CALLS_PER_IMPORT;
        }

        if (logCall)
        {
            Log("[import #%u] %s(r3=0x%08X, r4=0x%08X, r5=0x%08X, r6=0x%08X) lr=0x%08X -> 0 (stub)",
                index, stats.name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, GuestLR(ctx));

            if (strcmp(stats.name, "DbgPrint") == 0)
            {
                if (const char* format = GuestString(ctx.r3.u32))
                    Log("    DbgPrint format: \"%s\"", format);
            }
        }
        if (strncmp(stats.name, "KeBugCheck", 10) == 0)
        {
            diag::LogGuestRegisters(&ctx);  // the game's fatal error: where it came from
            diag::LogHostBacktrace();
        }

        ctx.r3.u64 = 0;
    }
}

#define GUEST_IMPORT(name) \
    static ImportStats g_stats_##name{ #name }; \
    extern "C" PPC_FUNC(StubImport_##name) { StubImport(g_stats_##name, ctx); } \
    __attribute__((alias("StubImport_" #name))) PPC_WEAK_FUNC(__imp__##name);

#include "import_list.inc"

#undef GUEST_IMPORT

const char* GetLastImportCalled()
{
    return g_lastImport;
}

void SetLastImportCalled(const char* name)
{
    g_lastImport = name;
}

void PrintImportSummary()
{
    // try_lock: this also runs from the crash handler, possibly on a thread that holds the lock.
    std::unique_lock lock(g_importMutex, std::try_to_lock);
    Log("[imports] %u calls to %zu distinct imports, in order of first call:", g_totalCalls, g_firstCallOrder.size());
    for (auto* stats : g_firstCallOrder)
        Log("    #%-6u %-40s x%u", stats->firstCallIndex, stats->name, stats->calls);
}
