// xboxkrnl dispatcher objects and waits: events, semaphores, mutants, the Nt*/Ke* wait functions
// and spin locks. Object model, guest layouts and the shared dispatcher lock are in objects.h.
//
// Guest layouts after the 16-byte DISPATCHER_HEADER:
//   KSEMAPHORE  0x10 s32 Limit
//   KMUTANT     0x10 LIST_ENTRY MutantListEntry, 0x18 KTHREAD* OwnerThread, 0x1C u8 Abandoned
// Waits are not alertable (no APCs yet): they only end with an object or a timeout.

#include "kernel.h"
#include "memory.h"
#include "objects.h"
#include "../platform/platform.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint32_t X_STATUS_OBJECT_NAME_EXISTS = 0x40000000;
    constexpr uint32_t WAIT_TYPE_ALL = 0;
    constexpr uint32_t X_MAXIMUM_WAIT_OBJECTS = 64;

    std::mutex g_namesMutex;
    std::unordered_map<std::string, KernelObject*> g_namedObjects;

    // OBJECT_ATTRIBUTES { HANDLE RootDirectory; ANSI_STRING* ObjectName; u32 Attributes; },
    // ANSI_STRING { u16 Length; u16 MaximumLength; char* Buffer; }.
    std::string ObjectName(uint32_t attributes)
    {
        if (!attributes)
            return {};

        uint32_t name = LoadBE32(attributes + 0x4);
        if (!name)
            return {};

        uint16_t length = __builtin_bswap16(*reinterpret_cast<uint16_t*>(g_memory.Translate(name)));
        uint32_t buffer = LoadBE32(name + 0x4);
        if (!buffer || !length)
            return {};

        return std::string(reinterpret_cast<const char*>(g_memory.Translate(buffer)), length);
    }

    // Registers `object` (or reuses the object already created under the same name) and writes a
    // new handle to `handlePtr`. Returns the NTSTATUS for the Nt*Create call.
    uint32_t CreateObjectHandle(KernelObject* object, uint32_t handlePtr, uint32_t attributes, const char*& nameOut)
    {
        static thread_local std::string name;
        name = ObjectName(attributes);
        nameOut = name.c_str();

        uint32_t status = X_STATUS_SUCCESS;
        if (!name.empty())
        {
            std::lock_guard lock(g_namesMutex);
            auto [it, inserted] = g_namedObjects.try_emplace(name, object);
            if (!inserted)
            {
                delete object;
                object = it->second;
                status = X_STATUS_OBJECT_NAME_EXISTS;
            }
            else
                RegisterObject(object);
        }
        else
            RegisterObject(object);

        StoreBE32(handlePtr, CreateHandle(object));
        return status;
    }

    template<typename T>
    T* ObjectOfType(KernelObject* object, std::initializer_list<uint8_t> types)
    {
        if (!object)
            return nullptr;
        for (uint8_t type : types)
        {
            if (object->type == type)
                return static_cast<T*>(object);
        }
        return nullptr;
    }

    EventObject* EventFromHandle(uint32_t handle)
    {
        return ObjectOfType<EventObject>(ObjectFromHandle(handle), { DISPATCHER_NOTIFICATION_EVENT, DISPATCHER_SYNCHRONIZATION_EVENT });
    }

    EventObject* EventFromGuest(uint32_t guest)
    {
        return ObjectOfType<EventObject>(ObjectFromGuest(guest), { DISPATCHER_NOTIFICATION_EVENT, DISPATCHER_SYNCHRONIZATION_EVENT });
    }

    // Sets/clears an event; returns the previous state.
    int32_t SetEventState(EventObject* event, bool signaled)
    {
        int32_t previous;
        {
            std::lock_guard lock(DispatcherMutex());
            previous = event->signaled;
            event->Set(signaled);
        }
        if (signaled)
            NotifyDispatcher();
        return previous;
    }

    // Adds `count` to a semaphore; returns the previous count, or -1 if the limit would be exceeded.
    int32_t ReleaseSemaphore(SemaphoreObject* semaphore, int32_t count)
    {
        int32_t previous;
        {
            std::lock_guard lock(DispatcherMutex());
            previous = semaphore->count;
            if (count <= 0 || int64_t(previous) + count > semaphore->limit)
                return -1;
            semaphore->count += count;
            semaphore->SetGuestSignalState(semaphore->count);
        }
        NotifyDispatcher();
        return previous;
    }

    uint32_t WaitAndLog(bool logCall, const char* name, std::span<KernelObject* const> objects, bool waitAll, uint32_t timeoutPtr, uint32_t lr)
    {
        WaitDeadline deadline = DeadlineFromTimeout(timeoutPtr);
        long long timeout = timeoutPtr ? (long long)int64_t(__builtin_bswap64(*reinterpret_cast<uint64_t*>(g_memory.Translate(timeoutPtr)))) : 0;

        if (logCall)
        {
            Log("[kernel] %s(%zu object(s) from 0x%08X%s, timeout=%s%lld) lr=0x%08X thread 0x%08X: waiting",
                name, objects.size(), objects.empty() ? 0 : objects[0]->guest, waitAll ? ", all" : "",
                timeoutPtr ? "" : "infinite ", timeout, lr, CurrentKThread());
        }

        uint32_t status = WaitForObjects(objects, waitAll, deadline, CurrentKThread());

        if (logCall)
            Log("[kernel] %s lr=0x%08X thread 0x%08X -> 0x%08X", name, lr, CurrentKThread(), status);
        return status;
    }
}

