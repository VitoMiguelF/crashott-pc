#pragma once

// On-screen touch controls (Android; touch_controls.cpp, docs/internals/android_port.md "Touch
// controls"): the game playable on a phone without a gamepad.
//
// - In gameplay: a floating left stick (a finger on the left of the screen puts its centre there;
//   it stays there, the knob stopping at the edge),
//   A / B / X / Y on the right in the pad's diamond, RT (block / dodge) and a spin button (the
//   left stick going round with X tapped, like the keyboard's spin attack) beside it, START and
//   BACK at the top right. A drag on the rest of the right half turns the camera (right stick).
//   A finger may slide from one button to the next.
//   The action buttons show Crash's shadow doing the action on a see-through fire plate (A jump,
//   X punch, Y flying kick, B reaching for a mutant, RT guard, spin), drawn from shapes here.
// - In the menus (the frontend's screens, the pause menu, the intro videos) nothing is drawn and
//   the touch is the mouse: a tap selects and accepts, as a left click does on PC, and a tap on a
//   prompt ("(B) BACK", "CONTINUE STORY (Y)") presses its button (prompt_icons.h ButtonAt). So do
//   the settings screen and the console.
// - In a cutscene or an intro video: no controls either; a finger held anywhere skips it.
// - A pad in use hides the controls until the next touch.
//
// CRASH_TOUCH_CONTROLS=0 turns them off on Android, =1 on elsewhere (a touch screen on SDL).

#include "../kernel/xinput_types.h"

#include <cstdint>

namespace touch_controls
{
    bool Enabled();

    // Window thread: a finger of the touch screen; (x, y) 0..1 in the window. False when the touch
    // is not for the controls: the window then makes it the mouse (the menus).
    enum class Phase { Down, Move, Up };
    bool OnFinger(uint64_t finger, Phase phase, float x, float y);

    // Controller 0 as the game reads it (kernel/xam_input_imports.cpp, the pad alone, before the
    // keyboard), `poll` the game's own read: the controls' buttons and sticks on top.
    void Apply(XINPUT_GAMEPAD& pad, bool poll);

    // Render thread, once per presented frame, over the back buffer.
    void Draw(uint32_t width, uint32_t height);
}
