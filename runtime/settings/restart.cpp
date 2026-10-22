// Restarting the game from the settings screen (settings.h, "Restart").
//
// The options the game itself reads at boot (music/effects/voice volume, language, mojo colours,
// HD AI Remaster) can't be changed under it; instead of asking the player to close and reopen
// the game, the settings screen offers to restart it. The new process gets the same executable,
// arguments, working directory and environment, plus "--restarted <pid>": it waits for this one
// to exit before starting (window, audio device, saves) and skips the intros (direct_boot.h: the
// logo videos and the title screen, straight to the main menu).
// Per-run overrides that would undo the new choices or replay a test script from the start are
// dropped: CRASH_SETTINGS (its values are already in settings.toml), CRASH_INPUT_SCRIPT,
// CRASH_REPLAY, CRASH_RECORD_INPUT and CRASH_BOOT (the new process stops at the main menu).
//
// CRASH_RESTART: "relaunch" (default), "exit" = exit with code 4 and let the caller start the
// game again (tools/run_game.py, which tracks the process), "off" = no restart (the options
// apply on the next launch, as before).

#include "settings.h"
#include "../diagnostics.h"
#include "../gpu/window.h"
#include "../kernel/input.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace settings
{
    namespace
    {
        bool g_restarted = false;

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

    RestartMode GetRestartMode()
    {
        static const RestartMode mode = [] {
            const char* value = getenv("CRASH_RESTART");
            if (!value || !*value || _stricmp(value, "relaunch") == 0)
                return RestartMode::Relaunch;
            if (_stricmp(value, "exit") == 0)
                return RestartMode::Exit;
            if (_stricmp(value, "off") != 0 && _stricmp(value, "0") != 0)
                Log("[settings] CRASH_RESTART=%s: unknown value, restarting is off", value);
            return RestartMode::Off;
        }();
        return mode;
    }

    void HandleRestartArguments(int argc, char** argv)
    {
        for (int i = 1; i + 1 < argc; i++)
        {
            if (strcmp(argv[i], "--restarted") != 0)
                continue;
            g_restarted = true;
            DWORD parent = DWORD(strtoul(argv[i + 1], nullptr, 10));
            // The previous instance may still be closing: its window, audio device and files.
            if (HANDLE process = parent ? OpenProcess(SYNCHRONIZE, FALSE, parent) : nullptr)
            {
                DWORD wait = WaitForSingleObject(process, 10000);
                CloseHandle(process);
                Log("[settings] restarted from the settings screen (previous process %lu %s)", parent,
                    wait == WAIT_OBJECT_0 ? "has exited" : "is still running after 10 s");
            }
            else
            {
                Log("[settings] restarted from the settings screen");
            }
            return;
        }
    }

    bool Restarted()
    {
        return g_restarted;
    }

    void Restart()
    {
        Save();
        RestartMode mode = GetRestartMode();
        if (mode == RestartMode::Exit)
        {
            input::StopRecording();
            Log("[settings] restart: exiting with code %d for the caller to start the game again (CRASH_RESTART=exit)", RESTART_EXIT_CODE);
            ExitProcess(RESTART_EXIT_CODE);
        }

        // Same executable and arguments, without a previous --restarted <pid> nor --boot <spec>.
        wchar_t exe[MAX_PATH];
        DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring commandLine = Quote(std::wstring(exe, length));
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
        {
            for (int i = 1; i < argc; i++)
            {
                if (wcscmp(argv[i], L"--restarted") == 0 || wcscmp(argv[i], L"--boot") == 0)
                {
                    i++;
                    continue;
                }
                commandLine += L" " + Quote(argv[i]);
            }
            LocalFree(argv);
        }
        commandLine += L" --restarted " + std::to_wstring(GetCurrentProcessId());

        // The environment is inherited, minus the per-run overrides (see the top of the file).
        for (const wchar_t* name : { L"CRASH_SETTINGS", L"CRASH_INPUT_SCRIPT", L"CRASH_REPLAY", L"CRASH_RECORD_INPUT", L"CRASH_BOOT" })
            SetEnvironmentVariableW(name, nullptr);
        // The window comes back where it was.
        int x, y, width, height;
        if (gpu::window::Placement(x, y, width, height))
            SetEnvironmentVariableW(L"CRASH_RESTART_WINDOW",
                                    (std::to_wstring(x) + L"," + std::to_wstring(y) + L"," + std::to_wstring(width) + L"," + std::to_wstring(height)).c_str());

        input::StopRecording();
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION process{};
        // Same console (the log keeps going to the same place) and working directory.
        if (!CreateProcessW(exe, commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process))
        {
            Log("[settings] restart failed: CreateProcess error %lu; the options apply on the next launch", GetLastError());
            return;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        Log("[settings] restarting: new process %lu", process.dwProcessId);
        ExitProcess(0);
    }
}