// NTSTATUS NtCreateEvent(HANDLE* Handle, OBJECT_ATTRIBUTES* Attributes, u32 EventType, BOOL InitialState)
// EventType: 0 = notification (manual reset), 1 = synchronization (auto reset).
PPC_FUNC(__imp__NtCreateEvent)
{
    KERNEL_IMPORT_ENTER(NtCreateEvent);

    auto* event = new EventObject();
    event->type = ctx.r5.u32 ? DISPATCHER_SYNCHRONIZATION_EVENT : DISPATCHER_NOTIFICATION_EVENT;
    event->signaled = ctx.r6.u32 != 0;

    const char* name;
    uint32_t status = CreateObjectHandle(event, ctx.r3.u32, ctx.r4.u32, name);
    if (status == X_STATUS_SUCCESS)
        event->SetGuestSignalState(event->signaled);

    KERNEL_LOG("[kernel] NtCreateEvent(type=%u, initial=%u, name=\"%s\") lr=0x%08X -> 0x%08X, handle 0x%08X",
               ctx.r5.u32, ctx.r6.u32, name, GuestLR(ctx), status, PPC_LOAD_U32(ctx.r3.u32));
    ctx.r3.u64 = status;
}

// NTSTATUS NtSetEvent(HANDLE Event, s32* PreviousState)
PPC_FUNC(__imp__NtSetEvent)
{
    KERNEL_IMPORT_ENTER(NtSetEvent);

    uint32_t handle = ctx.r3.u32;
    uint32_t previousPtr = ctx.r4.u32;
    uint32_t status = X_STATUS_SUCCESS;

    if (EventObject* event = EventFromHandle(handle))
    {
        int32_t previous = SetEventState(event, true);
        if (previousPtr)
            PPC_STORE_U32(previousPtr, uint32_t(previous));
    }
    else
        status = X_STATUS_INVALID_HANDLE;

    KERNEL_LOG("[kernel] NtSetEvent(0x%08X) lr=0x%08X -> 0x%08X", handle, GuestLR(ctx), status);
    ctx.r3.u64 = status;
}

// s32 KeSetEvent(KEVENT* Event, s32 Increment, BOOL Wait): previous state.
PPC_FUNC(__imp__KeSetEvent)
{
    KERNEL_IMPORT_ENTER(KeSetEvent);

    uint32_t guest = ctx.r3.u32;
    int32_t previous = 0;
    if (EventObject* event = EventFromGuest(guest))
        previous = SetEventState(event, true);
    else
        Log("[kernel] KeSetEvent: 0x%08X is not an event", guest);

    KERNEL_LOG("[kernel] KeSetEvent(0x%08X) lr=0x%08X -> %d", guest, GuestLR(ctx), previous);
    ctx.r3.u64 = uint32_t(previous);
}

