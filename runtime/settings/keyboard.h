#pragma once

// The on-screen keyboard of the name entry (NEW GAME, save_screens.cpp) and of the MULTIPLAYER fields (the
// code, a password, the player's name: online/mp_menu.cpp): the digits and QWERTY rows on rounded plates in
// the frontend's colours, ERASE and DONE below; the D-pad goes from key to key, A presses the highlighted one.
// Positions are in the virtual 1280x720 screen, the keys centred on x 640 from the keyboard's top.

#include "menu_art.h"

#include <functional>
#include <vector>

namespace settings::keyboard
{
    constexpr char32_t ERASE = 1, DONE = 2;  // the wide keys' "characters"
    constexpr float KEY_W = 50.0f, KEY_H = 46.0f, PITCH_X = 56.0f, PITCH_Y = 52.0f;
    constexpr float HEIGHT = 4 * PITCH_Y + KEY_H;  // the five rows

    struct Key
    {
        char32_t c;  // the character (A-Z, 0-9), or ERASE / DONE
        int row;
        float x, y, w, h;  // y from the keyboard's top
    };

    const std::vector<Key>& Keys();
    int KeyOf(char32_t c);  // -1 if not a key
    // Up / down: the key of that row nearest in x; left / right: around the row.
    int Step(int key, int dx, int dy);

    // The keys from `top` (virtual units; `scale` / `ox` / `oy` map them onto the canvas), `selected`
    // highlighted. `enabled(c)`: the key does something now (else dimmed). `zone(index, x0, y0, x1, y1)`
    // gets each key's rectangle (virtual units) for the mouse and touch.
    void Draw(menu_art::Canvas& canvas, float scale, float ox, float oy, float top, int selected, bool portuguese,
              const std::function<bool(char32_t)>& enabled, const std::function<void(int, float, float, float, float)>& zone);
}
