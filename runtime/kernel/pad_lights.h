#pragma once

// The light bar of the PlayStation pads (DualSense, DualShock 4) follows what each player controls
// (pad_lights.cpp; [controls] lightbar, SETTINGS > CONTROLS > LIGHT BAR):
//
//   - Crash on foot: the player's colour (orange; in co-op and online the colour each player has, the
//     multiplayer one; a skin's costume colour with the normal orange);
//   - a titan jacked: that titan's colour (the TITANS table, by the fight tree sub type of the titan);
//   - knocked out: red blinking; the game paused: the colour breathing.
//
// The DualSense's player LEDs show the player's number (online: the slot in the session). Nothing is
// sent to Xbox pads nor to a PlayStation pad that DSX / DS4Windows hold (pad_slots.h, hid_pads.h).
//
//   CRASH_LIGHTBAR_TRACE=1   logs every change of a player's state (on foot, titan, KO, pause) and colour

namespace input::lights
{
    // The game thread, once per game read of the controllers (xam_input_imports.cpp); acts every 30 ms.
    void Tick();
}