// s32 KeResetEvent(KEVENT* Event): previous state.
PPC_FUNC(__imp__KeResetEvent)
{
    KERNEL_IMPORT_ENTER(KeResetEvent);

    uint32_t guest = ctx.r3.u32;
    int32_t previous = 0;
    if (EventObject* event = EventFromGuest(guest))
        previous = SetEventState(event, false);
    else
        Log("[kernel] KeResetEvent: 0x%08X is not an event", guest);

    KERNEL_LOG("[kernel] KeResetEvent(0x%08X) lr=0x%08X -> %d", guest, GuestLR(ctx), previous);
    ctx.r3.u64 = uint32_t(previous);
}

// NTSTATUS NtCreateSemaphore(HANDLE* Handle, OBJECT_ATTRIBUTES* Attributes, s32 InitialCount, s32 MaximumCount)
PPC_FUNC(__imp__NtCreateSemaphore)
{
    KERNEL_IMPORT_ENTER(NtCreateSemaphore);

    auto* semaphore = new SemaphoreObject();
    semaphore->type = DISPATCHER_SEMAPHORE;
    semaphore->count = ctx.r5.s32;
    semaphore->limit = ctx.r6.s32;

    const char* name;
    uint32_t status = CreateObjectHandle(semaphore, ctx.r3.u32, ctx.r4.u32, name);
    if (status == X_STATUS_SUCCESS)
    {
        semaphore->SetGuestSignalState(semaphore->count);
        StoreBE32(semaphore->guest + 0x10, uint32_t(semaphore->limit));
    }

    KERNEL_LOG("[kernel] NtCreateSemaphore(initial=%d, max=%d, name=\"%s\") lr=0x%08X -> 0x%08X, handle 0x%08X",
               ctx.r5.s32, ctx.r6.s32, name, GuestLR(ctx), status, PPC_LOAD_U32(ctx.r3.u32));
    ctx.r3.u64 = status;
}

// NTSTATUS NtReleaseSemaphore(HANDLE Semaphore, s32 ReleaseCount, s32* PreviousCount)
PPC_FUNC(__imp__NtReleaseSemaphore)
{
    KERNEL_IMPORT_ENTER(NtReleaseSemaphore);

    uint32_t handle = ctx.r3.u32;
    int32_t count = ctx.r4.s32;
    uint32_t previousPtr = ctx.r5.u32;
    uint32_t status = X_STATUS_SUCCESS;
    int32_t previous = 0;

    if (auto* semaphore = ObjectOfType<SemaphoreObject>(ObjectFromHandle(handle), { DISPATCHER_SEMAPHORE }))
    {
        previous = ReleaseSemaphore(semaphore, count);
        if (previous < 0)
            status = X_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
        else if (previousPtr)
            PPC_STORE_U32(previousPtr, uint32_t(previous));
    }
    else
        status = X_STATUS_INVALID_HANDLE;

    KERNEL_LOG("[kernel] NtReleaseSemaphore(0x%08X, %d) lr=0x%08X -> 0x%08X (previous %d)", handle, count, GuestLR(ctx), status, previous);
    ctx.r3.u64 = status;
}

