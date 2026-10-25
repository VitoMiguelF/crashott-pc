#pragma once

// An SDL3 gamepad read as the XInput layout, by button position (South = A, East = B, West = X,
// North = Y), for xinput_backend_sdl.cpp (Xbox and other pads) and hid_pads_sdl.cpp (PlayStation pads).

#include "xinput_types.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace input::sdl
{
    // DualSense, DualShock 4 / 3 (the "PlayStation" pads of hid_pads.h: their icons, their own ids).
    inline bool IsPlayStation(SDL_GamepadType type)
    {
        return type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
    }

    inline int16_t Axis(SDL_Gamepad* pad, SDL_GamepadAxis axis, bool invert)
    {
        int value = SDL_GetGamepadAxis(pad, axis);
        if (invert)
            value = -value - 1;  // SDL: down is positive; XInput: up is positive
        return int16_t(std::clamp(value, -32768, 32767));
    }

    inline uint8_t Trigger(SDL_Gamepad* pad, SDL_GamepadAxis axis)
    {
        int value = std::max<int>(SDL_GetGamepadAxis(pad, axis), 0);
        return uint8_t(value * 255 / 32767);
    }

    inline XINPUT_GAMEPAD Read(SDL_Gamepad* pad)
    {
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
        return gamepad;
    }
}
