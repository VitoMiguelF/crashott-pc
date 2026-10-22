#pragma once

#include "imports.h"
#include "../diagnostics.h"
#include "../cpu/ppc_registers.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <atomic>
#include <cstdint>

// Shared helpers for the real xboxkrnl/xam import implementations in runtime/kernel/. Each one is
// a strong `PPC_FUNC(__imp__Name)` that replaces the weak logging stub from imports.cpp.

constexpr uint32_t X_STATUS_SUCCESS = 0x00000000;
constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t X_STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t X_STATUS_CONFLICTING_ADDRESSES = 0xC0000018;
constexpr uint32_t X_STATUS_MEMORY_NOT_ALLOCATED = 0xC00000A0;
constexpr uint32_t X_STATUS_UNABLE_TO_FREE_VM = 0xC000001A;

// Argument `index` (0-based; 8 and up) of a guest call: arguments past r3-r10 live in the caller's
// parameter save area, one 8-byte slot each from r1+0x50, a 32-bit value in the low word.
inline uint32_t GuestStackArgument(PPCContext& ctx, uint8_t* base, uint32_t index)
{
    return PPC_LOAD_U32(ctx.r1.u32 + 0x54 + (index - 8) * 8);
}

// Completes an XOVERLAPPED right away with `result` (Win32 error code): result/length/extended
// error fields and its event, as Xenia's CompleteOverlappedImmediate (xam_user_imports.cpp).
void CompleteXOverlapped(uint8_t* base, uint32_t overlapped, uint32_t result);
// Same, with the length field (+4, e.g. XamEnumerate's item count) given explicitly.
void CompleteXOverlappedEx(uint8_t* base, uint32_t overlapped, uint32_t result, uint32_t length);

// Queues an XNotify notification (XN_SYS_UI, XN_SYS_STORAGEDEVICESCHANGED...) on every listener
// whose mask covers its area (xam_notify_imports.cpp).
void BroadcastXNotification(uint32_t id, uint32_t param);

// Calls logged per implemented import; later calls are only counted.
constexpr uint32_t KERNEL_LOG_CALLS_PER_IMPORT = 16;

// Marks `name` as the last import called and returns whether this call should be logged.
// CRASH_LOG_IMPORTS=Name1,Name2 logs every call of those imports (debugging aid).
bool IsImportLogForced(const char* name);

#define KERNEL_IMPORT_ENTER(name) \
    static std::atomic<uint32_t> s_calls_##name; \
    static const bool s_forceLog_##name = IsImportLogForced(#name); \
    SetLastImportCalled(#name); \
    const bool logCall = s_forceLog_##name || s_calls_##name.fetch_add(1, std::memory_order_relaxed) < KERNEL_LOG_CALLS_PER_IMPORT

#define KERNEL_LOG(...) do { if (logCall) Log(__VA_ARGS__); } while (0)
