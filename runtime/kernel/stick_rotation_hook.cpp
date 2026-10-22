// Logs when the game itself recognises a stick rotation (runtime/NOTES.md "Controles de PC").
//
// sub_821EC4A8(device, dt) feeds each controller's stick samples of the frame to its two rotation
// detectors (device+492 left stick, device+496 right stick; sub_821EC3E0). A detector lights one of
// 8 octant slots (sub_821EB4D0: stick past 0.6) for 0.75 s (slots decay at 1.333/s, sub_821EB438;
// the stick back in the middle clears them) and says "rotating" (byte +40) when at least 7 of the 8
// are lit, then holds that for 0.2 s. The spin moves (Old Skool, Spin Dismount, Gyro Jackhammer...)
// read it. This hook only watches byte +40 to log the rising edges, so tests can check that the
// keyboard's RotateStick / SpinAttack (pc_input.cpp) satisfy the game's own detector.

#include "kernel.h"
#include "memory.h"

extern "C" PPC_FUNC(__imp__sub_821EC4A8);

PPC_FUNC(sub_821EC4A8)
{
    uint32_t device = ctx.r3.u32;
    __imp__sub_821EC4A8(ctx, base);
    // One state per controller object (the game keeps one per user).
    static uint32_t s_devices[4];
    static uint8_t s_was[4][2];
    static int s_count[2];
    int slot = 0;
    while (slot < 4 && s_devices[slot] && s_devices[slot] != device)
        slot++;
    if (slot == 4)
        return;
    s_devices[slot] = device;
    for (int stick = 0; stick < 2; stick++)
    {
        uint32_t detector = LoadBE32(device + 492 + 4 * stick);
        if (!detector)
            continue;
        uint8_t rotating = *(base + detector + 40);
        if (rotating && !s_was[slot][stick] && ++s_count[stick] <= 200)
            Log("[input] game: %s stick rotation detected on controller %d (#%d)", stick ? "right" : "left", slot, s_count[stick]);
        s_was[slot][stick] = rotating;
    }
}
