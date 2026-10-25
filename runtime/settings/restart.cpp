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

#include "../platform/platform.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace settings
{
    namespace
    {
        bool g_restarted = false;
    }

    RestartMode GetRestartMode()
    {
        static const RestartMode mode = [] {
            const char* value = getenv("CRASH_RESTART");
            if (!value || !*value || plat::CompareNoCase(value, "relaunch") == 0)
                return RestartMode::Relaunch;
            if (plat::CompareNoCase(value, "exit") == 0)
                return RestartMode::Exit;
            if (plat::CompareNoCase(value, "off") != 0 && plat::CompareNoCase(value, "0") != 0)
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
            uint32_t parent = uint32_t(strtoul(argv[i + 1], nullptr, 10));
            // The previous instance may still be closing: its window, audio device and files.
            bool exited = plat::WaitForProcessExit(parent, 10000);
            Log("[settings] restarted from the settings screen (previous process %u %s)", parent,
                exited ? "has exited" : "is still running after 10 s");
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
            plat::ExitProcess(RESTART_EXIT_CODE);
        }

        // Same executable and arguments, without a previous --restarted <pid> nor --boot <spec>.
        std::vector<std::string> arguments;
        std::vector<std::string> current = plat::CommandLineArguments();
        for (size_t i = 0; i < current.size(); i++)
        {
            if (current[i] == "--restarted" || current[i] == "--boot")
            {
                i++;
                continue;
            }
            arguments.push_back(current[i]);
        }
        arguments.push_back("--restarted");
        arguments.push_back(std::to_string(plat::CurrentProcessId()));

        // The environment is inherited, minus the per-run overrides (see the top of the file).
        for (const char* name : { "CRASH_SETTINGS", "CRASH_INPUT_SCRIPT", "CRASH_REPLAY", "CRASH_RECORD_INPUT", "CRASH_BOOT" })
            plat::SetEnvironment(name, nullptr);
        // The window comes back where it was.
        int x, y, width, height;
        if (gpu::window::Placement(x, y, width, height))
            plat::SetEnvironment("CRASH_RESTART_WINDOW",
                                 (std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(width) + "," + std::to_string(height)).c_str());

        input::StopRecording();
        uint32_t pid = plat::StartSelf(arguments);
        if (!pid)
        {
            Log("[settings] restart failed (%s); the options apply on the next launch", plat::LastErrorText().c_str());
            return;
        }
        Log("[settings] restarting: new process %u", pid);
        plat::ExitProcess(0);
    }
}
