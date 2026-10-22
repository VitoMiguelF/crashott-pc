// XAM user profile imports. User 0 is a local profile with no settings stored: profile settings
// read back as "not set" (from = 0), so titles fall back to their defaults. Asynchronous calls
// complete their XOVERLAPPED immediately, as Xenia's CompleteOverlappedImmediate does
// (xam_user.cc / kernel_state.cc).

#include "kernel.h"
#include "objects.h"

#include <mutex>

// XOVERLAPPED: result +0, length +4, context +8, event +0xC, completion routine +0x10,
// completion context +0x14, extended error +0x18.
void CompleteXOverlapped(uint8_t* base, uint32_t overlapped, uint32_t result)
{
    CompleteXOverlappedEx(base, overlapped, result, result ? 0xFFFFFFFF : 0);
}

void CompleteXOverlappedEx(uint8_t* base, uint32_t overlapped, uint32_t result, uint32_t length)
{
    PPC_STORE_U32(overlapped + 0x00, result);
    PPC_STORE_U32(overlapped + 0x04, length);
    PPC_STORE_U32(overlapped + 0x18, result);
    uint32_t event = PPC_LOAD_U32(overlapped + 0x0C);
    if (event)
    {
        KernelObject* object = ObjectFromHandle(event);
        if (object && (object->type == DISPATCHER_NOTIFICATION_EVENT || object->type == DISPATCHER_SYNCHRONIZATION_EVENT))
        {
            {
                std::lock_guard lock(DispatcherMutex());
                static_cast<EventObject*>(object)->Set(true);
            }
            NotifyDispatcher();
        }
    }
    if (PPC_LOAD_U32(overlapped + 0x10))
        Log("[xam] XOVERLAPPED completion routines are not supported");
}

namespace
{
    constexpr uint32_t X_ERROR_SUCCESS = 0;
    constexpr uint32_t X_ERROR_INSUFFICIENT_BUFFER = 0x7A;
    constexpr uint32_t X_ERROR_INVALID_PARAMETER = 0x57;
    constexpr uint32_t X_ERROR_IO_PENDING = 0x3E5;
    constexpr uint32_t X_ERROR_NO_SUCH_USER = 0x525;

    uint32_t Finish(uint8_t* base, uint32_t overlapped, uint32_t result)
    {
        if (!overlapped)
            return result;
        CompleteXOverlapped(base, overlapped, result);
        return X_ERROR_IO_PENDING;
    }
}

// u32 XamUserReadProfileSettings(u32 TitleId, u32 UserIndex, u32 XuidCount, u64* Xuids,
//                                u32 SettingCount, u32* SettingIds, u32* BufferSize, void* Buffer,
//                                XOVERLAPPED* Overlapped (stack))
PPC_FUNC(__imp__XamUserReadProfileSettings)
{
    KERNEL_IMPORT_ENTER(XamUserReadProfileSettings);
    uint32_t userIndex = ctx.r4.u32, xuidCount = ctx.r5.u32;
    uint32_t settingCount = ctx.r7.u32, settingIds = ctx.r8.u32, bufferSizePtr = ctx.r9.u32, buffer = ctx.r10.u32;
    uint32_t overlapped = GuestStackArgument(ctx, base, 8);
    uint32_t result;

    uint32_t bufferSize = bufferSizePtr ? PPC_LOAD_U32(bufferSizePtr) : 0;
    uint32_t count = xuidCount ? xuidCount : 1;
    uint32_t needed = 8 + 40 * settingCount * count;
    if (settingCount < 1 || settingCount > 32 || !bufferSizePtr || (bufferSize && !buffer))
    {
        result = X_ERROR_INVALID_PARAMETER;
    }
    else if (!buffer || bufferSize < needed)
    {
        if (!bufferSize)
            PPC_STORE_U32(bufferSizePtr, needed);
        result = X_ERROR_INSUFFICIENT_BUFFER;
    }
    else if (!xuidCount && userIndex)
    {
        result = Finish(base, overlapped, X_ERROR_NO_SUCH_USER);
    }
    else
    {
        // Header (count, pointer to the settings), then one 40-byte setting per id: from = 0
        // (not set), user index or XUID, setting id, empty data.
        uint32_t settings = buffer + 8;
        PPC_STORE_U32(buffer + 0, settingCount * count);
        PPC_STORE_U32(buffer + 4, settings);
        for (uint32_t x = 0; x < count; x++)
        {
            for (uint32_t i = 0; i < settingCount; i++)
            {
                uint32_t setting = settings + 40 * (x * settingCount + i);
                memset(base + setting, 0, 40);
                if (xuidCount)
                    PPC_STORE_U64(setting + 0x08, PPC_LOAD_U64(ctx.r6.u32 + x * 8));
                else
                    PPC_STORE_U32(setting + 0x08, userIndex);
                PPC_STORE_U32(setting + 0x10, PPC_LOAD_U32(settingIds + i * 4));
            }
        }
        result = Finish(base, overlapped, X_ERROR_SUCCESS);
    }
    KERNEL_LOG("[xam] XamUserReadProfileSettings(title 0x%08X, user %u, %u xuid(s), %u setting(s), buffer 0x%08X size %u, overlapped 0x%08X) lr=0x%08X -> 0x%X",
               ctx.r3.u32, userIndex, xuidCount, settingCount, buffer, bufferSize, overlapped, GuestLR(ctx), result);
    ctx.r3.u64 = result;
}
