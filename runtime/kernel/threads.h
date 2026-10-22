#pragma once

#include <cstdint>
#include <functional>

struct ThreadObject;

// Guest threads: each one is a host thread (64 MB host stack, since recompiled code nests one host
// frame per guest call) with its own GuestThreadContext (stack, PCR, TLS, KTHREAD). The KTHREAD
// doubles as the thread's dispatcher object, signaled when the thread exits.

// Creates a thread that runs guest function `entry` with r3/r4 set. With `suspended` it waits
// for ResumeGuestThread. `stackSize` 0 means the process default.
ThreadObject* CreateGuestThread(uint32_t entry, uint32_t r3, uint32_t r4, uint32_t stackSize, bool suspended, uint32_t cpu);

// Returns the previous suspend count.
uint32_t ResumeGuestThread(ThreadObject* thread);

// Marks the calling guest thread as exited with `exitCode` and ends its host thread.
[[noreturn]] void ExitCurrentGuestThread(uint32_t exitCode);

// Thread driven by runtime code instead of a guest entry point: `body` runs on a new host thread
// that has its own guest context (stack, PCR, KTHREAD), so it can call guest callbacks with
// CallGuestFunction (GPU interrupts). Shows up in diagnostics like any guest thread.
struct PPCContext;
ThreadObject* CreateRuntimeGuestThread(const char* name, uint32_t cpu, std::function<void(PPCContext&)> body);

// Calls guest function `function` with r3/r4 on the calling thread's context, restoring r1 after.
void CallGuestFunction(PPCContext& ctx, uint32_t function, uint32_t r3, uint32_t r4);

// Calls `callback` for every running guest thread (with the thread list locked).
void ForEachGuestThread(const std::function<void(ThreadObject&)>& callback);