// void KeInitializeSemaphore(KSEMAPHORE* Semaphore, s32 Count, s32 Limit)
PPC_FUNC(__imp__KeInitializeSemaphore)
{
    KERNEL_IMPORT_ENTER(KeInitializeSemaphore);

    uint32_t guest = ctx.r3.u32;
    PPC_STORE_U8(guest + 0x00, DISPATCHER_SEMAPHORE);
    PPC_STORE_U8(guest + 0x01, 0);
    PPC_STORE_U8(guest + 0x02, 5); // Size in dwords
    PPC_STORE_U8(guest + 0x03, 0);
    PPC_STORE_U32(guest + 0x04, ctx.r4.u32);
    PPC_STORE_U32(guest + 0x08, guest + 0x08); // empty wait list, so a fresh host object is created
    PPC_STORE_U32(guest + 0x0C, guest + 0x08);
    PPC_STORE_U32(guest + 0x10, ctx.r5.u32);
    ObjectFromGuest(guest);

    KERNEL_LOG("[kernel] KeInitializeSemaphore(0x%08X, count=%d, limit=%d) lr=0x%08X", guest, ctx.r4.s32, ctx.r5.s32, GuestLR(ctx));
}

// s32 KeReleaseSemaphore(KSEMAPHORE* Semaphore, s32 Increment, s32 Adjustment, BOOL Wait): previous count.
PPC_FUNC(__imp__KeReleaseSemaphore)
{
    KERNEL_IMPORT_ENTER(KeReleaseSemaphore);

    uint32_t guest = ctx.r3.u32;
    int32_t adjustment = ctx.r5.s32;
    int32_t previous = 0;

    if (auto* semaphore = ObjectOfType<SemaphoreObject>(ObjectFromGuest(guest), { DISPATCHER_SEMAPHORE }))
    {
        previous = ReleaseSemaphore(semaphore, adjustment);
        if (previous < 0)
        {
            // The console raises X_STATUS_SEMAPHORE_LIMIT_EXCEEDED here.
            Log("[kernel] KeReleaseSemaphore(0x%08X, %d): limit %d exceeded", guest, adjustment, semaphore->limit);
            previous = semaphore->count;
        }
    }
    else
        Log("[kernel] KeReleaseSemaphore: 0x%08X is not a semaphore", guest);

    KERNEL_LOG("[kernel] KeReleaseSemaphore(0x%08X, %d) lr=0x%08X -> %d", guest, adjustment, GuestLR(ctx), previous);
    ctx.r3.u64 = uint32_t(previous);
}

// NTSTATUS NtCreateMutant(HANDLE* Handle, OBJECT_ATTRIBUTES* Attributes, BOOL InitialOwner)
PPC_FUNC(__imp__NtCreateMutant)
{
    KERNEL_IMPORT_ENTER(NtCreateMutant);

    auto* mutant = new MutantObject();
    mutant->type = DISPATCHER_MUTANT;

    const char* name;
    uint32_t status = CreateObjectHandle(mutant, ctx.r3.u32, ctx.r4.u32, name);
    if (status == X_STATUS_SUCCESS)
    {
        std::lock_guard lock(DispatcherMutex());
        if (ctx.r5.u32)
            mutant->Acquire(CurrentKThread());
        else
            mutant->SetGuestSignalState(1);
    }

    KERNEL_LOG("[kernel] NtCreateMutant(owner=%u, name=\"%s\") lr=0x%08X -> 0x%08X, handle 0x%08X",
               ctx.r5.u32, name, GuestLR(ctx), status, PPC_LOAD_U32(ctx.r3.u32));
    ctx.r3.u64 = status;
}

// NTSTATUS NtReleaseMutant(HANDLE Mutant, s32* PreviousCount)
PPC_FUNC(__imp__NtReleaseMutant)
{
    KERNEL_IMPORT_ENTER(NtReleaseMutant);

    uint32_t handle = ctx.r3.u32;
    uint32_t previousPtr = ctx.r4.u32;
    uint32_t status = X_STATUS_SUCCESS;
    bool released = false;

    if (auto* mutant = ObjectOfType<MutantObject>(ObjectFromHandle(handle), { DISPATCHER_MUTANT }))
    {
        std::lock_guard lock(DispatcherMutex());
        if (mutant->owner != CurrentKThread() || mutant->recursion == 0)
        {
            status = X_STATUS_MUTANT_NOT_OWNED;
        }
        else
        {
            if (previousPtr)
                PPC_STORE_U32(previousPtr, uint32_t(1 - mutant->recursion));
            if (--mutant->recursion == 0)
            {
                mutant->owner = 0;
                StoreBE32(mutant->guest + 0x18, 0);
                released = true;
            }
            mutant->SetGuestSignalState(1 - mutant->recursion);
        }
    }
    else
        status = X_STATUS_INVALID_HANDLE;

    if (released)
        NotifyDispatcher();

    KERNEL_LOG("[kernel] NtReleaseMutant(0x%08X) lr=0x%08X -> 0x%08X", handle, GuestLR(ctx), status);
    ctx.r3.u64 = status;
}

