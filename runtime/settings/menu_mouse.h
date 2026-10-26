#pragma once

// The frontend's menus (Scrooby pages: main menu, save slots, difficulty, pause...) with the mouse
// (menu_mouse.cpp). The game updates the menu on screen once per frame from its frontend tree's
// menu action (CMenuAction::vf05, sub_820B2E98; this+24 = the pure3d::frontend::Menu); a hook
// there keeps the live menu: its selected index (byte +152) and its items (vector at +112/+116;
// each TextMenuItem's text at +140 has its position at +92 / +96, in page units / 640, y up).
// Widths and heights come from the pages' text chunks (0x18023: x, y, w, h on a 640x480 page,
// y up), read from the frontend packages when the game opens default.rcf.
//
// The same hook acts on the mouse, like a PC game: when the cursor moves onto an item, that item
// is selected at once with the game's own Menu::SetSelection (sub_822F2E40: the highlight, the
// cursor sprite) and its move sound plays once; a left click on an item selects it and presses A
// (kernel/pc_input.cpp, PressMenuAccept); the wheel steps with the game's previous / next
// (sub_822F2F58 / sub_822F3068, which skip disabled items). Nothing happens while the mouse is
// still, so the keyboard and the pads keep the selection they set.
//
// A YES / NO question over a menu (CConfirmMessage, "SAVE AND EXIT?" in the pause) has a menu of
// its own, updated by CConfirmMessage::vf03 (sub_821D5858, this+16): while it is shown, a hook
// there makes it the live menu, and the menu behind it gets no mouse.

#include <cstdint>
#include <vector>

namespace input::menu_mouse
{
    // Registers the read-only readers of the frontend packages (before the game opens default.rcf).
    void Init();

    struct Item
    {
        float x0, y0, x1, y1;  // page units: 640 x 480, y up
        bool enabled = true;   // can be selected (item +137 bit 0x80, as the game's previous / next see it)
    };
    struct Menu
    {
        uint32_t id = 0;   // guest address of the menu
        int index = -1;    // selected item
        std::vector<Item> items;
        bool vertical = true;  // items stacked (UP/DOWN) rather than side by side (LEFT/RIGHT)
    };
    // The menu the game updated last, if it did so in the last 150 ms.
    bool Live(Menu& menu);
    // The item under page point (x, y) (y up), or -1.
    int HitTest(const Menu& menu, float x, float y);
}
