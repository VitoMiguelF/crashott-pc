// CrashRecomp runtime entry point: sets up guest memory, maps the XEX image, fills the indirect
// call table and runs the title's entry point (_xstart) on a guest thread.
//
// Usage: CrashRecomp.exe [--game <folder>] [--mods <folder>] [--userdata <folder>]
//                        [--languages <folder>] [--data <folder>] [--xex <default.xex>] [--image <image.bin>]
//   Every folder defaults to one next to the exe (paths.h): game\ (or [paths] game of
//   settings.toml, or a folder picker on the first run), mods\, userdata\, languages\, data\.
//   --game      game folder, mounted read-only as D:\
//   --mods      mod overlay folder
//   --userdata  writable storage for saves, settings.toml and caches
//   --languages language packs (settings/language_pack.h)
//   --data      optional data derived from the game: subtitles\, symbols\, remaster_full\ / remaster_l1\
//   --xex       default.xex (default: <game>\default.xex); --image a decrypted image.bin instead of
//               decrypting the XEX at startup (xex_image/)
//   --settings-preview <folder>  draws the settings screen to PNG files and exits (settings_menu.h)
//   --hid-selftest  checks the PlayStation pad report parsing (kernel/hid_pads.h) and exits
//   --hid-monitor [seconds]  lists the PlayStation pads and prints their input (kernel/hid_pads.h)
//   --pads-selftest  checks which player each controller gets (local co-op, kernel/pad_slots.h) and exits
//   --prompt-preview <folder>  draws the button icons (Xbox / PlayStation / keyboard) to PNG files and
//               exits (settings/prompt_icons.h)
//   --precompile-shaders  translates and compiles every known game shader, both variants, into
//               userdata\shadercache on every core, and exits (gpu/shader_warmup.h)
//   --restarted <pid>  given by a restart from the settings screen (settings.h, Restart): waits for
//               the previous process to exit and skips the intros
// Environment: CRASH_TIMEOUT=<seconds> stops the run (with a report) after that long.

#include "audio/audio_output.h"
#include "audio/game_volumes.h"
#include "cpu/guest_thread.h"
#include "dev/dev_console.h"
#include "diagnostics.h"
#include "fs/vfs.h"
#include "gpu/gpu.h"
#include "gpu/renderer.h"
#include "gpu/shader_warmup.h"
#include "kernel/memory.h"
#include "kernel/xex_loader.h"
#include "kernel/hid_pads.h"
#include "kernel/pad_slots.h"
#include "kernel/pc_input.h"
#include "paths.h"
#include "settings/direct_boot.h"
#include "settings/prompt_icons.h"
#include "settings/language_pack.h"
#include "settings/settings.h"
#include "settings/settings_menu.h"
#include "subtitles/subtitles.h"

#include <windows.h>
#include <timeapi.h>

#include <cstdio>
#include <cstring>
#include <filesystem>

int main(int argc, char** argv)
{
    // Every folder, relative to the exe unless given on the command line (paths.h).
    if (!paths::Init(argc, argv))
        return 1;
    vfs::Config files{ paths::Game(), paths::Mods(), paths::UserData() };

    // After a restart from the settings screen: the previous process may still be exiting.
    settings::HandleRestartArguments(argc, argv);
    boot::HandleArguments(argc, argv);  // --skip-intros, --boot (settings/direct_boot.h)
    dev::console::HandleArguments(argc, argv);  // --dev-console (dev/dev_console.h)

    if (!g_memory.Init())
    {
        Log("could not reserve 4 GB of guest address space");
        return 1;
    }

    Log("[memory] guest memory reserved at host %p", g_memory.base);
    InstallCrashHandler();
    // 1 ms timer resolution: the guest vblank (gpu.cpp) and the command processor's waits use timed
    // waits, which otherwise round up to the 15.6 ms default tick and make the frame pacing uneven.
    timeBeginPeriod(1);
    // Out of Windows 11's power throttling (EcoQoS) for background windows: with the window out of
    // focus it lowered the clocks of the game's threads and ignored the 1 ms timer resolution
    // above (~100 fps instead of ~125 in the village; README "Desempenho").
    {
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        state.StateMask = 0;  // controlled and off: never throttled
        if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)))
            Log("[main] could not opt out of power throttling (%lu)", GetLastError());
    }

    XexModule module;
    if (!LoadXex(paths::Xex(), paths::Image(), module))
        return 1;

    InitFunctionTable();
    vfs::Init(files);
    // userdata\settings.toml, and the data patches behind the main menu's SETTINGS entry; then
    // the texts of the language pack chosen there, if any (after the SETTINGS label patch).
    settings::LoadLanguagePacks(paths::Languages());
    settings::Load(vfs::UserDataDir());
    // First run: the game folder picked in the dialog goes to settings.toml ([paths] game).
    if (paths::GameChosenNow())
    {
        settings::Values values = settings::Get();
        values.gamePath = reinterpret_cast<const char*>(paths::Game().generic_u8string().c_str());
        settings::Set(values);
        settings::Save();
    }
    // Music / effects / voice volumes, through the game's own volume categories (live).
    {
        settings::Values values = settings::Get();
        audio::volumes::SetPercent(values.musicVolume, values.effectsVolume, values.voiceVolume);
    }
    // HD AI Remaster (graphics.hd_remaster): adds the remaster mod of data\ to the mod list.
    settings::EnableHdRemaster();
    settings::RegisterFrontendPatches();
    settings::RegisterMojoPatches();
    settings::RegisterLanguagePack();
    // Subtitles of that language (<data>/subtitles, languages\<id>\subtitles.txt), if any.
    subtitles::Init();
    // The game's fonts and frames for the settings screen, read from default.rcf on a thread.
    settings::menu::Init();
    input::pc::Init();  // keyboard / mouse: menus vs gameplay from Frontend.bfig, PlayStation pads

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--hid-selftest") == 0)
            return input::hid::SelfTest();
        if (strcmp(argv[i], "--pads-selftest") == 0)
            return input::slots::SelfTest();
        if (strcmp(argv[i], "--hid-monitor") == 0)
            return input::hid::Monitor(i + 1 < argc ? atoi(argv[i + 1]) : 20);
        if (strcmp(argv[i], "--precompile-shaders") == 0)
            return gpu::warmup::Precompile(vfs::UserDataDir() / "shadercache") ? 2 : 0;
    }
    for (int i = 1; i + 1 < argc; i++)
    {
        if (strcmp(argv[i], "--test-shaders") == 0)
            return gpu::renderer::TestShaders(argv[i + 1]) ? 2 : 0;
        if (strcmp(argv[i], "--settings-preview") == 0)
            return settings::menu::Preview(argv[i + 1]);
        if (strcmp(argv[i], "--prompt-preview") == 0)
            return settings::prompts::Preview(argv[i + 1]);
    }
    StartWatchdog();

    // The main thread's stack size comes from the XEX header; 256 KB if absent (UnleashedRecomp's default).
    uint32_t stackSize = module.defaultStackSize ? module.defaultStackSize : 0x40000;
    InitGuestProcess(stackSize);
    gpu::Init();
    audio::Init();
    uint32_t result = RunGuestThread(module.entryPoint, stackSize);

    Log("[thread] guest entry point returned, r3=0x%08X", result);
    PrintImportSummary();
    return 0;
}
