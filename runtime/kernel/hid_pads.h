#pragma once

// PlayStation pads (DualSense, DualSense Edge, DualShock 4) read directly over HID (hid_pads.cpp),
// over USB or Bluetooth, mapped onto the XInput gamepad the game reads: Cross = A, Circle = B,
// Square = X, Triangle = Y, L1/R1 = LB/RB, L2/R2 = LT/RT, Options = START, Create/Share and the
// touchpad click = BACK. XInput only sees Xbox pads (and pads that DS4Windows/DSX turn into a
// virtual Xbox pad: then both are read and merged, and the icons stay PlayStation).
//
// A thread scans for pads every 2 s and when Windows says a device arrived (hot-plug), and reads
// each open pad on its own thread. Which game controller (player) each pad drives is decided in
// pad_slots.h.

#include <windows.h>
#include <xinput.h>

#include <cstdint>
#include <string>
#include <vector>

namespace input::hid
{
    // Starts the scan thread (once; also started by the first Read).
    void Start();
    // A device arrived or left (WM_DEVICECHANGE): rescan now.
    void Rescan();
    // The PlayStation pads connected now, in connection order: an id each (never reused, so one
    // that is unplugged and plugged again is a new pad) and its name ("DualSense (USB)").
    struct PadInfo
    {
        uint32_t id;
        std::string name;
    };
    std::vector<PadInfo> Pads();
    // The current state of pad `id`; false when it is gone.
    bool ReadPad(uint32_t id, XINPUT_GAMEPAD& pad);
    // Rumble for pad `id` (XamInputSetState's motors, 0..65535). Not used for a pad that
    // DS4Windows/DSX also present as a virtual Xbox pad (they forward that pad's rumble).
    void SetPadVibration(uint32_t id, uint16_t left, uint16_t right);
    // A PlayStation pad had input in the last second.
    bool RecentlyActive();
    // A PlayStation pad is present but another program holds it (DSX, DS4Windows, HidHide) and
    // presents it as a virtual Xbox pad: XInput input then counts as PlayStation for the icons.
    bool HiddenPads();
    // Number of PlayStation pads connected, and the name of the first ("DualSense (USB)").
    int Count(const char** firstName = nullptr);
    // --hid-selftest: parses one synthetic report of each layout and checks the CRC-32; exit code.
    int SelfTest();
    // --hid-monitor <seconds>: lists the PlayStation pads and prints every change of their state;
    // Cross held makes the pad rumble.
    int Monitor(int seconds);
}
