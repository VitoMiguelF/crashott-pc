#include "guest_thread.h"
#include "../diagnostics.h"
#include "../kernel/memory.h"
#include "../kernel/objects.h"
#include "../kernel/threads.h"

#include <windows.h>

namespace
{
    constexpr uint32_t KPROCESS_SIZE = 0x60;

    uint32_t g_guestProcess;
    uint32_t g_defaultStackSize = 0x40000;
}

void InitGuestProcess(uint32_t defaultStackSize)
{
    g_defaultStackSize = defaultStackSize;
    g_guestProcess = GuestAlloc(KPROCESS_SIZE);
    StoreBE32(g_guestProcess + 0x1C, defaultStackSize);
}

uint32_t GuestProcessDefaultStackSize()
{
    return g_defaultStackSize;
}

GuestThreadContext::GuestThreadContext(uint32_t cpuNumber, uint32_t threadId, uint32_t stackSize)
{
    stackSize = (stackSize + 0xFFF) & ~0xFFFu;
    block = GuestAlloc(stackSize + PCR_SIZE + TLS_SIZE + TEB_SIZE, 0x1000, STACK_GUARD_SIZE);

    pcr = block + stackSize;
    tls = pcr + PCR_SIZE;
    kthread = tls + TLS_SIZE;

    StoreBE32(pcr + 0x0, tls);                   // PCR: TLS slot array
    StoreBE32(pcr + 0x100, kthread);             // PCR: current thread
    *g_memory.Translate(pcr + 0x10C) = uint8_t(cpuNumber); // PCR: current processor
    StoreBE32(tls + 0x10, 0xFFFFFFFF);           // TLS entry the CRT expects to start as -1 (as in UnleashedRecomp)
    StoreBE32(kthread + 0x84, g_guestProcess);   // KTHREAD: process
    StoreBE32(kthread + 0x14C, threadId);        // KTHREAD: thread id

    ctx.r1.u64 = pcr - STACK_TOP_RESERVE;
    ctx.r13.u64 = pcr;
    ctx.fpscr.loadFromHost();
}

uint32_t RunGuestThread(uint32_t entry, uint32_t stackSize)
{
    ThreadObject* thread = CreateGuestThread(entry, 0, 0, stackSize, false, 0);
    // The title's main thread runs the game loop and records the GPU commands: one of the two
    // threads the frame rate depends on (the other is the command processor's).
    SetThreadPriority(thread->hostHandle, THREAD_PRIORITY_ABOVE_NORMAL);
    WaitForSingleObject(thread->hostHandle, INFINITE);
    return thread->exitCode;
}
