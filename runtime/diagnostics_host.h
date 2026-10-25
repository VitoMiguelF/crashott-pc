#pragma once

#include <cstddef>
#include <cstdint>

struct PPCContext;

// The OS side of diagnostics.cpp: crash handlers, what a host address is, and sampling a guest
// thread for the watchdog. Windows: diagnostics_win32.cpp (vectored handler, SuspendThread);
// POSIX: diagnostics_posix.cpp (the platform fault handler, no thread sampling yet).
namespace diag
{
    // ---- given by diagnostics.cpp ----
    void LogGuestRegisters(const PPCContext* ctx);
    void ReportGuestState(const char* reason);  // the "========== reason ==========" report

    // ---- given by the OS backend ----
    // "nearest guest function ..." for the exe's code, "<module>+0xOFFSET" otherwise.
    void DescribeHostAddress(uintptr_t address, char* buffer, size_t size);
    // Registered after the demand-commit handler, so faults it resolves never get here.
    void InstallHostCrashHandlers();
    // Ends the process at once with `code` (no atexit, no destructors).
    [[noreturn]] void Terminate(uint32_t code);
    // A handle the watchdog can sample the calling thread through; released by CloseThread.
    void* OpenCurrentThread();
    void CloseThread(void* thread);
    // The host instruction pointer of `thread` (suspended for the read, left suspended: the
    // watchdog ends the process next). False if unknown.
    bool SampleThread(void* thread, uintptr_t& rip);
}
