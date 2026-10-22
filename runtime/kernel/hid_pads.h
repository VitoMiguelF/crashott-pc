#pragma once

// PlayStation pads (DualSense, DualSense Edge, DualShock 4) read directly over HID (hid_pads.cpp),
// over USB or Bluetooth, mapped onto the XInput gamepad the game reads: Cross = A, Circle = B,
// Square = X, Triangle = Y, L1/R1 = LB/RB, L2/R2 = LT/RT, Options = START, Create/Share and the
// touchpad click = BACK. XInput only sees Xbox pads (and pads that DS4Windows/DSX turn into a
// virtual Xbox pad: then both are read and merged, and the icons stay PlayStation).
//
// A thread scans for pads every 2 s and when Windows says a device arrived (hot-plug), and reads
// each open pad on its own thread. The n-th PlayStation pad found is merged into XInput user n.

#include <windows.h>
#include <xinput.h>

#include <cstdint>

namespace input::hid
{
    // Starts the scan thread (once; also started by the first Read).
    void Start();
    // A device arrived or left (WM_DEVICECHANGE): rescan now.
    void Rescan();
    // Merges the PlayStation pad of `user` into `pad` (buttons OR'd, the stick or trigger pushed
    // further wins). False when no PlayStation pad is on that user.
    bool Read(uint32_t user, bool xinputConnected, XINPUT_GAMEPAD& pad);
    // Rumble for the PlayStation pad of `user` (XamInputSetState's motors, 0..65535). Only used
    // when no XInput pad is on that user (DS4Windows/DSX forward the virtual pad's rumble).
    void SetVibration(uint32_t user, uint16_t left, uint16_t right);
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
