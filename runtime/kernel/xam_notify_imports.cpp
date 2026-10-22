// XAM notification listeners: XamNotifyCreateListener / XNotifyGetNext.
//
// A listener is a kernel object (a notification event, signaled while notifications are queued)
// so titles can wait on its handle or poll it. As on the console (and in Xenia's
// kernel_state.cc), listeners that subscribe to system notifications receive the startup
// sequence: XN_SYS_UI on and off, then XN_SYS_SIGNINCHANGED twice. Xenia sends it to the first
// such listener only; this title creates two (a storage watcher that only asks for id 0xB, then
// the game's own), so every system listener gets it. Without it the title stays on a black screen
// before loading anything.

#include "kernel.h"
#include "objects.h"

#include <algorithm>
#include <deque>
#include <vector>
#include <mutex>

namespace
{
    constexpr uint32_t XN_SYS_UI = 0x00000009;
    constexpr uint32_t XN_SYS_SIGNINCHANGED = 0x0000000A;
    constexpr uint32_t XN_SYS_STORAGEDEVICESCHANGED = 0x0000000B;
    constexpr uint64_t NOTIFY_SYSTEM = 0x00000001;

    struct ListenerObject : EventObject
    {
        uint64_t mask{};
        std::deque<std::pair<uint32_t, uint32_t>> queue; // id, param (guarded by the dispatcher lock)
    };

    std::vector<ListenerObject*> g_listeners; // guarded by the dispatcher lock

    void Enqueue(ListenerObject* listener, uint32_t id, uint32_t param)
    {
        {
            std::lock_guard lock(DispatcherMutex());
            listener->queue.push_back({ id, param });
            listener->signaled = true;
            listener->SetGuestSignalState(1);
        }
        NotifyDispatcher();
    }
}

// HANDLE XamNotifyCreateListener(u64 Mask, u32 MaxVersion)
PPC_FUNC(__imp__XamNotifyCreateListener)
{
    KERNEL_IMPORT_ENTER(XamNotifyCreateListener);
    auto* listener = new ListenerObject();
    listener->type = DISPATCHER_NOTIFICATION_EVENT;
    listener->mask = ctx.r3.u64;
    RegisterObject(listener);
    uint32_t handle = CreateHandle(listener);
    {
        std::lock_guard lock(DispatcherMutex());
        g_listeners.push_back(listener);
    }

    if (listener->mask & NOTIFY_SYSTEM)
    {
        Enqueue(listener, XN_SYS_UI, 1);
        Enqueue(listener, XN_SYS_UI, 0);
        Enqueue(listener, XN_SYS_SIGNINCHANGED, 1);
        Enqueue(listener, XN_SYS_SIGNINCHANGED, 1);
        Enqueue(listener, XN_SYS_STORAGEDEVICESCHANGED, 0);
    }
    KERNEL_LOG("[xam] XamNotifyCreateListener(mask 0x%llX, version %u) lr=0x%08X -> 0x%08X", (unsigned long long)ctx.r3.u64,
               ctx.r4.u32, GuestLR(ctx), handle);
    ctx.r3.u64 = handle;
}

// Notification ids carry their area in bits 25-30; a listener gets the areas set in its mask
// (bit 0 = system), as in Xenia's XNotifyListener::EnqueueNotification.
void BroadcastXNotification(uint32_t id, uint32_t param)
{
    uint64_t areaBit = 1ull << ((id >> 25) & 0x3F);
    std::vector<ListenerObject*> targets;
    {
        std::lock_guard lock(DispatcherMutex());
        for (ListenerObject* listener : g_listeners)
        {
            if (listener->mask & areaBit)
                targets.push_back(listener);
        }
    }
    for (ListenerObject* listener : targets)
        Enqueue(listener, id, param);
    Log("[xam] notification 0x%X (param 0x%X) to %zu listener(s)", id, param, targets.size());
}

// BOOL XNotifyGetNext(HANDLE Listener, u32 MatchId, u32* Id, u32* Param): dequeues the oldest
// notification (with MatchId != 0, the oldest one with that id).
PPC_FUNC(__imp__XNotifyGetNext)
{
    KERNEL_IMPORT_ENTER(XNotifyGetNext);
    uint32_t handle = ctx.r3.u32, matchId = ctx.r4.u32, idPtr = ctx.r5.u32, paramPtr = ctx.r6.u32;
    auto* listener = dynamic_cast<ListenerObject*>(handle ? ObjectFromHandle(handle) : nullptr);
    bool found = false;
    uint32_t id = 0, param = 0;
    if (listener)
    {
        std::lock_guard lock(DispatcherMutex());
        for (auto it = listener->queue.begin(); it != listener->queue.end(); ++it)
        {
            if (!matchId || it->first == matchId)
            {
                id = it->first;
                param = it->second;
                listener->queue.erase(it);
                found = true;
                break;
            }
        }
        if (listener->queue.empty())
        {
            listener->signaled = false;
            listener->SetGuestSignalState(0);
        }
    }
    if (idPtr)
        PPC_STORE_U32(idPtr, id);
    if (paramPtr)
        PPC_STORE_U32(paramPtr, param);
    {
        // First call from each (listener, id, caller), to see who polls what.
        static std::mutex seenMutex;
        static std::vector<uint64_t> seen;
        uint64_t key = (uint64_t(handle) << 40) ^ (uint64_t(matchId) << 32) ^ GuestLR(ctx);
        std::lock_guard lock(seenMutex);
        if (std::find(seen.begin(), seen.end(), key) == seen.end())
        {
            seen.push_back(key);
            Log("[xam] XNotifyGetNext(0x%08X, match 0x%X) first call from lr=0x%08X", handle, matchId, GuestLR(ctx));
        }
    }
    if (found)
        Log("[xam] XNotifyGetNext(0x%08X, match 0x%X) lr=0x%08X -> id 0x%X param 0x%X", handle, matchId, GuestLR(ctx), id, param);
    ctx.r3.u64 = found ? 1 : 0;
}

// u32 XamUserGetSigninState(u32 UserIndex): user 0 is signed in locally (1), as with Xenia's
// default profile; the others are not signed in (0).
PPC_FUNC(__imp__XamUserGetSigninState)
{
    KERNEL_IMPORT_ENTER(XamUserGetSigninState);
    uint32_t state = ctx.r3.u32 == 0 ? 1 : 0;
    KERNEL_LOG("[xam] XamUserGetSigninState(%u) lr=0x%08X -> %u", ctx.r3.u32, GuestLR(ctx), state);
    ctx.r3.u64 = state;
}
