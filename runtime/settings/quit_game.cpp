#include "quit_game.h"
#include "settings.h"
#include "../diagnostics.h"
#include "../audio/audio_output.h"
#include "../gpu/renderer.h"
#include "../kernel/input.h"
#include "../platform/platform.h"

#include <atomic>
#include <thread>

namespace settings
{
    namespace
    {
        std::atomic<bool> g_requested{ false };
        std::atomic<bool> g_finishing{ false };
    }

    void RequestQuit()
    {
        if (g_requested.exchange(true))
            return;
        Log("[quit] EXIT GAME chosen on the main menu: closing the game");
        Save();
        input::StopRecording();
        if (!gpu::renderer::IsActive())
            FinishQuit();
        std::thread([] {
            plat::SleepMs(3000);
            Log("[quit] no present in 3 s: exiting without waiting for the renderer");
            FinishQuit();
        }).detach();
    }

    bool QuitRequested()
    {
        return g_requested.load(std::memory_order_relaxed);
    }

    void FinishQuit()
    {
        if (g_finishing.exchange(true))
        {
            for (;;)
                plat::SleepMs(1000);  // the other thread is exiting
        }
        audio::Shutdown();
        Log("[quit] exiting with code 0");
        plat::ExitProcess(0);
    }
}
