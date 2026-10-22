#pragma once

#include <cstdint>

struct PPCContext;

void Log(const char* format, ...);
[[noreturn]] void Fatal(const char* format, ...);
// Called after every Log line with its text (at most 1023 characters), outside the log lock; it
// must not Log itself. One observer: the replay's WAIT entries (kernel/input_record.cpp).
void SetLogObserver(void (*observer)(const char* line));

// Crash reporting: a vectored handler that turns host faults inside recompiled code into a
// report (exception, nearest guest function, guest LR/SP, last import called), and an optional
// watchdog that stops the run after CRASH_TIMEOUT seconds.
void InstallCrashHandler();
void StartWatchdog();

// Called on each thread running guest code: its context is reported if it crashes, and every
// registered thread is reported on timeout. ClearDiagnosticContext unregisters the calling thread.
void SetDiagnosticContext(PPCContext* ctx);
void ClearDiagnosticContext();
// Replaces the calling thread's registered context (guest fibers switch it; see kernel/fibers.cpp).
void UpdateDiagnosticContext(PPCContext* ctx);

// Guest address of the recompiled function whose host code contains `hostAddress`, or 0.
uint32_t FindGuestFunction(uintptr_t hostAddress);

// Prints the per-import call counts and the order in which imports were first called.
void PrintImportSummary();

// Extra state printed in crash/timeout/fatal reports (e.g. GPU counters). Register before the
// guest starts.
void AddDiagnosticReporter(void (*reporter)());

// CRASH_MEMDIFF_TIMES: guest memory snapshots and diffs on the input clock (diagnostics_memdiff.cpp).
// Called on each game read of controller 0.
void MemDiffTick();
