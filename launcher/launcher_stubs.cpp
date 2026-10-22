// The few runtime functions the launcher links against without needing them: the log goes to
// launcher.log next to the exe instead of the console; the PlayStation / keyboard button icons
// and the archive rewrites of the language packs are game-only (the launcher shows the game's
// own Xbox icons and never opens the archive for the game).

#include "../runtime/diagnostics.h"
#include "../runtime/fs/rcf_overlay.h"
#include "../runtime/settings/prompt_icons.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace
{
    std::mutex g_logMutex;
    FILE* g_log;

    FILE* LogFile()
    {
        if (!g_log)
        {
            wchar_t path[MAX_PATH];
            DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring file(path, length);
#ifdef CRASH_INSTALLER
            file = file.substr(0, file.find_last_of(L"\\/") + 1) + L"CrashSetup.log";
#else
            file = file.substr(0, file.find_last_of(L"\\/") + 1) + L"launcher.log";
#endif
            g_log = _wfopen(file.c_str(), L"w");
        }
        return g_log;
    }
}

void Log(const char* format, ...)
{
    std::lock_guard lock(g_logMutex);
    if (FILE* file = LogFile())
    {
        va_list args;
        va_start(args, format);
        vfprintf(file, format, args);
        va_end(args);
        fputc('\n', file);
        fflush(file);
    }
}

[[noreturn]] void Fatal(const char* format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    Log("[fatal] %s", text);
    MessageBoxA(nullptr, text, "CrashLauncher", MB_OK | MB_ICONERROR);
    std::exit(1);
}

void SetLogObserver(void (*)(const char*)) {}

namespace settings::prompts
{
    uint32_t Style() { return 0; }
    bool Draw(char32_t, uint32_t, int, int, const uint32_t*, std::vector<uint32_t>&) { return false; }
}

namespace rcf_overlay
{
    void RegisterEntryRewrite(const std::string&, const std::string&, const std::string&, EntryPatcher) {}
}
