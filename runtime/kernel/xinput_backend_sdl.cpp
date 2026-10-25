// SDL3 gamepads as the "Xbox" pads of pad_slots.cpp (xinput_backend.h): slot n is the n-th
// gamepad SDL lists, by button position (gamepad_sdl.h), so the Steam Deck's own controls play like
// an Xbox pad. PlayStation pads are left to hid_pads_sdl.cpp, as XInput leaves them to the HID reader
// on Windows (their icons). The window thread pumps SDL's events, which is what refreshes the state
// read here.
#include "xinput_backend.h"
#include "gamepad_sdl.h"
#include "../diagnostics.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace input::xinput
{
    namespace
    {
        constexpr uint32_t SLOTS = 4;

        std::mutex g_mutex;
        std::atomic<uint32_t> g_generation{ 1 };
        uint32_t g_seenGeneration = 0;
        SDL_Gamepad* g_pads[SLOTS];
        XINPUT_GAMEPAD g_last[SLOTS];
        uint32_t g_packet[SLOTS];

        // Reopens the gamepads in SDL's order after one arrived or left.
        void Refresh()
        {
            uint32_t generation = g_generation;
            if (generation == g_seenGeneration || !SDL_WasInit(SDL_INIT_GAMEPAD))
                return;
            g_seenGeneration = generation;
            for (SDL_Gamepad*& pad : g_pads)
            {
                if (pad)
                    SDL_CloseGamepad(pad);
                pad = nullptr;
            }
            int count = 0;
            SDL_JoystickID* ids = SDL_GetGamepads(&count);
            uint32_t slot = 0;
            for (int i = 0; ids && i < count && slot < SLOTS; i++)
            {
                if (sdl::IsPlayStation(SDL_GetGamepadTypeForID(ids[i])))
                    continue;
                g_pads[slot] = SDL_OpenGamepad(ids[i]);
                if (g_pads[slot])
                {
                    Log("[input] gamepad %u: %s", slot, SDL_GetGamepadName(g_pads[slot]));
                    slot++;
                }
            }
            SDL_free(ids);
        }
    }

    void DevicesChanged()
    {
        g_generation++;
    }

    bool GetState(uint32_t slot, XINPUT_STATE& state)
    {
        if (slot >= SLOTS)
            return false;
        std::lock_guard lock(g_mutex);
        Refresh();
        SDL_Gamepad* pad = g_pads[slot];
        if (!pad || !SDL_GamepadConnected(pad))
            return false;

        XINPUT_GAMEPAD gamepad = sdl::Read(pad);
        if (memcmp(&gamepad, &g_last[slot], sizeof(gamepad)) != 0)
        {
            g_last[slot] = gamepad;
            g_packet[slot]++;
        }
        state.dwPacketNumber = g_packet[slot];
        state.Gamepad = gamepad;
        return true;
    }

    void SetState(uint32_t slot, uint16_t left, uint16_t right)
    {
        if (slot >= SLOTS)
            return;
        std::lock_guard lock(g_mutex);
        if (SDL_Gamepad* pad = g_pads[slot])
            SDL_RumbleGamepad(pad, left, right, 0);  // 0 ms: until the next call, like XInput
    }
}
