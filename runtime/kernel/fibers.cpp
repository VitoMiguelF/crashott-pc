#include "kernel.h"
#include "memory.h"
#include "../diagnostics.h"

#include "../platform/platform.h"

#include <memory>
#include <mutex>
#include <unordered_map>

// Guest fibers (XAPI CreateFiber/ConvertThreadToFiber/SwitchToFiber/DeleteFiber, statically linked
// into the title). The title's I/O "threads" (sub_822889E8/sub_82288BB0 thread objects) are fibers
// on the main thread: the loader (sub_822D8D60) switches to them with sub_82288CE0, which calls
// SwitchToFiber. XAPI's SwitchToFiber (0x823E9C10) is hand-written assembly that stores the
// non-volatile registers in the current fiber and loads the target's, ending in a tail call to
// KeSetCurrentStackPointers. XenonRecomp can't express that (the target fiber resumes in the
// middle of some other function), so `sub_823E8D70` (the `SwitchToFiber` entry, `b 0x823E9C10`)
// came out as an empty function and every switch was a no-op: the loading fibers never ran,
// nothing past the .rcf directories was read and the boot state machine never left state 2.
//
// Here every guest fiber is a host fiber with its own PPCContext. The recompiled frames of a
// suspended fiber stay on its host stack and keep referring to its own context, so switching is a
// host SwitchToFiber plus the guest-visible bookkeeping (KTHREAD fiber pointer and stack fields).
// The guest fiber structure (0xA50 bytes, from CreateFiber = sub_823E8C30) is still allocated and
// filled by the original code:
//   +0x00 parameter (GetFiberData), +0x04 stack allocation base, +0x08 stack base, +0x0C stack
//   limit, +0x1C saved LR, +0x30 saved r1 (64-bit), +0x120 saved r31 (64-bit; for a new fiber the
//   start routine, run by the stub at 0x823E9C00 through sub_823E8D80).

PPC_EXTERN_FUNC(sub_823E8D80);
extern "C" PPC_FUNC(__imp__sub_823E8C30);
extern "C" PPC_FUNC(__imp__sub_823E8BE0);
extern "C" PPC_FUNC(__imp__sub_823E8D10);

namespace
{
    constexpr uint32_t KTHREAD_STACK_BASE = 0x5C;
    constexpr uint32_t KTHREAD_STACK_LIMIT = 0x60;
    constexpr uint32_t KTHREAD_STACK_ALLOC_BASE = 0xD0;
    constexpr uint32_t KTHREAD_FIBER = 0x164;

    constexpr uint32_t FIBER_PARAMETER = 0x00;
    constexpr uint32_t FIBER_STACK_ALLOC_BASE = 0x04;
    constexpr uint32_t FIBER_STACK_BASE = 0x08;
    constexpr uint32_t FIBER_STACK_LIMIT = 0x0C;
    constexpr uint32_t FIBER_LR = 0x1C;
    constexpr uint32_t FIBER_R1 = 0x30;
    constexpr uint32_t FIBER_R31 = 0x120;

    // LR of a fiber that has never run: `mr r3,r31; bl sub_823E8D80` (start routine, then bugcheck).
    constexpr uint32_t FIBER_START_STUB = 0x823E9C00;

    constexpr size_t HOST_FIBER_STACK_RESERVE = 64 * 1024 * 1024; // as for guest threads

    struct Fiber
    {
        uint32_t guest{};
        void* host{};
        PPCContext* ctx{};                   // the context this fiber's recompiled frames use
        std::unique_ptr<PPCContext> ownedCtx; // for fibers made by CreateFiber
        bool running{};
        uint64_t resumeR13{};                // PCR of the thread that switched to this fiber
    };

    // Guest fiber structure -> host fiber. Fibers made by CreateFiber are deleted with DeleteFiber;
    // the ones for converted threads live as long as the thread (t_threadFiber).
    std::mutex g_fibersMutex;
    std::unordered_map<uint32_t, Fiber*> g_fibers;

    // Fiber the calling host thread is running, and the one it was converted to, if it ever
    // switched fibers.
    thread_local Fiber* t_currentFiber;

    void Unmap(Fiber* fiber)
    {
        auto it = g_fibers.find(fiber->guest);
        if (it != g_fibers.end() && it->second == fiber)
            g_fibers.erase(it);
    }

    void HostFiberMain(void* param)
    {
        auto* fiber = static_cast<Fiber*>(param);
        PPCContext& ctx = *fiber->ctx;
        uint8_t* base = g_memory.base;
        UpdateDiagnosticContext(&ctx);

        uint32_t start = uint32_t(PPC_LOAD_U64(fiber->guest + FIBER_R31));
        ctx.r1.u64 = uint32_t(PPC_LOAD_U64(fiber->guest + FIBER_R1));
        ctx.r13.u64 = fiber->resumeR13;
        ctx.r31.u64 = start;
        ctx.r3.u64 = start;
#ifndef PPC_CONFIG_SKIP_LR
        ctx.lr = 0;
#endif
        Log("[fiber] fiber 0x%08X starts: routine 0x%08X(0x%08X) r1=0x%08X", fiber->guest, start,
            PPC_LOAD_U32(fiber->guest + FIBER_PARAMETER), ctx.r1.u32);

        sub_823E8D80(ctx, base); // calls the routine with the fiber parameter; KeBugCheck if it returns
        Fatal("[fiber] start routine of fiber 0x%08X returned", fiber->guest);
    }

