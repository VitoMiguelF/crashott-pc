// XMsg* (xam): messages to system "apps" inside XAM. Only the Xbox Music Player (XMP, app 0xFA)
// needs answers: the title asks it whether the user is playing their own music from the dashboard
// and mutes its own soundtrack if so (an Xbox 360 certification rule). With the old stubs nothing
// was written back, XMPGetStatus left stack garbage in the state (sub_82247818 tests state == 0),
// and the menu music was decoded but never mixed. Here the music player is always idle and the
// title has playback control, as on a console with no background music. Message numbers and
// layouts follow Xenia's xmp_app.cc. XUserWriteAchievements (app 0xFB, 0x000B0008) unlocks achievements
// (achievements/achievements.h); other messages (e.g. XUserSetContext, app 0xFB) succeed without doing anything.

#include "kernel.h"
#include "../achievements/achievements.h"

#include <atomic>

namespace
{
    constexpr uint32_t X_ERROR_SUCCESS = 0;
    constexpr uint32_t X_ERROR_IO_PENDING = 0x3E5;

    constexpr uint32_t APP_XMP = 0xFA;
    constexpr uint32_t XMP_GET_STATUS = 0x00070009;            // { client, u32* state }
    constexpr uint32_t XMP_GET_VOLUME = 0x0007000B;            // { client, float* volume }
    constexpr uint32_t XMP_SET_PLAYBACK_CONTROLLER = 0x0007001A;
    constexpr uint32_t XMP_GET_PLAYBACK_CONTROLLER = 0x0007001B; // { client, u32* controller, u32* locked }
    constexpr uint32_t XMP_STATE_IDLE = 0;
    // XGI (app 0xFB): XUserWriteAchievements = { u32 count, XUSER_ACHIEVEMENT* } (achievements.h).
    constexpr uint32_t APP_XGI = 0xFB;
    constexpr uint32_t XGI_WRITE_ACHIEVEMENTS = 0x000B0008;

    std::atomic<uint32_t> g_loggedXmp;

    uint32_t DispatchMessage(uint8_t* base, uint32_t app, uint32_t message, uint32_t buffer)
    {
        if (app == APP_XGI && message == XGI_WRITE_ACHIEVEMENTS)
            achievements::WriteMessage(buffer);
        if (app != APP_XMP)
            return X_ERROR_SUCCESS;

        if (g_loggedXmp.fetch_add(1, std::memory_order_relaxed) < 16)
            Log("[xmsg] XMP message 0x%08X (buffer 0x%08X)", message, buffer);
        if (!buffer)
            return X_ERROR_SUCCESS;

        switch (message)
        {
        case XMP_GET_STATUS:
            if (uint32_t state = PPC_LOAD_U32(buffer + 4))
                PPC_STORE_U32(state, XMP_STATE_IDLE);
            break;
        case XMP_GET_VOLUME:
            if (uint32_t volume = PPC_LOAD_U32(buffer + 4))
                PPC_STORE_U32(volume, 0x3F800000); // 1.0f
            break;
        case XMP_GET_PLAYBACK_CONTROLLER:
            // Controller 0 = the title; not locked.
            if (uint32_t controller = PPC_LOAD_U32(buffer + 4))
                PPC_STORE_U32(controller, 0);
            if (uint32_t locked = PPC_LOAD_U32(buffer + 8))
                PPC_STORE_U32(locked, 0);
            break;
        default: // playlist control, SetPlaybackController, SetVolume...: nothing plays, so nothing to do
            break;
        }
        return X_ERROR_SUCCESS;
    }

    // Asynchronous requests finish immediately; with an XOVERLAPPED the call reports "pending" and
    // the overlapped holds the result, as Xenia's XMsgStartIORequest does.
    uint32_t StartRequest(uint8_t* base, uint32_t app, uint32_t message, uint32_t overlapped, uint32_t buffer)
    {
        uint32_t result = DispatchMessage(base, app, message, buffer);
        if (!overlapped)
            return result;
        CompleteXOverlapped(base, overlapped, result);
        return X_ERROR_IO_PENDING;
    }
}

// u32 XMsgInProcessCall(u32 App, u32 Message, void* Param1, void* Param2)
PPC_FUNC(__imp__XMsgInProcessCall)
{
    KERNEL_IMPORT_ENTER(XMsgInProcessCall);
    uint32_t app = ctx.r3.u32, message = ctx.r4.u32, buffer = ctx.r5.u32;
    KERNEL_LOG("[xmsg] XMsgInProcessCall(app 0x%X, message 0x%08X, 0x%08X, 0x%08X) lr=0x%08X", app, message, buffer,
               ctx.r6.u32, GuestLR(ctx));
    ctx.r3.u64 = DispatchMessage(base, app, message, buffer);
}

// u32 XMsgStartIORequest(u32 App, u32 Message, XOVERLAPPED* Overlapped, void* Buffer, u32 BufferLength)
PPC_FUNC(__imp__XMsgStartIORequest)
{
    KERNEL_IMPORT_ENTER(XMsgStartIORequest);
    KERNEL_LOG("[xmsg] XMsgStartIORequest(app 0x%X, message 0x%08X, overlapped 0x%08X, 0x%08X, %u) lr=0x%08X",
               ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, GuestLR(ctx));
    ctx.r3.u64 = StartRequest(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

// u32 XMsgStartIORequestEx(u32 App, u32 Message, XOVERLAPPED* Overlapped, void* Buffer, u32 BufferLength,
//                          void* Extra)
PPC_FUNC(__imp__XMsgStartIORequestEx)
{
    KERNEL_IMPORT_ENTER(XMsgStartIORequestEx);
    KERNEL_LOG("[xmsg] XMsgStartIORequestEx(app 0x%X, message 0x%08X, overlapped 0x%08X, 0x%08X, %u, 0x%08X) lr=0x%08X",
               ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, GuestLR(ctx));
    ctx.r3.u64 = StartRequest(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}
