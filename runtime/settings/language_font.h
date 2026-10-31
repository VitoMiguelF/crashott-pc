#pragma once

// Glyphs a language pack needs that the game's fonts lack (language_pack.h). The X360 fonts
// (Titans_Large/Small in package\b4c85fe7.p3d) have ã Ã Ê Ô as empty glyphs and no õ Õ at all, so
// these are composed from glyphs the font does have: a base letter plus the accent of another one,
// e.g. "ã = a + ñ - n" takes the rows of ñ above the top of n (the tilde) and centers them over a.
// The composed glyphs go to one new texture page of the font (a copy of one of its pages with the
// same palette), and their glyph records point there. A font that already has 32 pages (the most
// the engine takes; Titans_Large) gets them in the cells of the recipes' donors instead: glyphs
// no text of the language uses (Å å Ò ò È Ö), redrawn on their pages.
//
// Pure3D fonts (little-endian chunks, tools/texts.py):
//   0x22000 font: u32 version, name, f32 x4 (size, width, height, baseline), u32 texture count;
//                 children: the texture pages (0x19000), the glyph list (0x22001) and others
//   0x19000 texture: name, u32 version, width, height, ...; child 0x19001 image (name, version,
//                 width, height, bpp, palettized, alpha, format) with child 0x19002 (u32 size, PNG)
//   0x22001 glyphs: u32 count, then per glyph (sorted by code) u32 texture, f32 u0, v0, u1, v1
//                 (bottom-left, top-right; v grows upwards), f32 left bearing, right bearing,
//                 width, advance (pixels), u32 code; an empty glyph has u0 == u1

#include <cstdint>
#include <string>
#include <vector>

namespace settings::language_font
{
    struct Recipe
    {
        char16_t target, base, accent, accentBase;
        char16_t donor;  // fonts at the page limit: the glyph whose cell the new one takes (0 = none)
    };

    // "ã = a + ñ - n @ å" -> recipe ("@ donor" optional); false if malformed.
    bool ParseRecipe(const std::u16string& text, Recipe& recipe);

    // Appends font chunk [data, data + size) to `out` with the glyphs of `recipes` the font lacks
    // (missing or empty) composed; returns the number of glyphs added (0: the chunk is copied as is).
    int ComposeFont(const uint8_t* data, size_t size, const std::vector<Recipe>& recipes, std::vector<uint8_t>& out,
                    std::string& log);

    // A button icon WidenIcons made wider: its new texture's palette indices (w x h, top row first).
    struct WidenedIcon
    {
        char32_t icon;
        int w, h;
        std::vector<uint8_t> indices;
    };

    // The icons `codes` of font chunk [data, data + size) whose texture is taller than wide (RT /
    // LT in Titans_Large: 64x128) get a texture `aspect` times as wide as high, the old one centred
    // in it, and a glyph that wide (in the units of the square icon 0xB1), so the PlayStation and
    // keyboard icons drawn in their place have room (prompt_icons.cpp); the game's own icon looks
    // the same, with space around it. Appends the font to `out`; returns the icons widened.
    int WidenIcons(const uint8_t* data, size_t size, const std::vector<char32_t>& codes, float aspect, std::vector<uint8_t>& out,
                   std::vector<WidenedIcon>& widened, std::string& log);

    // Decodes a PNG to straight-alpha RGBA8 (plat::DecodePng); false on failure.
    bool DecodePng(const uint8_t* data, size_t size, int& width, int& height, std::vector<uint8_t>& rgba);
}
