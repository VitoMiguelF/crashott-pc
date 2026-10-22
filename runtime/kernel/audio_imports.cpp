// XAudio render driver and XMA context imports (xboxkrnl).
//
// The title registers a render driver client (callback + argument) and expects the callback to be
// called once per audio frame (256 samples at 48 kHz, ~5.3 ms); the callback (or the title's
// renderer thread it wakes) mixes a frame and hands it over with XAudioSubmitRenderDriverFrame.
// Without these calls the title's audio renderer never advances, and anything synchronized to it
// (sound bank loading, movie playback) waits forever. A runtime guest thread calls every registered
// callback at that rate, paced by the host device (audio/audio_output.cpp); submitted frames are
// played and can be dumped. Semantics follow Xenia's xboxkrnl_audio.cc / audio_system.cc.
// The XMA decoder itself is in audio/xma.cpp.

#include "kernel.h"
#include "threads.h"
#include "memory.h"
#include "../audio/audio_output.h"
#include "../audio/xma.h"

#include <chrono>
#include <cstring>
#include <mutex>

namespace
{
    constexpr uint32_t MAX_CLIENTS = 8;
    constexpr uint32_t DRIVER_HANDLE_BASE = 0x41550000;
    constexpr auto STATS_PERIOD = std::chrono::seconds(10);

    struct Client
    {
        uint32_t callback = 0;
        uint32_t argument = 0;
    };

    std::mutex g_mutex;
    Client g_clients[MAX_CLIENTS];
    bool g_threadStarted;
    void ClockThread(PPCContext& ctx)
    {
        audio::StartOutput();
        auto lastStats = std::chrono::steady_clock::now();
        while (true)
        {
            audio::WaitForNextFrame();

            Client clients[MAX_CLIENTS];
            {
                std::lock_guard lock(g_mutex);
                memcpy(clients, g_clients, sizeof(clients));
            }
            for (const Client& client : clients)
            {
                if (client.callback)
                {
                    CallGuestFunction(ctx, client.callback, client.argument, 0);
                    audio::CountCallback();
                }
            }

            if (auto now = std::chrono::steady_clock::now(); now - lastStats >= STATS_PERIOD)
            {
                audio::LogStats();
                lastStats = now;
            }
        }
    }
}


// u32 XAudioRegisterRenderDriverClient(u32* CallbackAndArgument, u32* DriverHandle)
PPC_FUNC(__imp__XAudioRegisterRenderDriverClient)
{
    KERNEL_IMPORT_ENTER(XAudioRegisterRenderDriverClient);
    uint32_t callbackPtr = ctx.r3.u32, driverPtr = ctx.r4.u32;
    if (!callbackPtr || !driverPtr)
    {
        ctx.r3.u64 = 0x80070057; // E_INVALIDARG
        return;
    }
    uint32_t callback = PPC_LOAD_U32(callbackPtr), argument = PPC_LOAD_U32(callbackPtr + 4);

    std::lock_guard lock(g_mutex);
    uint32_t index = MAX_CLIENTS;
    for (uint32_t i = 0; i < MAX_CLIENTS; i++)
    {
        if (!g_clients[i].callback)
        {
            index = i;
            break;
        }
    }
    if (index == MAX_CLIENTS)
    {
        ctx.r3.u64 = 0x8007000E; // E_OUTOFMEMORY
        return;
    }
    g_clients[index] = { callback, argument };
    PPC_STORE_U32(driverPtr, DRIVER_HANDLE_BASE | index);
    if (!g_threadStarted)
    {
        g_threadStarted = true;
        CreateRuntimeGuestThread("audio clock", 4, ClockThread);
    }
    KERNEL_LOG("[audio] XAudioRegisterRenderDriverClient(callback 0x%08X, argument 0x%08X) lr=0x%08X -> driver 0x%08X",
               callback, argument, GuestLR(ctx), DRIVER_HANDLE_BASE | index);
    ctx.r3.u64 = 0;
}

// u32 XAudioUnregisterRenderDriverClient(u32 DriverHandle)
PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient)
{
    KERNEL_IMPORT_ENTER(XAudioUnregisterRenderDriverClient);
    uint32_t index = ctx.r3.u32 & 0xFFFF;
    if (index < MAX_CLIENTS)
    {
        std::lock_guard lock(g_mutex);
        g_clients[index] = {};
    }
    KERNEL_LOG("[audio] XAudioUnregisterRenderDriverClient(0x%08X) lr=0x%08X", ctx.r3.u32, GuestLR(ctx));
    ctx.r3.u64 = 0;
}

// u32 XAudioSubmitRenderDriverFrame(u32 DriverHandle, float* Samples): 256 samples x 6 channels,
// planar big-endian floats.
PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame)
{
    KERNEL_IMPORT_ENTER(XAudioSubmitRenderDriverFrame);
    if (ctx.r4.u32)
        audio::SubmitFrame(base + ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// u32 XAudioGetVoiceCategoryVolumeChangeMask(u32 DriverHandle, u32* Mask): no category volume
// changes (the console's dashboard settings never change here).
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolumeChangeMask)
{
    KERNEL_IMPORT_ENTER(XAudioGetVoiceCategoryVolumeChangeMask);
    if (ctx.r4.u32)
        PPC_STORE_U32(ctx.r4.u32, 0);
    ctx.r3.u64 = 0;
}

// u32 XAudioGetVoiceCategoryVolume(u32 Category, float* Volume): full volume for every category.
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume)
{
    KERNEL_IMPORT_ENTER(XAudioGetVoiceCategoryVolume);
    if (ctx.r4.u32)
        PPC_STORE_U32(ctx.r4.u32, 0x3F800000); // 1.0f
    ctx.r3.u64 = 0;
}

// NTSTATUS XMACreateContext(void** Context): one of the decoder's 320 contexts (audio/xma.cpp).
PPC_FUNC(__imp__XMACreateContext)
{
    KERNEL_IMPORT_ENTER(XMACreateContext);
    uint32_t context = xma::AllocateContext();
    if (ctx.r3.u32)
        PPC_STORE_U32(ctx.r3.u32, context);
    KERNEL_LOG("[audio] XMACreateContext(0x%08X) lr=0x%08X -> context 0x%08X", ctx.r3.u32, GuestLR(ctx), context);
    ctx.r3.u64 = context ? X_STATUS_SUCCESS : X_STATUS_NO_MEMORY;
}

// void XMAReleaseContext(void* Context)
PPC_FUNC(__imp__XMAReleaseContext)
{
    KERNEL_IMPORT_ENTER(XMAReleaseContext);
    xma::ReleaseContext(ctx.r3.u32);
    KERNEL_LOG("[audio] XMAReleaseContext(0x%08X) lr=0x%08X", ctx.r3.u32, GuestLR(ctx));
    ctx.r3.u64 = 0;
}
