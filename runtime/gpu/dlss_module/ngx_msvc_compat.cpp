// The few MSVC runtime pieces NVIDIA's static NGX library (thirdparty/dlss/lib/nvsdk_ngx_s.lib, built
// with Visual Studio for the static CRT) needs when CrashDlss.dll is linked by llvm-mingw instead of
// lld-link + the MSVC libraries. The library's objects are plain COFF, so lld links them as they are;
// everything else they import (the UCRT, __CxxFrameHandler3 and _CxxThrowException, kernel32) comes
// from ucrtbase.dll through mingw-w64's import libraries. Its /DEFAULTLIB:LIBCMT / libcpmt / OLDNAMES
// directives are dropped in CMakeLists.txt.
//
// What is here, and how it differs from the real thing:
// - thread-safe local statics (_Init_thread_header / _footer / _epoch): the same protocol as the
//   MSVC CRT (guard 0 = not started, -1 = running, else the epoch it finished in);
// - the /GS stack cookie and its handlers: the cookie is checked on return like MSVC does; the
//   unwinding handlers skip the check and hand C++ exceptions to __CxxFrameHandler3;
// - operator new / delete with MSVC names and the STL's _Xbad_alloc / _Xlength_error: the library only
//   uses them for small strings and vectors, so running out of memory ends the process with a message
//   instead of throwing std::bad_alloc through MSVC frames.
#if defined(__MINGW32__) && defined(CRASH_HAVE_DLSS)

#include <windows.h>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C"
{
    // --- thread-safe statics --------------------------------------------------------------------
    // Inline code in the library tests `guard > _Init_thread_epoch` (a TLS int) before calling in.
    thread_local int _Init_thread_epoch = INT_MIN;

    namespace
    {
        SRWLOCK g_onceLock = SRWLOCK_INIT;
        CONDITION_VARIABLE g_onceDone = CONDITION_VARIABLE_INIT;
        int g_globalEpoch = INT_MIN;
    }

    void _Init_thread_header(int* once)
    {
        AcquireSRWLockExclusive(&g_onceLock);
        while (*once == -1)  // another thread is initializing it
            SleepConditionVariableSRW(&g_onceDone, &g_onceLock, INFINITE, 0);
        if (*once == 0)
            *once = -1;  // ours to initialize
        else
            _Init_thread_epoch = g_globalEpoch;
        ReleaseSRWLockExclusive(&g_onceLock);
    }

    void _Init_thread_footer(int* once)
    {
        AcquireSRWLockExclusive(&g_onceLock);
        *once = ++g_globalEpoch;
        _Init_thread_epoch = g_globalEpoch;
        ReleaseSRWLockExclusive(&g_onceLock);
        WakeAllConditionVariable(&g_onceDone);
    }

    void _Init_thread_abort(int* once)
    {
        AcquireSRWLockExclusive(&g_onceLock);
        *once = 0;
        ReleaseSRWLockExclusive(&g_onceLock);
        WakeAllConditionVariable(&g_onceDone);
    }

    // --- /GS -----------------------------------------------------------------------------------
    uintptr_t __security_cookie = 0x00002B992DDFA232ull;

    void __security_check_cookie(uintptr_t cookie)
    {
        if (cookie != __security_cookie)
            __fastfail(FAST_FAIL_STACK_COOKIE_CHECK_FAILURE);
    }

    EXCEPTION_DISPOSITION __CxxFrameHandler3(EXCEPTION_RECORD*, void*, CONTEXT*, DISPATCHER_CONTEXT*);

    EXCEPTION_DISPOSITION __GSHandlerCheck(EXCEPTION_RECORD*, void*, CONTEXT*, DISPATCHER_CONTEXT*)
    {
        return ExceptionContinueSearch;
    }

    // HandlerData starts with __CxxFrameHandler3's FuncInfo RVA (the GS data follows it).
    EXCEPTION_DISPOSITION __GSHandlerCheck_EH(EXCEPTION_RECORD* record, void* frame, CONTEXT* context, DISPATCHER_CONTEXT* dispatcher)
    {
        return __CxxFrameHandler3(record, frame, context, dispatcher);
    }

    int _fltused = 0x9875;

    // --- operator new / delete and the STL's error paths, under their MSVC names --------------
    [[noreturn]] static void OutOfMemory(const char* what)
    {
        fprintf(stderr, "[dlss] NGX library: %s\n", what);
        fflush(stderr);
        abort();
    }

    void* MsvcNew(size_t size) __asm__("??2@YAPEAX_K@Z");
    void* MsvcNew(size_t size)
    {
        void* p = malloc(size ? size : 1);
        if (!p)
            OutOfMemory("out of memory");
        return p;
    }

    void MsvcDelete(void* p) __asm__("??3@YAXPEAX@Z");
    void MsvcDelete(void* p) { free(p); }

    [[noreturn]] void MsvcXbadAlloc() __asm__("?_Xbad_alloc@std@@YAXXZ");
    void MsvcXbadAlloc() { OutOfMemory("std::bad_alloc"); }

    [[noreturn]] void MsvcXlengthError(const char* message) __asm__("?_Xlength_error@std@@YAXPEBD@Z");
    void MsvcXlengthError(const char* message) { OutOfMemory(message ? message : "std::length_error"); }
}

#endif
