#pragma once

#include <ppc_config.h>
#include <ppc_context.h>

// Per-thread guest state, following UnleashedRecomp's cpu/guest_thread.cpp: one guest block
// holding the stack, the PCR (r13), the TLS slots and the KTHREAD/TEB. Unlike UnleashedRecomp the
// stack comes first, above 64 KB of uncommitted guard, so an overflow faults instead of
// overwriting the PCR/TEB.
struct GuestThreadContext
{
    static constexpr uint32_t PCR_SIZE = 0xAB0;
    static constexpr uint32_t TLS_SIZE = 0x100;
    static constexpr uint32_t TEB_SIZE = 0x2E0;
    static constexpr uint32_t STACK_GUARD_SIZE = 0x10000;
    // Space left above the initial r1, for callers' frame fields the entry function may touch.
    static constexpr uint32_t STACK_TOP_RESERVE = 0x100;

    PPCContext ctx{};
    uint32_t block{};
    uint32_t pcr{};
    uint32_t tls{};
    uint32_t kthread{};

    GuestThreadContext(uint32_t cpuNumber, uint32_t threadId, uint32_t stackSize);
};

// KPROCESS every KTHREAD points to (KTHREAD+0x84). XAPI reads the default stack size from it
// (+0x1C) when a thread or fiber is created with stack size 0. Call once before any thread starts.
void InitGuestProcess(uint32_t defaultStackSize);
uint32_t GuestProcessDefaultStackSize();

// Runs guest function `entry` as the title's main thread (see kernel/threads.h) and waits for it
// to finish. Returns its exit code (r3 on return, or the ExTerminateThread argument).
uint32_t RunGuestThread(uint32_t entry, uint32_t stackSize);
