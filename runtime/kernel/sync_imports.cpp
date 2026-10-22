// xboxkrnl critical sections, on the guest RTL_CRITICAL_SECTION (UnleashedRecomp's kernel/imports.cpp
// is the reference):
//
//   0x00  DISPATCHER_HEADER Header  (Type u8, Absolute u8, ProcessType u8, Inserted u8,
//                                    SignalState s32, WaitListHead LIST_ENTRY)
//   0x10  s32 LockCount             (-1 when free; incremented by every Enter)
//   0x14  s32 RecursionCount
//   0x18  u32 OwningThread          (KTHREAD of the owner, 0 when free)
//
// The owner is the guest KTHREAD (PCR+0x100), as on the console, rather than r13 as in
// UnleashedRecomp, so title code that compares OwningThread to the current thread sees the same
// value. Contended waits use std::atomic wait/notify on OwningThread instead of the header.

#include "kernel.h"
#include "memory.h"

#include <atomic>

namespace
{
    constexpr uint32_t CS_LOCK_COUNT = 0x10;
    constexpr uint32_t CS_RECURSION_COUNT = 0x14;
    constexpr uint32_t CS_OWNING_THREAD = 0x18;

    std::atomic_ref<uint32_t> GuestAtomic(uint8_t* base, uint32_t guest)
    {
        return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(base + guest));
    }

    // Atomically adds `delta` to a big-endian guest s32.
    void AtomicAddBE(uint8_t* base, uint32_t guest, int32_t delta)
    {
        auto value = GuestAtomic(base, guest);
        uint32_t expected = value.load();
        while (!value.compare_exchange_weak(expected, __builtin_bswap32(__builtin_bswap32(expected) + delta)))
        {
        }
    }

    uint32_t CurrentThread(PPCContext& ctx, uint8_t* base)
    {
        return PPC_LOAD_U32(ctx.r13.u32 + 0x100);
    }
}

// void RtlInitializeCriticalSection(RTL_CRITICAL_SECTION* cs)
PPC_FUNC(__imp__RtlInitializeCriticalSection)
{
    KERNEL_IMPORT_ENTER(RtlInitializeCriticalSection);

    uint32_t cs = ctx.r3.u32;
    PPC_STORE_U8(cs + 0x00, 1);          // Type: synchronization event
    PPC_STORE_U8(cs + 0x01, 0);          // Absolute (spin count / 256)
    PPC_STORE_U8(cs + 0x02, 0);
    PPC_STORE_U8(cs + 0x03, 0);
    PPC_STORE_U32(cs + 0x04, 0);         // SignalState
    PPC_STORE_U32(cs + 0x08, cs + 0x08); // WaitListHead: empty list points to itself
    PPC_STORE_U32(cs + 0x0C, cs + 0x08);
    PPC_STORE_U32(cs + CS_LOCK_COUNT, 0xFFFFFFFF);
    PPC_STORE_U32(cs + CS_RECURSION_COUNT, 0);
    PPC_STORE_U32(cs + CS_OWNING_THREAD, 0);

    KERNEL_LOG("[kernel] RtlInitializeCriticalSection(0x%08X) lr=0x%08X", cs, GuestLR(ctx));
}

// void RtlEnterCriticalSection(RTL_CRITICAL_SECTION* cs)
PPC_FUNC(__imp__RtlEnterCriticalSection)
{
    SetLastImportCalled("RtlEnterCriticalSection");

    uint32_t cs = ctx.r3.u32;
    uint32_t thread = CurrentThread(ctx, base);
    uint32_t threadBE = __builtin_bswap32(thread);

    AtomicAddBE(base, cs + CS_LOCK_COUNT, 1);

    auto owner = GuestAtomic(base, cs + CS_OWNING_THREAD);
    if (owner.load() == threadBE)
    {
        PPC_STORE_U32(cs + CS_RECURSION_COUNT, PPC_LOAD_U32(cs + CS_RECURSION_COUNT) + 1);
        return;
    }

    while (true)
    {
        uint32_t expected = 0;
        if (owner.compare_exchange_weak(expected, threadBE))
            break;
        if (expected != 0)
            owner.wait(expected);
    }

    PPC_STORE_U32(cs + CS_RECURSION_COUNT, 1);
}

// void RtlLeaveCriticalSection(RTL_CRITICAL_SECTION* cs)
PPC_FUNC(__imp__RtlLeaveCriticalSection)
{
    SetLastImportCalled("RtlLeaveCriticalSection");

    uint32_t cs = ctx.r3.u32;
    uint32_t recursion = PPC_LOAD_U32(cs + CS_RECURSION_COUNT) - 1;
    PPC_STORE_U32(cs + CS_RECURSION_COUNT, recursion);

    if (recursion != 0)
    {
        AtomicAddBE(base, cs + CS_LOCK_COUNT, -1);
        return;
    }

    auto owner = GuestAtomic(base, cs + CS_OWNING_THREAD);
    owner.store(0);
    AtomicAddBE(base, cs + CS_LOCK_COUNT, -1);
    owner.notify_one();
}
