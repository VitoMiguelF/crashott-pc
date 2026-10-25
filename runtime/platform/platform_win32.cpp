// Windows implementation of platform.h. Behaviour matches what the runtime did with Win32 directly
// before the platform layer (same flags, same handler order).
#include "platform.h"

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <timeapi.h>

#include <algorithm>
#include <array>
#include <mutex>
#include <utility>
#include <vector>

namespace plat
{
    // ---- Time -------------------------------------------------------------------------------
    uint64_t PerfCounter()
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return uint64_t(now.QuadPart);
    }

    uint64_t PerfFrequency()
    {
        static const uint64_t frequency = [] {
            LARGE_INTEGER value;
            QueryPerformanceFrequency(&value);
            return uint64_t(value.QuadPart);
        }();
        return frequency;
    }

    uint64_t TickCountMs()
    {
        return GetTickCount64();
    }

    uint64_t SystemTimeFileTime()
    {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        return (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    }

    void SleepMs(uint32_t milliseconds)
    {
        ::Sleep(milliseconds);
    }

    void YieldThread()
    {
        ::SwitchToThread();
    }

    void RaiseTimerResolution()
    {
        timeBeginPeriod(1);
    }

    struct TimerWait
    {
        HANDLE timer;
        HANDLE wake;
    };

    TimerWait* CreateTimerWait()
    {
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        HANDLE wake = timer ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
        if (!wake)
        {
            if (timer)
                CloseHandle(timer);
            return nullptr;
        }
        return new TimerWait{ timer, wake };
    }

    void WaitTimer(TimerWait* timer, int64_t nanoseconds)
    {
        LARGE_INTEGER due;
        due.QuadPart = -std::max<LONGLONG>(1, nanoseconds / 100); // relative, 100 ns units
        SetWaitableTimerEx(timer->timer, &due, 0, nullptr, nullptr, nullptr, 0);
        HANDLE handles[2] = { timer->wake, timer->timer };
        WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    }

    void WakeTimer(TimerWait* timer)
    {
        SetEvent(timer->wake);
    }

    // ---- Threads ----------------------------------------------------------------------------
    namespace
    {
        struct ThreadStart
        {
            ThreadEntry entry;
            void* param;
        };

        DWORD WINAPI ThreadProc(LPVOID param)
        {
            ThreadStart start = *static_cast<ThreadStart*>(param);
            delete static_cast<ThreadStart*>(param);
            return start.entry(start.param);
        }

        int NativePriority(ThreadPriority priority)
        {
            switch (priority)
            {
            case ThreadPriority::Lowest: return THREAD_PRIORITY_LOWEST;
            case ThreadPriority::AboveNormal: return THREAD_PRIORITY_ABOVE_NORMAL;
            case ThreadPriority::Highest: return THREAD_PRIORITY_HIGHEST;
            default: return THREAD_PRIORITY_NORMAL;
            }
        }
    }

    ThreadHandle CreateThread(ThreadEntry entry, void* param, size_t stackSize, bool suspended, uint32_t* threadId)
    {
        auto* start = new ThreadStart{ entry, param };
        DWORD id = 0;
        HANDLE thread = ::CreateThread(nullptr, stackSize, ThreadProc, start,
                                       (suspended ? CREATE_SUSPENDED : 0) | STACK_SIZE_PARAM_IS_A_RESERVATION, &id);
        if (!thread)
        {
            delete start;
            return nullptr;
        }
        if (threadId)
            *threadId = id;
        return thread;
    }

    uint32_t ResumeThread(ThreadHandle thread)
    {
        DWORD previous = ::ResumeThread(thread);
        return previous == DWORD(-1) ? 0 : previous;
    }

    void JoinThread(ThreadHandle thread)
    {
        WaitForSingleObject(thread, INFINITE);
    }

    void ExitThread(uint32_t exitCode)
    {
        ::ExitThread(exitCode);
    }

    void SetThreadPriority(ThreadHandle thread, ThreadPriority priority)
    {
        ::SetThreadPriority(thread, NativePriority(priority));
    }

    void SetCurrentThreadPriority(ThreadPriority priority)
    {
        ::SetThreadPriority(GetCurrentThread(), NativePriority(priority));
    }

    void SetCurrentThreadName(const char* name)
    {
        wchar_t wide[64];
        int length = MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64);
        if (length > 0)
            SetThreadDescription(GetCurrentThread(), wide);
    }

    uint32_t CurrentThreadId()
    {
        return GetCurrentThreadId();
    }

    // ---- Fibers -----------------------------------------------------------------------------
    FiberHandle CurrentThreadFiber()
    {
        return IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    }

    FiberHandle CreateFiber(size_t stackSize, void (*entry)(void* param), void* param)
    {
        constexpr SIZE_T COMMIT = 256 * 1024;
        return CreateFiberEx(COMMIT, stackSize, FIBER_FLAG_FLOAT_SWITCH, reinterpret_cast<LPFIBER_START_ROUTINE>(entry), param);
    }

    void SwitchToFiber(FiberHandle fiber)
    {
        ::SwitchToFiber(fiber);
    }

    void DeleteFiber(FiberHandle fiber)
    {
        ::DeleteFiber(fiber);
    }

    // ---- Virtual memory ---------------------------------------------------------------------
    namespace
    {
        // Placeholder APIs (Windows 10 1803+), looked up at run time so no extra import library is needed.
        constexpr ULONG X_MEM_RESERVE_PLACEHOLDER = 0x00040000;
        constexpr ULONG X_MEM_REPLACE_PLACEHOLDER = 0x00004000;
        constexpr ULONG X_MEM_PRESERVE_PLACEHOLDER = 0x00000002;
        using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
        using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);

        DWORD NativeAccess(PageAccess access)
        {
            switch (access)
            {
            case PageAccess::ReadOnly: return PAGE_READONLY;
            case PageAccess::ReadWrite: return PAGE_READWRITE;
            default: return PAGE_NOACCESS;
            }
        }
    }

    uint8_t* ReserveMemory(size_t size, uintptr_t preferred)
    {
        auto* base = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(preferred), size, MEM_RESERVE, PAGE_READWRITE));
        if (!base && preferred)
            base = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE));
        return base;
    }

    uint8_t* ReserveMemoryWithViews(size_t size, uintptr_t preferred, size_t sectionSize, const size_t* viewOffsets, size_t viewCount)
    {
        HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
        auto virtualAlloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernelBase, "VirtualAlloc2"));
        auto mapViewOfFile3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernelBase, "MapViewOfFile3"));
        if (!virtualAlloc2 || !mapViewOfFile3)
            return nullptr;

        auto* base = static_cast<uint8_t*>(virtualAlloc2(nullptr, reinterpret_cast<void*>(preferred), size,
                                                         MEM_RESERVE | X_MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!base)
            return nullptr;

        // Split the placeholder at every view boundary: [gap, view, gap, view, ..., rest).
        struct Piece { size_t offset, size; bool view; };
        std::vector<Piece> pieces;
        size_t at = 0;
        for (size_t i = 0; i < viewCount; i++)
        {
            if (viewOffsets[i] > at)
                pieces.push_back({ at, viewOffsets[i] - at, false });
            pieces.push_back({ viewOffsets[i], sectionSize, true });
            at = viewOffsets[i] + sectionSize;
        }
        if (at < size)
            pieces.push_back({ at, size - at, false });

        bool ok = true;
        for (size_t i = 0; ok && i + 1 < pieces.size(); i++)
            ok = VirtualFree(base + pieces[i].offset, pieces[i].size, MEM_RELEASE | X_MEM_PRESERVE_PLACEHOLDER) != 0;

        HANDLE section = ok ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                                 DWORD(uint64_t(sectionSize) >> 32), DWORD(sectionSize), nullptr) : nullptr;
        ok = section != nullptr;
        for (const Piece& piece : pieces)
        {
            if (!ok)
                break;
            ok = piece.view
                ? mapViewOfFile3(section, GetCurrentProcess(), base + piece.offset, 0, piece.size, X_MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) != nullptr
                : virtualAlloc2(nullptr, base + piece.offset, piece.size, MEM_RESERVE | X_MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) != nullptr;
        }
        return ok ? base : nullptr; // a half-built reservation is leaked; the caller falls back to a flat one
    }

    bool CommitMemory(void* address, size_t size)
    {
        return VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    }

    bool DecommitMemory(void* address, size_t size)
    {
        return VirtualFree(address, size, MEM_DECOMMIT) != 0;
    }

    bool ProtectMemory(void* address, size_t size, PageAccess access)
    {
        DWORD old;
        return VirtualProtect(address, size, NativeAccess(access), &old) != 0;
    }

    MemoryRegion QueryMemory(const void* address)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(address, &mbi, sizeof(mbi));
        MemoryRegion region;
        region.base = mbi.BaseAddress;
        region.size = mbi.RegionSize;
        region.state = mbi.State == MEM_COMMIT ? PageState::Committed : mbi.State == MEM_RESERVE ? PageState::Reserved : PageState::Free;
        return region;
    }

    // ---- Fault handling ---------------------------------------------------------------------
    uint64_t FaultInfo::InstructionPointer() const
    {
        return static_cast<CONTEXT*>(native)->Rip;
    }

    void FaultInfo::SetInstructionPointer(uint64_t ip)
    {
        static_cast<CONTEXT*>(native)->Rip = ip;
    }

    uint64_t FaultInfo::Register(unsigned x86Index) const
    {
        return (&static_cast<CONTEXT*>(native)->Rax)[x86Index & 15];
    }

    void FaultInfo::RequestSingleStep()
    {
        static_cast<CONTEXT*>(native)->EFlags |= 0x100; // trap flag
    }

    namespace
    {
        // One vectored handler per registration, each added first, so the order is the one the
        // runtime had with AddVectoredExceptionHandler(1, ...) (newest first).
        constexpr size_t MAX_HANDLERS = 16;
        FaultHandler g_handlers[MAX_HANDLERS];

        template <size_t N>
        LONG CALLBACK Vectored(PEXCEPTION_POINTERS info)
        {
            auto* record = info->ExceptionRecord;
            FaultInfo fault;
            fault.code = record->ExceptionCode;
            fault.native = info->ContextRecord;
            if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
            {
                fault.kind = FaultKind::AccessViolation;
                fault.write = record->ExceptionInformation[0] == 1;
                fault.address = reinterpret_cast<void*>(record->ExceptionInformation[1]);
            }
            else if (record->ExceptionCode == EXCEPTION_SINGLE_STEP)
            {
                fault.kind = FaultKind::SingleStep;
            }
            else
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            return g_handlers[N](fault) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
        }

        template <size_t... N>
        constexpr std::array<PVECTORED_EXCEPTION_HANDLER, sizeof...(N)> MakeThunks(std::index_sequence<N...>)
        {
            return { Vectored<N>... };
        }
        constexpr auto g_thunks = MakeThunks(std::make_index_sequence<MAX_HANDLERS>{});

        std::mutex g_handlersMutex;
        size_t g_handlerCount;
    }

    void AddFaultHandler(FaultHandler handler)
    {
        std::lock_guard lock(g_handlersMutex);
        if (g_handlerCount == MAX_HANDLERS)
            return;
        size_t index = g_handlerCount++;
        g_handlers[index] = handler;
        AddVectoredExceptionHandler(1, g_thunks[index]);
    }

    // ---- Dynamic libraries ------------------------------------------------------------------
    void* OpenLibrary(const std::filesystem::path& path)
    {
        // A full path: its own dependencies are searched next to it (the FFmpeg DLLs).
        if (path.has_parent_path())
            return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        return LoadLibraryW(path.c_str());
    }

    void* FindSymbol(void* library, const char* name)
    {
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
    }

    void CloseLibrary(void* library)
    {
        ::FreeLibrary(static_cast<HMODULE>(library));
    }

    std::string LastErrorText()
    {
        DWORD error = GetLastError();
        return "error " + std::to_string(error);
    }

    // ---- Process ----------------------------------------------------------------------------
    std::filesystem::path ExecutablePath()
    {
        std::wstring buffer(MAX_PATH, L'\0');
        while (true)
        {
            DWORD length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
            if (length == 0)
                return {};
            if (length < buffer.size())
            {
                buffer.resize(length);
                return buffer;
            }
            buffer.resize(buffer.size() * 2);
        }
    }

    namespace
    {
        std::wstring Widen(const std::string& text)
        {
            int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0);
            std::wstring wide(length > 0 ? length : 0, L'\0');
            if (length > 0)
                MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), wide.data(), length);
            return wide;
        }

        std::string Narrow(const wchar_t* text)
        {
            int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            std::string narrow(length > 1 ? length - 1 : 0, '\0');
            if (length > 1)
                WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow.data(), length, nullptr, nullptr);
            return narrow;
        }

        // One argument quoted for CommandLineToArgvW / the CRT (backslashes before a quote doubled).
        std::wstring Quote(const std::wstring& argument)
        {
            if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos)
                return argument;
            std::wstring quoted = L"\"";
            size_t backslashes = 0;
            for (wchar_t c : argument)
            {
                if (c == L'\\')
                {
                    backslashes++;
                    continue;
                }
                quoted.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
                backslashes = 0;
                quoted += c;
            }
            quoted.append(backslashes * 2, L'\\');
            return quoted + L"\"";
        }
    }

    uint32_t CurrentProcessId()
    {
        return GetCurrentProcessId();
    }

    std::vector<std::string> CommandLineArguments()
    {
        std::vector<std::string> arguments;
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
        {
            for (int i = 1; i < argc; i++)
                arguments.push_back(Narrow(argv[i]));
            LocalFree(argv);
        }
        return arguments;
    }

    uint32_t StartSelf(const std::vector<std::string>& arguments)
    {
        std::wstring exe = ExecutablePath().wstring();
        std::wstring commandLine = Quote(exe);
        for (const std::string& argument : arguments)
            commandLine += L" " + Quote(Widen(argument));

        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION process{};
        // Same console (the log keeps going to the same place) and working directory.
        if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process))
            return 0;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return process.dwProcessId;
    }

    bool WaitForProcessExit(uint32_t pid, uint32_t timeoutMs)
    {
        HANDLE process = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
        if (!process)
            return true;
        DWORD wait = WaitForSingleObject(process, timeoutMs);
        CloseHandle(process);
        return wait == WAIT_OBJECT_0;
    }

    void SetEnvironment(const char* name, const char* value)
    {
        SetEnvironmentVariableW(Widen(name).c_str(), value ? Widen(value).c_str() : nullptr);
    }

    bool DisablePowerThrottling()
    {
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        state.StateMask = 0;  // controlled and off: never throttled
        return SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)) != 0;
    }

    // ---- Dialogs ----------------------------------------------------------------------------
    void ShowMessageBox(const char* title, const char* text, MessageKind kind, void* owner)
    {
        UINT icon = kind == MessageKind::Error ? MB_ICONERROR : kind == MessageKind::Warning ? MB_ICONWARNING : MB_ICONINFORMATION;
        MessageBoxW(static_cast<HWND>(owner), Widen(text).c_str(), Widen(title).c_str(), MB_OK | icon);
    }

    std::filesystem::path PickFolder(const char* title, void* owner)
    {
        std::filesystem::path chosen;
        HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        IFileOpenDialog* dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        {
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            dialog->SetTitle(Widen(title).c_str());
            if (dialog->Show(static_cast<HWND>(owner)) == S_OK)
            {
                IShellItem* item = nullptr;
                PWSTR name = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)))
                {
                    chosen = std::filesystem::path(name);
                    CoTaskMemFree(name);
                }
                if (item)
                    item->Release();
            }
            dialog->Release();
        }
        if (SUCCEEDED(init))
            CoUninitialize();
        return chosen;
    }

    int CompareNoCase(const char* a, const char* b)
    {
        return _stricmp(a, b);
    }

    void ExitProcess(uint32_t exitCode)
    {
        ::ExitProcess(exitCode);
    }
}
