// Sampling profiler for CrashRecomp.exe (no admin, no ETW): every interval it suspends each thread
// of the target that used CPU since the last tick, reads its stack with StackWalk64 (the exe's
// .pdata covers the recompiled functions too) and resumes it. Output: one line per sample,
//   S <tid> <cycles since last tick> <rip> <return address 1> ...   (hex, absolute)
// plus "M <exe base>" and "T <tid> <total cycles>" at the end. tools/perf/profile.py names the
// addresses with the linker map (build\CrashRecomp.map) and private\symbols\functions.csv.
//
//   sampler.exe <pid> <seconds> <interval_us> <out.txt> [max_frames]
//
// Build with the bundled toolchain: toolchain\llvm-mingw\bin\clang++ -O2 -std=c++20 -static tools\perf\sampler.cpp -o build\perf\sampler.exe -ldbghelp -lpsapi -lwinmm
// (or with vcvars64 + LLVM on PATH: clang-cl /O2 /EHsc /std:c++20 tools\perf\sampler.cpp /Fe:build\perf\sampler.exe /Fo:build\perf\ /link winmm.lib)
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

struct Thread
{
    HANDLE handle;
    ULONG64 lastCycles;
    ULONG64 firstCycles;
};

static void RefreshThreads(DWORD pid, std::map<DWORD, Thread>& threads)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{ sizeof(entry) };
    for (BOOL ok = Thread32First(snap, &entry); ok; ok = Thread32Next(snap, &entry))
    {
        if (entry.th32OwnerProcessID != pid || threads.count(entry.th32ThreadID))
            continue;
        HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!handle)
            continue;
        ULONG64 cycles = 0;
        QueryThreadCycleTime(handle, &cycles);
        threads[entry.th32ThreadID] = { handle, cycles, cycles };
    }
    CloseHandle(snap);
}

int main(int argc, char** argv)
{
    if (argc < 5)
    {
        fprintf(stderr, "usage: sampler <pid> <seconds> <interval_us> <out.txt> [max_frames]\n");
        return 1;
    }
    DWORD pid = strtoul(argv[1], nullptr, 0);
    double seconds = atof(argv[2]);
    int intervalUs = atoi(argv[3]);
    int maxFrames = argc > 5 ? atoi(argv[5]) : 48;

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process)
    {
        fprintf(stderr, "OpenProcess(%lu) failed: %lu\n", pid, GetLastError());
        return 1;
    }
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    if (!SymInitialize(process, nullptr, TRUE))
        fprintf(stderr, "SymInitialize failed: %lu\n", GetLastError());

    HMODULE exe = nullptr;
    DWORD needed;
    EnumProcessModules(process, &exe, sizeof(exe), &needed);

    FILE* out = fopen(argv[4], "w");
    fprintf(out, "M %llx\n", (unsigned long long)exe);

    timeBeginPeriod(1);
    std::map<DWORD, Thread> threads;
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    LONGLONG next = start.QuadPart;
    LONGLONG step = frequency.QuadPart * intervalUs / 1000000;
    uint64_t samples = 0, ticks = 0;
    std::vector<DWORD64> stack;

    while (true)
    {
        QueryPerformanceCounter(&now);
        if (now.QuadPart - start.QuadPart > LONGLONG(seconds * frequency.QuadPart))
            break;
        DWORD code = 0;
        if (!GetExitCodeProcess(process, &code) || code != STILL_ACTIVE)
            break;
        if (ticks % 200 == 0)
            RefreshThreads(pid, threads);
        ticks++;

        for (auto& [tid, thread] : threads)
        {
            ULONG64 cycles = 0;
            if (!QueryThreadCycleTime(thread.handle, &cycles))
                continue;
            ULONG64 delta = cycles - thread.lastCycles;
            thread.lastCycles = cycles;
            if (delta < 20000)   // did not run (or barely) since the last tick
                continue;
            if (SuspendThread(thread.handle) == DWORD(-1))
                continue;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread.handle, &context))
            {
                stack.clear();
                STACKFRAME64 frame{};
                frame.AddrPC.Offset = context.Rip;
                frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = context.Rbp;
                frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Offset = context.Rsp;
                frame.AddrStack.Mode = AddrModeFlat;
                stack.push_back(context.Rip);
                while ((int)stack.size() < maxFrames &&
                       StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread.handle, &frame, &context, nullptr,
                                   SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                {
                    if (!frame.AddrReturn.Offset)
                        break;
                    stack.push_back(frame.AddrReturn.Offset);
                }
                fprintf(out, "S %lu %llu", tid, (unsigned long long)delta);
                for (DWORD64 address : stack)
                    fprintf(out, " %llx", (unsigned long long)address);
                fputc('\n', out);
                samples++;
            }
            ResumeThread(thread.handle);
        }

        next += step;
        QueryPerformanceCounter(&now);
        LONGLONG wait = next - now.QuadPart;
        if (wait > 0)
        {
            DWORD ms = DWORD(wait * 1000 / frequency.QuadPart);
            if (ms)
                Sleep(ms);
            else
                while (QueryPerformanceCounter(&now), now.QuadPart < next)
                    YieldProcessor();
        }
        else
            next = now.QuadPart;
    }

    for (auto& [tid, thread] : threads)
        fprintf(out, "T %lu %llu\n", tid, (unsigned long long)(thread.lastCycles - thread.firstCycles));
    QueryPerformanceCounter(&now);
    fprintf(out, "D %.3f %llu\n", double(now.QuadPart - start.QuadPart) / frequency.QuadPart, (unsigned long long)ticks);
    fclose(out);
    fprintf(stderr, "%llu samples in %llu ticks, %zu threads\n", (unsigned long long)samples, (unsigned long long)ticks, threads.size());
    return 0;
}
