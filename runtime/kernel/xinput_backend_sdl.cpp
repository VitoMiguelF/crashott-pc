// SDL3 gamepads as the "Xbox" pads of pad_slots.cpp (xinput_backend.h): slot n is the n-th
// gamepad SDL lists, by button position (South = A, East = B, West = X, North = Y), so a DualSense
// or the Steam Deck's own controls play like an Xbox pad. The window thread pumps SDL's events,
// which is what refreshes the state read here.
#include "xinput_backend.h"
#include "../diagnostics.h"

#include <SDL3/SDL.h>

#include <algorithm>
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
            for (int i = 0; ids && i < count && i < int(SLOTS); i++)
            {
                g_pads[i] = SDL_OpenGamepad(ids[i]);
                if (g_pads[i])
                    Log("[input] gamepad %d: %s", i, SDL_GetGamepadName(g_pads[i]));
            }
            SDL_free(ids);
        }

        int16_t Axis(SDL_Gamepad* pad, SDL_GamepadAxis axis, bool invert)
        {
            int value = SDL_GetGamepadAxis(pad, axis);
            if (invert)
                value = -value - 1;  // SDL: down is positive; XInput: up is positive
            return int16_t(std::clamp(value, -32768, 32767));
        }

        uint8_t Trigger(SDL_Gamepad* pad, SDL_GamepadAxis axis)
        {
            int value = std::max<int>(SDL_GetGamepadAxis(pad, axis), 0);
            return uint8_t(value * 255 / 32767);
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

        static constexpr struct { SDL_GamepadButton button; uint16_t mask; } BUTTONS[] = {
            { SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP },
            { SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
            { SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT },
            { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
            { SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START },
            { SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK },
            { SDL_GAMEPAD_BUTTON_TOUCHPAD, XINPUT_GAMEPAD_BACK },  // like hid_pads: the touchpad click is BACK
            { SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB },
            { SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER },
            { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER },
            { SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A },
            { SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B },
            { SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X },
            { SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y },
        };
        XINPUT_GAMEPAD gamepad{};
        for (const auto& b : BUTTONS)
        {
            if (SDL_GetGamepadButton(pad, b.button))
                gamepad.wButtons |= b.mask;
        }
        gamepad.bLeftTrigger = Trigger(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        gamepad.bRightTrigger = Trigger(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        gamepad.sThumbLX = Axis(pad, SDL_GAMEPAD_AXIS_LEFTX, false);
        gamepad.sThumbLY = Axis(pad, SDL_GAMEPAD_AXIS_LEFTY, true);
        gamepad.sThumbRX = Axis(pad, SDL_GAMEPAD_AXIS_RIGHTX, false);
        gamepad.sThumbRY = Axis(pad, SDL_GAMEPAD_AXIS_RIGHTY, true);
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
