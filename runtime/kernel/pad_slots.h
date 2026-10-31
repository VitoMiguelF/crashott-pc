#pragma once

// Which physical controller drives which game controller (the XamInput user index 0-3), so that
// local co-op works with any mix of pads (pad_slots.cpp, runtime/NOTES.md "Co-op local").
//
// Every XInput pad and every PlayStation pad read over HID (hid_pads.h) is one device. A PlayStation
// pad that DS4Windows/DSX also present as a virtual Xbox pad (both visible) is recognised by its
// presses showing up on that XInput pad at the same moment, and the two become one device; until
// then (its first presses) it drives nothing. Devices get players in connection order, or as
// [controls] player1 / player2 choose ("auto" | "xbox1".."xbox4" | "playstation1".."playstation4",
// settings/controls.h), and keep them until unplugged. Player 1 is on the game controller that
// pressed START on the title screen (the game's own choice, 0 before that); with player1 = "auto"
// the device that pressed it becomes player 1. The keyboard and the mouse drive player 1 with its pad
// (a single pad is player 1 with them), unless a player's row says "keyboard": then they are that player's
// alone and the pads go to the others (the keyboard on player 1 and controller 1 on player 2, say).

#include "xinput_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace input::slots
{
    // The physical controller on game controller `user` (0-3) now; false when none is there. The
    // keyboard and the scripted pads (CRASH_INPUT_SCRIPT) are added by xam_input_imports.cpp.
    bool Read(uint32_t user, XINPUT_GAMEPAD& pad);
    // XamInputSetState's motors (0..65535) for the device on `user`; 0 with [controls] vibration off.
    void SetVibration(uint32_t user, uint16_t left, uint16_t right);
    // The light bar (0xRRGGBB) and DualSense player LEDs (1-8, 0 = off) of the PlayStation pad on
    // `user` (pad_lights.h). Nothing for an Xbox pad, nor for a PlayStation pad that DS4Windows/DSX
    // present as a virtual Xbox pad (the lights are theirs).
    void SetLight(uint32_t user, uint32_t rgb, uint8_t player);
    // The game controller of pad player `player` (0-3; player 1 = PlayerOneUser()).
    uint32_t UserOfPlayer(int player);

    // The game controller of player 1 (the one the game took on the title screen), where the
    // keyboard and the mouse go.
    uint32_t PlayerOneUser();
    // The game controller the keyboard and the mouse drive: player 1's, or that of the player whose row is
    // "keyboard" ([controls] player1 / player2).
    uint32_t KeyboardUser();

    // The devices connected now, for the settings screen: settings key ("xbox1", "playstation2"),
    // name ("Controle 1", "DualSense (USB)") and player (0 = player 1, -1 = none).
    struct Device
    {
        std::string key, name;
        int player;
    };
    std::vector<Device> Devices();

    // --pads-selftest: plays scripted plug / press sequences through the assignment and checks the
    // players each device gets; exit code.
    int SelfTest();
}
