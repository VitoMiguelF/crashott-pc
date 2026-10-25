#pragma once

#include "xinput_types.h"

#include <cstdint>

// The "Xbox" pads pad_slots.cpp reads, slots 0-3: XInput on Windows (xinput_backend_win32.cpp),
// SDL3 gamepads elsewhere (xinput_backend_sdl.cpp: every SDL gamepad, PlayStation ones included,
// mapped onto the XInput layout by position). The PlayStation HID reader (hid_pads.h) is extra,
// Windows-only for now.
namespace input::xinput
{
    // The state of slot `slot`, false when no pad is there.
    bool GetState(uint32_t slot, XINPUT_STATE& state);
    // Motors 0..65535 (left = low frequency).
    void SetState(uint32_t slot, uint16_t left, uint16_t right);
    // A pad arrived or left (the window saw it): re-list the pads. XInput needs nothing.
    void DevicesChanged();
}