    // The fiber the calling thread is running now, which the guest knows as `guest` (KTHREAD fiber
    // pointer). The first time, that is the host thread itself, converted to a fiber.
    Fiber* CurrentFiber(uint32_t guest, PPCContext& ctx)
    {
        Fiber* fiber = t_currentFiber;
        if (!fiber)
        {
            void* host = plat::CurrentThreadFiber();
            if (!host)
                Fatal("[fiber] ConvertThreadToFiber failed (%s)", plat::LastErrorText().c_str());

            fiber = new Fiber;
            fiber->host = host;
            fiber->ctx = &ctx;
            fiber->running = true;
            t_currentFiber = fiber;
        }
        else if (fiber->guest == guest)
        {
            return fiber;
        }

        // New thread fiber, or the thread converted back and forth (ConvertFiberToThread +
        // ConvertThreadToFiber): same host fiber, new guest structure.
        Unmap(fiber);
        fiber->guest = guest;
        g_fibers[guest] = fiber;
        return fiber;
    }

    // Host fiber for guest fiber `guest`, created on first use from what CreateFiber stored.
    Fiber* TargetFiber(uint32_t guest, const PPCContext& ctx)
    {
        auto it = g_fibers.find(guest);
        if (it != g_fibers.end())
            return it->second;

        uint32_t lr = LoadBE32(guest + FIBER_LR);
        if (lr != FIBER_START_STUB)
            Fatal("[fiber] SwitchToFiber(0x%08X): unknown fiber (saved LR 0x%08X)", guest, lr);

        auto* fiber = new Fiber;
        fiber->guest = guest;
        fiber->ownedCtx = std::make_unique<PPCContext>(ctx); // FPSCR and the rest of the thread's state
        fiber->ctx = fiber->ownedCtx.get();
        fiber->host = plat::CreateFiber(HOST_FIBER_STACK_RESERVE, HostFiberMain, fiber);
        if (!fiber->host)
            Fatal("[fiber] CreateFiber failed (%s)", plat::LastErrorText().c_str());

        g_fibers[guest] = fiber;
        return fiber;
    }

    // Drops the host side of a guest fiber that is being freed (DeleteFiber, or a fresh CreateFiber
    // reusing the address of one whose deletion was missed). Converted threads keep theirs.
    void ForgetFiber(uint32_t guest)
    {
        std::lock_guard lock(g_fibersMutex);
        auto it = g_fibers.find(guest);
        if (it == g_fibers.end() || it->second->running || !it->second->ownedCtx)
            return;

        Fiber* fiber = it->second;
        g_fibers.erase(it);
        plat::DeleteFiber(fiber->host);
        delete fiber;
    }
}

// void SwitchToFiber(void* fiber)
PPC_FUNC(sub_823E8D70)
{
    uint32_t target = ctx.r3.u32;
    uint32_t kthread = PPC_LOAD_U32(ctx.r13.u32 + 0x100);
    uint32_t current = PPC_LOAD_U32(kthread + KTHREAD_FIBER);

    static std::atomic<uint32_t> s_switches;
    if (s_switches.fetch_add(1, std::memory_order_relaxed) < 16)
        Log("[fiber] SwitchToFiber(0x%08X) from 0x%08X lr=0x%08X", target, current, GuestLR(ctx));

    if (target == current)
        return;
    if (!current || !target)
        Fatal("[fiber] SwitchToFiber(0x%08X) from 0x%08X: not a fiber", target, current);

    Fiber* from;
    Fiber* to;
    {
        std::lock_guard lock(g_fibersMutex);
        from = CurrentFiber(current, ctx);
        to = TargetFiber(target, ctx);
        if (to->running)
            Fatal("[fiber] SwitchToFiber(0x%08X): fiber already running on another thread", target);
        from->running = false;
        to->running = true;
        to->resumeR13 = ctx.r13.u64;
    }

    // What XAPI's switch leaves behind: the current fiber's stack pointer, the KTHREAD fiber
    // pointer and the target's stack bounds (KeSetCurrentStackPointers).
    PPC_STORE_U64(current + FIBER_R1, ctx.r1.u32);
    PPC_STORE_U32(kthread + KTHREAD_FIBER, target);
    PPC_STORE_U32(kthread + KTHREAD_STACK_ALLOC_BASE, PPC_LOAD_U32(target + FIBER_STACK_ALLOC_BASE));
    PPC_STORE_U32(kthread + KTHREAD_STACK_BASE, PPC_LOAD_U32(target + FIBER_STACK_BASE));
    PPC_STORE_U32(kthread + KTHREAD_STACK_LIMIT, PPC_LOAD_U32(target + FIBER_STACK_LIMIT));

    t_currentFiber = to;
    UpdateDiagnosticContext(to->ctx);
    plat::SwitchToFiber(to->host);

    // Resumed, possibly on another thread: take that thread's PCR.
    t_currentFiber = from;
    ctx.r13.u64 = from->resumeR13;
    UpdateDiagnosticContext(&ctx);
}

// void* CreateFiber(u32 stackSize, void* startAddress, void* parameter)
PPC_FUNC(sub_823E8C30)
{
    __imp__sub_823E8C30(ctx, base);
    if (ctx.r3.u32)
        ForgetFiber(ctx.r3.u32);
}

// BOOL ConvertFiberToThread(): frees the current guest fiber; the host thread stays a fiber.
PPC_FUNC(sub_823E8BE0)
{
    if (Fiber* fiber = t_currentFiber)
    {
        std::lock_guard lock(g_fibersMutex);
        Unmap(fiber);
        fiber->guest = 0;
    }
    __imp__sub_823E8BE0(ctx, base);
}

// void DeleteFiber(void* fiber)
PPC_FUNC(sub_823E8D10)
{
    ForgetFiber(ctx.r3.u32);
    __imp__sub_823E8D10(ctx, base);
}