// NTSTATUS NtWaitForSingleObjectEx(HANDLE Handle, u32 WaitMode, BOOL Alertable, s64* Timeout)
PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    KERNEL_IMPORT_ENTER(NtWaitForSingleObjectEx);

    uint32_t handle = ctx.r3.u32;
    KernelObject* object = ObjectFromHandle(handle);
    if (!object)
    {
        Log("[kernel] NtWaitForSingleObjectEx(0x%08X) lr=0x%08X: invalid handle", handle, GuestLR(ctx));
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }

    KernelObject* objects[] = { object };
    ctx.r3.u64 = WaitAndLog(logCall, "NtWaitForSingleObjectEx", objects, false, ctx.r6.u32, GuestLR(ctx));
}

// NTSTATUS NtWaitForMultipleObjectsEx(u32 Count, HANDLE* Handles, u32 WaitType, u32 WaitMode,
//                                     BOOL Alertable, s64* Timeout)
PPC_FUNC(__imp__NtWaitForMultipleObjectsEx)
{
    KERNEL_IMPORT_ENTER(NtWaitForMultipleObjectsEx);

    uint32_t count = ctx.r3.u32;
    uint32_t handles = ctx.r4.u32;
    if (count == 0 || count > X_MAXIMUM_WAIT_OBJECTS)
    {
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }

    std::vector<KernelObject*> objects(count);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t handle = PPC_LOAD_U32(handles + i * 4);
        objects[i] = ObjectFromHandle(handle);
        if (!objects[i])
        {
            Log("[kernel] NtWaitForMultipleObjectsEx: invalid handle 0x%08X (lr=0x%08X)", handle, GuestLR(ctx));
            ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
            return;
        }
    }

    ctx.r3.u64 = WaitAndLog(logCall, "NtWaitForMultipleObjectsEx", objects, ctx.r5.u32 == WAIT_TYPE_ALL, ctx.r8.u32, GuestLR(ctx));
}

// NTSTATUS KeWaitForSingleObject(void* Object, u32 WaitReason, u32 WaitMode, BOOL Alertable, s64* Timeout)
PPC_FUNC(__imp__KeWaitForSingleObject)
{
    KERNEL_IMPORT_ENTER(KeWaitForSingleObject);

    uint32_t guest = ctx.r3.u32;
    KernelObject* object = ObjectFromGuest(guest);
    if (!object)
    {
        Log("[kernel] KeWaitForSingleObject(0x%08X) lr=0x%08X: unsupported object", guest, GuestLR(ctx));
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }

    KernelObject* objects[] = { object };
    ctx.r3.u64 = WaitAndLog(logCall, "KeWaitForSingleObject", objects, false, ctx.r7.u32, GuestLR(ctx));
}

// NTSTATUS KeWaitForMultipleObjects(u32 Count, void** Objects, u32 WaitType, u32 WaitReason,
//                                   u32 WaitMode, BOOL Alertable, s64* Timeout, KWAIT_BLOCK* WaitBlocks)
PPC_FUNC(__imp__KeWaitForMultipleObjects)
{
    KERNEL_IMPORT_ENTER(KeWaitForMultipleObjects);

    uint32_t count = ctx.r3.u32;
    uint32_t pointers = ctx.r4.u32;
    if (count == 0 || count > X_MAXIMUM_WAIT_OBJECTS)
    {
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }

    std::vector<KernelObject*> objects(count);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t guest = PPC_LOAD_U32(pointers + i * 4);
        objects[i] = ObjectFromGuest(guest);
        if (!objects[i])
        {
            Log("[kernel] KeWaitForMultipleObjects: unsupported object 0x%08X (lr=0x%08X)", guest, GuestLR(ctx));
            ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
            return;
        }
    }

    ctx.r3.u64 = WaitAndLog(logCall, "KeWaitForMultipleObjects", objects, ctx.r5.u32 == WAIT_TYPE_ALL, ctx.r9.u32, GuestLR(ctx));
}

