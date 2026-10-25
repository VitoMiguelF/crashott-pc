#pragma once

// The save screens of the main menu as a PC game (save_screens.cpp; README "Nome e saves no PC",
// runtime/NOTES.md "Teclado do nome e apagar saves"):
//
// - NEW GAME > ENTER YOUR NAME: the game's letter wheel is replaced by an on-screen QWERTY
//   keyboard drawn in the game's style (its fonts and colours) where the wheel was. D-pad / stick
//   move between the keys, A types the highlighted one, X erases, Y or START is DONE, B goes back
//   (the game's own hints). The mouse points at keys and clicks them; a physical keyboard types
//   directly (letters and digits), with Enter = DONE, Backspace = erase and Esc = back. Every key
//   goes through the game's own code (CNameEntryScreenAction: the "add the wheel's letter" and
//   "erase" functions, its sounds and its DONE flag), so the name reaches the save exactly as the
//   wheel would give it: up to 5 characters, A-Z and 0-9.
// - LOAD GAME / NEW GAME > CHOOSE GAME SLOT: X (Delete / Backspace on the keyboard, or a click on
//   the prompt) on a slot with a save asks "delete this saved game?" in the game's style (NO / YES,
//   NO highlighted); YES deletes it with the game's own XenonSaveDrive delete (XamContentDelete,
//   kernel/xam_content_imports.cpp) and the game reads the slots again (its "access the save
//   folder" screens), back on the same slot. Deleted saves cannot be recovered.
//
// Drawn by the presenter over the game (gpu/xenos_renderer.cpp), like the settings screen.

#include "../kernel/xinput_types.h"

#include <cstdint>

namespace settings::save_screens
{
    // The frontend tree's node shown (frontend_hooks.cpp, every frame). Game thread.
    void NoteFrontendNode(uint32_t node);
    // The exit the frontend takes from `node` (frontend_hooks.cpp): after a delete, the slot screen
    // goes through the game's "access the save folder" screens to read the slots again.
    int32_t OverrideExit(uint32_t node, int32_t exit);

    // Controller 0 as the game is about to see it (kernel/xam_input_imports.cpp, ReadController,
    // `poll` = the game's own read): the screens read their input from it, and then hide it from
    // the game while the keyboard or the delete question is up (the keyboard lets B through: the
    // game's BACK), and until every button is released after they close.
    void FilterGameInput(XINPUT_GAMEPAD& pad, bool poll);

    // The game's menus must not follow the mouse (the delete question is up). menu_mouse.cpp.
    bool BlocksMenuMouse();

    // Render thread, once per presented frame, over the back buffer (RENDER_TARGET state).
    void Draw(uint32_t width, uint32_t height);
}
