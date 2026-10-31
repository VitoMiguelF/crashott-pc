#pragma once

// The cheat menu (cheat_menu.cpp; docs/dev_features.md "Menu de cheats", README "Cheats").
//
// Opened from the game's pause with a code, as the game's own cheats are (pause, hold RT, X X Y A =
// Big Head, Y X Y A = Shadow Crash; dev_cheats.cpp):
//   controller   hold RT, then Y Y X X
//   keyboard     hold Shift, then Tab Tab Backspace Backspace (the menus' Y and X)
//   touch, mouse tap the top-left, top-right, top-left and top-right corners of the screen
// Not in an online session (the other machines wouldn't have them).
//
// Its items: INVINCIBILITY, 1 HIT TITAN and DISABLE AI (on / off, until the game is closed), +1 LIFE,
// and UNLOCK ALL MOVES / SKINS / EPISODES / EVERYTHING. The unlocks are the game's own objective
// steps (dev_library.lua.inc): they are permanent, written to the save the next time the game saves,
// so each asks for a second A first. Drawn in the game's style (its message box, fonts and icons), in
// the game's language (English, French, German, Spanish, Italian, Dutch) or Portuguese. Up / down
// (D-pad, stick, arrows / WASD) choose, A (Enter, Space, a tap) does it, B / BACK / START (Esc)
// closes it; the mouse and the touch screen point at the items.

#include "../kernel/xinput_types.h"

#include <cstdint>
#include <filesystem>

struct PPCContext;

namespace dev::cheat_menu
{
    // Controller 0 as the game is about to see it (kernel/xam_input_imports.cpp, player 1, after the
    // keyboard; `poll` = the game's own read): watches for the code in the pause, drives the menu
    // while it is open and then hides the pad from the game (and until every button is released).
    void FilterGameInput(XINPUT_GAMEPAD& pad, bool poll);
    // The game must see nothing pressed (other players' pads, the menus' mouse).
    bool BlocksGameInput();
    bool IsOpen();

    // Game thread, every script update (dev_tools.cpp): the actions that call into the game (lives,
    // unlocks, the menu's sounds).
    void RunPending(PPCContext& ctx, uint8_t* base);

    // Render thread, once per presented frame, over the back buffer.
    void Draw(uint32_t width, uint32_t height);

    // --cheats-preview <dir>: writes the menu in every language at a few window sizes to PNG files
    // (over a flat background) without starting the game. Returns the exit code.
    int Preview(const std::filesystem::path& directory);
}
