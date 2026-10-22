// xboxkrnl threads: creation, resume and termination, handles to them, kernel stacks, delays,
// thread-local storage and the process type.
//
// TLS slot values live in the guest thread block, in the array the PCR points to (PCR+0x0, see
// cpu/guest_thread.cpp), so title code that reads slots straight through r13 sees the same values
// as KeTlsGetValue. Slot indices are process-wide, from a 64-entry bitmap (GuestThreadContext's
// 0x100-byte TLS array).

#include "kernel.h"
#include "memory.h"
#include "objects.h"
#include "threads.h"
#include "../cpu/guest_thread.h"

#include <windows.h>

#include <bit>
#include <mutex>
#include <thread>

namespace
{
    constexpr uint32_t TLS_SLOT_COUNT = GuestThreadContext::TLS_SIZE / 4;
    constexpr uint32_t X_TLS_OUT_OF_INDEXES = 0xFFFFFFFF;

    std::mutex g_tlsMutex;
    uint64_t g_tlsUsed;

    uint32_t TlsSlotAddress(PPCContext& ctx, uint8_t* base, uint32_t index)
    {
        return PPC_LOAD_U32(ctx.r13.u32 + 0x0) + index * 4;
    }
}

// u32 KeGetCurrentProcessType(): 1 = title process.
PPC_FUNC(__imp__KeGetCurrentProcessType)
{
    KERNEL_IMPORT_ENTER(KeGetCurrentProcessType);
    KERNEL_LOG("[kernel] KeGetCurrentProcessType() lr=0x%08X -> 1", GuestLR(ctx));
    ctx.r3.u64 = 1;
}

// u32 KeTlsAlloc(): slot index, or X_TLS_OUT_OF_INDEXES.
PPC_FUNC(__imp__KeTlsAlloc)
{
    KERNEL_IMPORT_ENTER(KeTlsAlloc);

    uint32_t index = X_TLS_OUT_OF_INDEXES;
    {
        std::lock_guard lock(g_tlsMutex);
        if (~g_tlsUsed != 0)
        {
            index = uint32_t(std::countr_one(g_tlsUsed));
            g_tlsUsed |= 1ull << index;
        }
    }

    // A new slot starts as 0 in every thread (threads created later get a zeroed block).
    if (index != X_TLS_OUT_OF_INDEXES)
    {
        ForEachGuestThread([&](ThreadObject& thread)
        {
            PPC_STORE_U32(thread.context->tls + index * 4, 0);
        });
    }

    KERNEL_LOG("[kernel] KeTlsAlloc() lr=0x%08X -> %d", GuestLR(ctx), int32_t(index));
    ctx.r3.u64 = index;
}

// BOOL KeTlsFree(u32 index)
PPC_FUNC(__imp__KeTlsFree)
{
    KERNEL_IMPORT_ENTER(KeTlsFree);

    uint32_t index = ctx.r3.u32;
    bool ok = false;
    if (index < TLS_SLOT_COUNT)
    {
        std::lock_guard lock(g_tlsMutex);
        ok = (g_tlsUsed >> index) & 1;
        g_tlsUsed &= ~(1ull << index);
    }

    KERNEL_LOG("[kernel] KeTlsFree(%u) lr=0x%08X -> %d", index, GuestLR(ctx), ok);
    ctx.r3.u64 = ok;
}

// void* KeTlsGetValue(u32 index)
PPC_FUNC(__imp__KeTlsGetValue)
{
    KERNEL_IMPORT_ENTER(KeTlsGetValue);

    uint32_t index = ctx.r3.u32;
    uint32_t value = index < TLS_SLOT_COUNT ? PPC_LOAD_U32(TlsSlotAddress(ctx, base, index)) : 0;

    KERNEL_LOG("[kernel] KeTlsGetValue(%u) lr=0x%08X -> 0x%08X", index, GuestLR(ctx), value);
    ctx.r3.u64 = value;
}

