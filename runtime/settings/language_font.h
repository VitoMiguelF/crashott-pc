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

    // Decodes a PNG (any kind WIC reads) to straight-alpha RGBA8; false on failure.
    bool DecodePng(const uint8_t* data, size_t size, int& width, int& height, std::vector<uint8_t>& rgba);
}