// Spin locks: a guest u32, 0 when free, holding the owner's PCR (r13) as in Xenia. IRQL is not
// modeled, so the "old IRQL" results are 0.
namespace
{
    void AcquireSpinLock(uint8_t* base, uint32_t lock, uint32_t owner)
    {
        std::atomic_ref<uint32_t> value(*reinterpret_cast<uint32_t*>(base + lock));
        uint32_t ownerBE = __builtin_bswap32(owner);
        while (true)
        {
            uint32_t expected = 0;
            if (value.compare_exchange_weak(expected, ownerBE, std::memory_order_acquire))
                return;
            while (value.load(std::memory_order_relaxed) != 0)
                plat::CpuRelax();
        }
    }

    void ReleaseSpinLock(uint8_t* base, uint32_t lock)
    {
        std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(base + lock)).store(0, std::memory_order_release);
    }
}

// u8 KfAcquireSpinLock(u32* SpinLock): old IRQL.
PPC_FUNC(__imp__KfAcquireSpinLock)
{
    SetLastImportCalled("KfAcquireSpinLock");
    AcquireSpinLock(base, ctx.r3.u32, ctx.r13.u32);
    ctx.r3.u64 = 0;
}

// void KfReleaseSpinLock(u32* SpinLock, u8 OldIrql)
PPC_FUNC(__imp__KfReleaseSpinLock)
{
    SetLastImportCalled("KfReleaseSpinLock");
    ReleaseSpinLock(base, ctx.r3.u32);
}

// void KeAcquireSpinLockAtRaisedIrql(u32* SpinLock)
PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql)
{
    SetLastImportCalled("KeAcquireSpinLockAtRaisedIrql");
    AcquireSpinLock(base, ctx.r3.u32, ctx.r13.u32);
}

// void KeReleaseSpinLockFromRaisedIrql(u32* SpinLock)
PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql)
{
    SetLastImportCalled("KeReleaseSpinLockFromRaisedIrql");
    ReleaseSpinLock(base, ctx.r3.u32);
}

// void RtlRaiseException(EXCEPTION_RECORD* Record)
// There is no guest exception dispatch yet: the call returns as if a handler had continued
// execution. That is what the debugger-naming exception (0x406D1388, raised inside
// __try/__except(EXCEPTION_CONTINUE_EXECUTION)) expects; anything else is logged as a warning.
PPC_FUNC(__imp__RtlRaiseException)
{
    KERNEL_IMPORT_ENTER(RtlRaiseException);

    uint32_t record = ctx.r3.u32;
    uint32_t code = PPC_LOAD_U32(record + 0x0);
    constexpr uint32_t SET_THREAD_NAME = 0x406D1388;

    if (code == SET_THREAD_NAME)
    {
        // EXCEPTION_RECORD: +0x14 ExceptionInformation[]: { 0x1000, name, thread id, flags }.
        uint32_t name = PPC_LOAD_U32(record + 0x18);
        uint32_t threadId = PPC_LOAD_U32(record + 0x1C);
        KERNEL_LOG("[kernel] thread 0x%08X named \"%s\"", threadId, name ? reinterpret_cast<const char*>(base + name) : "");
    }
    else
    {
        Log("[kernel] RtlRaiseException(code 0x%08X, flags 0x%X, address 0x%08X) lr=0x%08X: not dispatched, execution continues",
            code, PPC_LOAD_U32(record + 0x4), PPC_LOAD_U32(record + 0xC), GuestLR(ctx));
    }
}
