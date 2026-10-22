#pragma once

// Button icons for the device in use (prompt_icons.cpp): the game's Xbox 360 icons (the font
// glyphs 0xB1-0xBE: A B X Y BACK START RT RB stick < > rotate LT LB, each its own small texture)
// swapped for PlayStation ones (Cross / Circle / Square / Triangle, L1 R1 L2 R2, Options, the
// touchpad) or for the keyboard keys and mouse buttons bound to that button. The replacements are
// drawn here (shapes + the game's own Titans font for the labels; nothing taken from other games).
//
// The renderer (gpu/d3d12_renderer.cpp) recognises the game's icon textures by their HD texture
// key (gpu/hd_textures.h), so the menus, the HUD prompts and the tutorial texts all change; the
// settings screen draws its icons through menu_art::DrawIcon, which asks here too.

#include <cstdint>
#include <filesystem>
#include <vector>

namespace settings::prompts
{
    // The icon (0xB1-0xBE) a game texture is, or 0. The icons are palettized: an 8-bit index
    // texture (k_8, bottom row first) read through a 256x1 palette by the game's pal*fp shaders
    // (palette.fxh: sampler s8). `key` is the HD texture key (gpu/hd_textures.h) of the index
    // texture as R8; `box` receives the icon's opaque area (x0, y0, x1, y1, top row first).
    char32_t IconOfTexture(uint64_t key, int box[4] = nullptr);

    // What the icons look like now: 0 = the game's own (Xbox), otherwise a value that changes
    // whenever the drawings change (device, menus vs gameplay, the bound keys). Cheap; any thread.
    uint32_t Style();

    // Draws `icon` for `style` at w x h, straight-alpha RGBA (R in the low byte), top row first,
    // placed over the opaque area of `original` (the game's icon at that size, same layout; may be
    // null: the whole area). False: keep the game's icon (e.g. the stick and the arrows stay).
    bool Draw(char32_t icon, uint32_t style, int w, int h, const uint32_t* original, std::vector<uint32_t>& out);

    // The same, as the game's icons are stored: `indices` (w x h, bottom row first) into
    // `palette` (256 colours, straight alpha, R in the low byte), drawn inside `box`.
    bool DrawPalettized(char32_t icon, uint32_t style, int w, int h, const int box[4], std::vector<uint8_t>& indices,
                        std::vector<uint32_t>& palette);

    // --prompt-preview <dir>: a sheet of the game's icons and ours (PlayStation, keyboard in
    // gameplay and in menus) at a few sizes, as PNG, without starting the game. Returns the exit code.
    int Preview(const std::filesystem::path& directory);
}
