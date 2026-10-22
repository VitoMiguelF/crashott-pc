#include "threads.h"
#include "memory.h"
#include "objects.h"
#include "../cpu/guest_thread.h"
#include "../diagnostics.h"

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
    constexpr SIZE_T HOST_STACK_SIZE = 64 * 1024 * 1024;

    // Guest stacks are never smaller than this, whatever the title asks for: the recompiled
    // code uses the same frames as the original, but runtime helpers may need slack.
    constexpr uint32_t MIN_GUEST_STACK_SIZE = 0x10000;

    std::mutex g_threadsMutex;
    std::vector<ThreadObject*> g_threads;

    struct RuntimeThreadObject : ThreadObject
    {
        const char* name{};
        std::function<void(PPCContext&)> body;
    };

    void FinishThread(ThreadObject* thread, uint32_t exitCode)
    {
        {
            std::lock_guard lock(DispatcherMutex());
            thread->exitCode = exitCode;
            thread->exited = true;
            thread->SetGuestSignalState(1);
        }
        NotifyDispatcher();

        {
            std::lock_guard lock(g_threadsMutex);
            std::erase(g_threads, thread);
        }

        Log("[thread] guest thread %u (entry 0x%08X) exited with 0x%08X", thread->threadId, thread->entry, exitCode);
        ClearDiagnosticContext();
        SetCurrentThreadObject(nullptr);
    }

    DWORD WINAPI GuestThreadProc(LPVOID param)
    {
        auto* thread = static_cast<ThreadObject*>(param);
        auto& ctx = thread->context->ctx;

        SetCurrentThreadObject(thread);
        SetDiagnosticContext(&ctx);

        if (auto* runtimeThread = dynamic_cast<RuntimeThreadObject*>(thread))
        {
            Log("[thread] starting runtime thread %u (%s): r1=0x%08X r13=0x%08X",
                thread->threadId, runtimeThread->name, ctx.r1.u32, ctx.r13.u32);
            runtimeThread->body(ctx);
            FinishThread(thread, 0);
            return 0;
        }

        Log("[thread] starting guest thread %u: entry 0x%08X(r3=0x%08X, r4=0x%08X) r1=0x%08X r13=0x%08X",
            thread->threadId, thread->entry, thread->entryR3, thread->entryR4, ctx.r1.u32, ctx.r13.u32);

        ctx.r3.u64 = thread->entryR3;
        ctx.r4.u64 = thread->entryR4;
        PPCFunc* function = PPC_LOOKUP_FUNC(g_memory.base, thread->entry);
        function(ctx, g_memory.base);

        FinishThread(thread, ctx.r3.u32);
        return 0;
    }

    ThreadObject* StartThread(ThreadObject* thread, uint32_t stackSize, bool suspended, uint32_t cpu)
    {
        thread->type = DISPATCHER_THREAD;

        // Always created suspended, so the guest block exists before the thread runs.
        DWORD hostId;
        HANDLE host = CreateThread(nullptr, HOST_STACK_SIZE, GuestThreadProc, thread,
                                   CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &hostId);
        if (!host)
            Fatal("CreateThread failed (%lu)", GetLastError());

        if (stackSize == 0)
            stackSize = GuestProcessDefaultStackSize();

        thread->hostHandle = host;
        thread->threadId = hostId;
        thread->context = new GuestThreadContext(cpu, hostId, std::max(stackSize, MIN_GUEST_STACK_SIZE));
        RegisterObject(thread, thread->context->kthread);

        {
            std::lock_guard lock(g_threadsMutex);
            g_threads.push_back(thread);
        }

        if (!suspended)
            ResumeThread(host);

        return thread;
    }
}

ThreadObject* CreateGuestThread(uint32_t entry, uint32_t r3, uint32_t r4, uint32_t stackSize, bool suspended, uint32_t cpu)
{
    auto* thread = new ThreadObject();
    thread->entry = entry;
    thread->entryR3 = r3;
    thread->entryR4 = r4;
    return StartThread(thread, stackSize, suspended, cpu);
}

ThreadObject* CreateRuntimeGuestThread(const char* name, uint32_t cpu, std::function<void(PPCContext&)> body)
{
    auto* thread = new RuntimeThreadObject();
    thread->name = name;
    thread->body = std::move(body);
    return StartThread(thread, 0, false, cpu);
}

void CallGuestFunction(PPCContext& ctx, uint32_t function, uint32_t r3, uint32_t r4)
{
    uint64_t r1 = ctx.r1.u64;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    PPCFunc* guestFunction = PPC_LOOKUP_FUNC(g_memory.base, function);
    guestFunction(ctx, g_memory.base);
    ctx.r1.u64 = r1;
}

uint32_t ResumeGuestThread(ThreadObject* thread)
{
    DWORD previous = ResumeThread(thread->hostHandle);
    return previous == DWORD(-1) ? 0 : previous;
}

void ExitCurrentGuestThread(uint32_t exitCode)
{
    ThreadObject* thread = CurrentThreadObject();
    if (!thread)
        Fatal("ExTerminateThread called outside a guest thread");

    FinishThread(thread, exitCode);
    ExitThread(exitCode);
}

void ForEachGuestThread(const std::function<void(ThreadObject&)>& callback)
{
    std::lock_guard lock(g_threadsMutex);
    for (auto* thread : g_threads)
        callback(*thread);
}
