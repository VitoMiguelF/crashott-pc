// No direct PlayStation HID reader outside Windows yet: SDL3 reads DualSense / DualShock 4 pads
// as gamepads (xinput_backend_sdl.cpp). A hidraw port of hid_pads_win32.cpp would bring back the
// PlayStation icons detection and the touchpad.
#include "hid_pads.h"
#include "../diagnostics.h"

namespace input::hid
{
    void Start() {}
    void Rescan() {}
    std::vector<PadInfo> Pads() { return {}; }
    bool ReadPad(uint32_t, XINPUT_GAMEPAD&) { return false; }
    void SetPadVibration(uint32_t, uint16_t, uint16_t) {}
    bool RecentlyActive() { return false; }
    bool HiddenPads() { return false; }

    int Count(const char** firstName)
    {
        if (firstName)
            *firstName = nullptr;
        return 0;
    }

    int SelfTest()
    {
        Log("[hid] the PlayStation HID reader is Windows-only (hid_pads_win32.cpp)");
        return 1;
    }

    int Monitor(int)
    {
        return SelfTest();
    }
}
