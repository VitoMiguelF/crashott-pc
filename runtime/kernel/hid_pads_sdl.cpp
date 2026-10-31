// PlayStation pads (DualSense, DualShock 4 / 3) through SDL3 outside Windows: SDL's HIDAPI drivers
// already parse their reports, so this only gives them the hid_pads.h interface. They get their own
// ids (SDL_JoystickID, never reused) and leave the "Xbox" slots of xinput_backend_sdl.cpp, which is
// what makes pad_slots.cpp treat them as PlayStation pads: their presses show PlayStation icons.
#include "hid_pads.h"
#include "gamepad_sdl.h"
#include "pc_input.h"
#include "../diagnostics.h"
#include "../platform/platform.h"

#include <atomic>
#include <cstdlib>
#include <mutex>

namespace input::hid
{
    namespace
    {
        struct Pad
        {
            SDL_JoystickID id;
            SDL_Gamepad* gamepad;
            std::string name;
            XINPUT_GAMEPAD state{};
            uint32_t light = 0;  // 0x80000000 | player << 24 | 0xRRGGBB last set (0 = never)
        };

        std::mutex g_mutex;
        std::vector<Pad> g_pads;
        std::atomic<uint32_t> g_generation{ 1 };
        uint32_t g_seenGeneration = 0;
        std::atomic<uint64_t> g_lastActive{ 0 };

        // Same threshold as hid_pads_win32.cpp: buttons, a trigger past 60, a stick past half way.
        bool Active(const XINPUT_GAMEPAD& a, const XINPUT_GAMEPAD& b)
        {
            auto moved = [](int16_t x, int16_t y) { return std::abs(int(x) - int(y)) > 8000 && (std::abs(int(x)) > 12000 || std::abs(int(y)) > 12000); };
            return a.wButtons != b.wButtons || std::abs(int(a.bLeftTrigger) - int(b.bLeftTrigger)) > 60 ||
                   std::abs(int(a.bRightTrigger) - int(b.bRightTrigger)) > 60 || moved(a.sThumbLX, b.sThumbLX) ||
                   moved(a.sThumbLY, b.sThumbLY) || moved(a.sThumbRX, b.sThumbRX) || moved(a.sThumbRY, b.sThumbRY);
        }

        std::string Name(SDL_Gamepad* gamepad)
        {
            SDL_GamepadType type = SDL_GetGamepadType(gamepad);
            std::string name = type == SDL_GAMEPAD_TYPE_PS5 ? "DualSense" : type == SDL_GAMEPAD_TYPE_PS4 ? "DualShock 4" : "DualShock 3";
            switch (SDL_GetGamepadConnectionState(gamepad))
            {
            case SDL_JOYSTICK_CONNECTION_WIRED: return name + " (USB)";
            case SDL_JOYSTICK_CONNECTION_WIRELESS: return name + " (Bluetooth)";
            default: return name;
            }
        }

        // Opens the PlayStation pads that arrived and closes the ones that left (g_mutex held).
        void Refresh()
        {
            uint32_t generation = g_generation;
            if (generation == g_seenGeneration || !SDL_WasInit(SDL_INIT_GAMEPAD))
                return;
            g_seenGeneration = generation;
            int count = 0;
            SDL_JoystickID* ids = SDL_GetGamepads(&count);
            auto listed = [&](SDL_JoystickID id) { return std::find(ids, ids + (ids ? count : 0), id) != ids + (ids ? count : 0); };
            std::erase_if(g_pads, [&](Pad& pad) {
                if (listed(pad.id))
                    return false;
                Log("[input] %s disconnected", pad.name.c_str());
                SDL_CloseGamepad(pad.gamepad);
                return true;
            });
            for (int i = 0; ids && i < count; i++)
            {
                bool open = std::any_of(g_pads.begin(), g_pads.end(), [&](const Pad& pad) { return pad.id == ids[i]; });
                if (open || !sdl::IsPlayStation(SDL_GetGamepadTypeForID(ids[i])))
                    continue;
                if (SDL_Gamepad* gamepad = SDL_OpenGamepad(ids[i]))
                {
                    g_pads.push_back({ ids[i], gamepad, Name(gamepad) });
                    Log("[input] %s connected (SDL: %s)", g_pads.back().name.c_str(), SDL_GetGamepadName(gamepad));
                }
            }
            SDL_free(ids);
        }

        Pad* Find(uint32_t id)
        {
            for (Pad& pad : g_pads)
            {
                if (pad.id == id)
                    return &pad;
            }
            return nullptr;
        }
    }

    void Start() {}

    void Rescan()
    {
        g_generation++;
    }

    std::vector<PadInfo> Pads()
    {
        std::lock_guard lock(g_mutex);
        Refresh();
        std::vector<PadInfo> pads;
        for (const Pad& pad : g_pads)
        {
            if (SDL_GamepadConnected(pad.gamepad))
                pads.push_back({ uint32_t(pad.id), pad.name });
        }
        return pads;
    }

    bool ReadPad(uint32_t id, XINPUT_GAMEPAD& state)
    {
        std::lock_guard lock(g_mutex);
        Pad* pad = Find(id);
        if (!pad || !SDL_GamepadConnected(pad->gamepad))
            return false;
        state = sdl::Read(pad->gamepad);
        if (Active(state, pad->state))
        {
            g_lastActive = plat::TickCountMs();
            input::pc::NoteDevice(input::pc::Device::PlayStation);
        }
        pad->state = state;
        return true;
    }

    void SetPadVibration(uint32_t id, uint16_t left, uint16_t right)
    {
        std::lock_guard lock(g_mutex);
        if (Pad* pad = Find(id))
            SDL_RumbleGamepad(pad->gamepad, left, right, 0);  // 0 ms: until the next call, like XInput
    }

    // SDL's HIDAPI drivers write the light bar (DualSense, DualShock 4) and, from the player index,
    // the DualSense's player LEDs; a pad SDL reads another way (no HIDAPI) just returns false.
    void SetPadLight(uint32_t id, uint32_t rgb, uint8_t player)
    {
        std::lock_guard lock(g_mutex);
        Pad* pad = Find(id);
        uint32_t light = 0x80000000u | uint32_t(player) << 24 | (rgb & 0xFFFFFF);
        if (!pad || pad->light == light)
            return;
        if ((pad->light >> 24 & 0x7F) != player || !pad->light)
            SDL_SetGamepadPlayerIndex(pad->gamepad, player ? int(player) - 1 : -1);
        SDL_SetGamepadLED(pad->gamepad, uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb));
        pad->light = light;
    }

    bool RecentlyActive()
    {
        uint64_t last = g_lastActive.load();
        return last && plat::TickCountMs() - last < 1000;
    }

    // No program hides the pads behind a virtual Xbox pad here (that is DS4Windows/DSX on Windows).
    bool HiddenPads()
    {
        return false;
    }

    int Count(const char** firstName)
    {
        std::lock_guard lock(g_mutex);
        Refresh();
        if (firstName)
            *firstName = g_pads.empty() ? nullptr : g_pads.front().name.c_str();
        return int(g_pads.size());
    }

    int SelfTest()
    {
        Log("[hid] SDL parses the PlayStation reports outside Windows: no self-test (hid_pads_win32.cpp)");
        return 1;
    }

    int Monitor(int)
    {
        return SelfTest();
    }
}