// BOOL KeTlsSetValue(u32 index, void* value)
PPC_FUNC(__imp__KeTlsSetValue)
{
    KERNEL_IMPORT_ENTER(KeTlsSetValue);

    uint32_t index = ctx.r3.u32;
    uint32_t value = ctx.r4.u32;
    bool ok = index < TLS_SLOT_COUNT;
    if (ok)
        PPC_STORE_U32(TlsSlotAddress(ctx, base, index), value);

    KERNEL_LOG("[kernel] KeTlsSetValue(%u, 0x%08X) lr=0x%08X -> %d", index, value, GuestLR(ctx), ok);
    ctx.r3.u64 = ok;
}

// NTSTATUS ExCreateThread(HANDLE* Handle, u32 StackSize, u32* ThreadId, void* XapiThreadStartup,
//                         void* StartAddress, void* StartContext, u32 CreationFlags)
// With XapiThreadStartup the thread runs XapiThreadStartup(StartAddress, StartContext), which
// calls StartAddress(StartContext) and then ExTerminateThread with its result (as in Xenia).
// CreationFlags: bit 0 = suspended, bits 24-31 = processor mask.
PPC_FUNC(__imp__ExCreateThread)
{
    KERNEL_IMPORT_ENTER(ExCreateThread);

    uint32_t handlePtr = ctx.r3.u32;
    uint32_t stackSize = ctx.r4.u32;
    uint32_t threadIdPtr = ctx.r5.u32;
    uint32_t xapiStartup = ctx.r6.u32;
    uint32_t start = ctx.r7.u32;
    uint32_t startContext = ctx.r8.u32;
    uint32_t flags = ctx.r9.u32;

    bool suspended = flags & 1;
    uint32_t processorMask = flags >> 24;
    uint32_t cpu = processorMask ? uint32_t(std::countr_zero(processorMask)) : 0;

    ThreadObject* thread = xapiStartup
        ? CreateGuestThread(xapiStartup, start, startContext, stackSize, true, cpu)
        : CreateGuestThread(start, startContext, 0, stackSize, true, cpu);

    uint32_t handle = CreateHandle(thread);
    if (handlePtr)
        PPC_STORE_U32(handlePtr, handle);
    if (threadIdPtr)
        PPC_STORE_U32(threadIdPtr, thread->threadId);

    Log("[kernel] ExCreateThread(stack=0x%X, startup=0x%08X, start=0x%08X, context=0x%08X, flags=0x%08X) lr=0x%08X -> handle 0x%08X, thread %u, kthread 0x%08X",
        stackSize, xapiStartup, start, startContext, flags, GuestLR(ctx), handle, thread->threadId, thread->guest);

    // Resumed only once the handle and id are written, so the new thread never sees them unset.
    if (!suspended)
        ResumeGuestThread(thread);

    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// void ExTerminateThread(u32 ExitStatus)
PPC_FUNC(__imp__ExTerminateThread)
{
    SetLastImportCalled("ExTerminateThread");
    ExitCurrentGuestThread(ctx.r3.u32);
}

// NTSTATUS NtResumeThread(HANDLE Thread, u32* SuspendCount)
PPC_FUNC(__imp__NtResumeThread)
{
    KERNEL_IMPORT_ENTER(NtResumeThread);

    uint32_t handle = ctx.r3.u32;
    uint32_t countPtr = ctx.r4.u32;
    KernelObject* object = ObjectFromHandle(handle);
    uint32_t status = X_STATUS_SUCCESS;
    uint32_t previous = 0;

    if (!object)
        status = X_STATUS_INVALID_HANDLE;
    else if (object->type != DISPATCHER_THREAD)
        status = X_STATUS_OBJECT_TYPE_MISMATCH;
    else
        previous = ResumeGuestThread(static_cast<ThreadObject*>(object));

    if (status == X_STATUS_SUCCESS && countPtr)
        PPC_STORE_U32(countPtr, previous);

    KERNEL_LOG("[kernel] NtResumeThread(0x%08X) lr=0x%08X -> 0x%08X (previous count %u)", handle, GuestLR(ctx), status, previous);
    ctx.r3.u64 = status;
}

// u32 KeResumeThread(KTHREAD* Thread): previous suspend count.
PPC_FUNC(__imp__KeResumeThread)
{
    KERNEL_IMPORT_ENTER(KeResumeThread);

    uint32_t kthread = ctx.r3.u32;
    KernelObject* object = ObjectFromGuest(kthread);
    uint32_t previous = 0;
    if (object && object->type == DISPATCHER_THREAD)
        previous = ResumeGuestThread(static_cast<ThreadObject*>(object));
    else
        Log("[kernel] KeResumeThread: 0x%08X is not a known thread", kthread);

    KERNEL_LOG("[kernel] KeResumeThread(0x%08X) lr=0x%08X -> %u", kthread, GuestLR(ctx), previous);
    ctx.r3.u64 = previous;
}

// NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, void* ObjectType, void** Object)
// Objects are never freed, so references are not counted.
PPC_FUNC(__imp__ObReferenceObjectByHandle)
{
    KERNEL_IMPORT_ENTER(ObReferenceObjectByHandle);

    uint32_t handle = ctx.r3.u32;
    uint32_t objectPtr = ctx.r5.u32;
    KernelObject* object = ObjectFromHandle(handle);
    uint32_t status = object ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;

    if (objectPtr)
        PPC_STORE_U32(objectPtr, object ? object->guest : 0);

    KERNEL_LOG("[kernel] ObReferenceObjectByHandle(0x%08X, type=0x%08X) lr=0x%08X -> 0x%08X (object 0x%08X)",
               handle, ctx.r4.u32, GuestLR(ctx), status, object ? object->guest : 0);
    ctx.r3.u64 = status;
}

// void ObDereferenceObject(void* Object)
PPC_FUNC(__imp__ObDereferenceObject)
{
    SetLastImportCalled("ObDereferenceObject");
}

// NTSTATUS NtClose(HANDLE Handle). Only kernel-object handles exist so far.
PPC_FUNC(__imp__NtClose)
{
    KERNEL_IMPORT_ENTER(NtClose);

    uint32_t handle = ctx.r3.u32;
    uint32_t status = CloseObjectHandle(handle) ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
    KERNEL_LOG("[kernel] NtClose(0x%08X) lr=0x%08X -> 0x%08X", handle, GuestLR(ctx), status);
    if (status != X_STATUS_SUCCESS && !logCall)
        Log("[kernel] NtClose(0x%08X): unknown handle", handle);
    ctx.r3.u64 = status;
}

// void* MmCreateKernelStack(u32 Size, u32 Flags): top of a new stack (XAPI uses it for fibers).
// Comes from the runtime heap, with an uncommitted guard below; never freed.
PPC_FUNC(__imp__MmCreateKernelStack)
{
    KERNEL_IMPORT_ENTER(MmCreateKernelStack);

    uint32_t size = (ctx.r3.u32 + 0xFFF) & ~0xFFFu;
    uint32_t top = GuestAlloc(size, 0x1000, GuestThreadContext::STACK_GUARD_SIZE) + size;
    KERNEL_LOG("[kernel] MmCreateKernelStack(0x%X, 0x%X) lr=0x%08X -> 0x%08X", ctx.r3.u32, ctx.r4.u32, GuestLR(ctx), top);
    ctx.r3.u64 = top;
}

// void MmDeleteKernelStack(void* StackBase, void* StackLimit)
PPC_FUNC(__imp__MmDeleteKernelStack)
{
    KERNEL_IMPORT_ENTER(MmDeleteKernelStack);
    KERNEL_LOG("[kernel] MmDeleteKernelStack(0x%08X, 0x%08X) lr=0x%08X (not freed)", ctx.r3.u32, ctx.r4.u32, GuestLR(ctx));
}

// NTSTATUS KeDelayExecutionThread(u32 WaitMode, BOOL Alertable, s64* Interval)
// Interval in 100 ns units, negative = relative. A zero interval yields the processor.
PPC_FUNC(__imp__KeDelayExecutionThread)
{
    KERNEL_IMPORT_ENTER(KeDelayExecutionThread);

    uint32_t intervalPtr = ctx.r5.u32;
    int64_t interval = intervalPtr ? int64_t(PPC_LOAD_U64(intervalPtr)) : 0;
    WaitDeadline deadline = DeadlineFromTimeout(intervalPtr);

    if (!deadline || *deadline <= std::chrono::steady_clock::now())
        SwitchToThread();
    else
        std::this_thread::sleep_until(*deadline);

    KERNEL_LOG("[kernel] KeDelayExecutionThread(mode=%u, alertable=%u, interval=%lld) lr=0x%08X",
               ctx.r3.u32, ctx.r4.u32, (long long)interval, GuestLR(ctx));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// u64 KeQueryPerformanceFrequency(). The recompiled mftb reads the host TSC (__rdtsc), so the
// frequency is the TSC rate, measured once.
PPC_FUNC(__imp__KeQueryPerformanceFrequency)
{
    KERNEL_IMPORT_ENTER(KeQueryPerformanceFrequency);

    static const uint64_t frequency = []
    {
        LARGE_INTEGER qpcFrequency, qpcStart, qpcEnd;
        QueryPerformanceFrequency(&qpcFrequency);
        QueryPerformanceCounter(&qpcStart);
        uint64_t tscStart = __rdtsc();
        Sleep(50);
        QueryPerformanceCounter(&qpcEnd);
        uint64_t tscEnd = __rdtsc();
        double seconds = double(qpcEnd.QuadPart - qpcStart.QuadPart) / double(qpcFrequency.QuadPart);
        return uint64_t(double(tscEnd - tscStart) / seconds);
    }();

    KERNEL_LOG("[kernel] KeQueryPerformanceFrequency() lr=0x%08X -> %llu", GuestLR(ctx), (unsigned long long)frequency);
    ctx.r3.u64 = frequency;
}

// NTSTATUS KeSetAffinityThread(KTHREAD* Thread, u32 Affinity, u32* PreviousAffinity)
// Unlike NT, the previous affinity goes through the pointer and the result is a status (as in
// Xenia). The thread's processor (PCR+0x10C) becomes the lowest processor in the mask: titles
// index per-processor data with it (lbz r,268(r13)), so it must differ between pinned threads.
// The host thread itself is not pinned.
PPC_FUNC(__imp__KeSetAffinityThread)
{
    KERNEL_IMPORT_ENTER(KeSetAffinityThread);

    uint32_t kthread = ctx.r3.u32;
    uint32_t affinity = ctx.r4.u32;
    uint32_t previousPtr = ctx.r5.u32;

    KernelObject* object = ObjectFromGuest(kthread);
    if (!affinity || !object || object->type != DISPATCHER_THREAD)
    {
        Log("[kernel] KeSetAffinityThread(0x%08X, 0x%X) lr=0x%08X: invalid thread or affinity", kthread, affinity, GuestLR(ctx));
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }

    auto* thread = static_cast<ThreadObject*>(object);
    uint32_t processorAddress = thread->context->pcr + 0x10C;
    uint32_t previous = PPC_LOAD_U8(processorAddress);
    uint32_t processor = uint32_t(std::countr_zero(affinity));

    if (previousPtr)
        PPC_STORE_U32(previousPtr, 1u << previous);
    PPC_STORE_U8(processorAddress, uint8_t(processor));

    KERNEL_LOG("[kernel] KeSetAffinityThread(thread %u, 0x%X) lr=0x%08X: processor %u -> %u",
               thread->threadId, affinity, GuestLR(ctx), previous, processor);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}
