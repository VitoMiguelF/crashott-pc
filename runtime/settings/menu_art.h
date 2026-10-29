#pragma once

// The game's own art for the settings screen (settings_menu.cpp), read at startup from the user's
// default.rcf (never copied anywhere) and drawn on the CPU at the window's resolution:
//
//   package\b4c85fe7.p3d (Fe_Persistent_fe)  fonts Titans_Small (the text of the save messages,
//                                            "... Don't turn off your Xbox 360 console.") and
//                                            Titans_Large (NEW GAME / LOAD GAME), with the
//                                            button icons (A B X Y BACK START RT RB stick < >
//                                            rotate LT LB, codes 0xB1-0xBE); MSG_Background
//   package\c1e387c7.p3d (Fe_InGame_fe)      InGame_Pause_Background (the swirls at the sides of
//                                            the pause menu), FE_star_open
//   package\cdd70a8c.p3d (Fe_Frontend_fe)    FE_Name_Divider_Left / _Right; FE_Background_Weave / _Fold /
//                                            _Foliage_Top / _Foliage_Bottom / _Light_Map and FE_Title_Logo
//                                            (the main menu's backdrop and the logo, for the launcher)
//
// A mod's copy of one of these entries (mods\<mod>\rcf\default\<entry>) is used instead of the
// original. The fonts lack ã õ Ã Õ Ê Ô (empty or missing glyphs); they are composed as the PT-BR
// language pack does (language_font.h). Loading runs on a thread started by StartLoading.
//
// Fonts: each glyph is a cell of fixed height (the line), white with an outline at ~41% alpha;
// the game tints the fill and the outline (orange on dark brown in the pause menu, cyan on teal
// for the selected item). DrawGameText does the same: alpha 0.41 -> outline colour, 1.0 -> fill.

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace settings::menu_art
{
    // Premultiplied RGBA8, R in the low byte.
    struct Image
    {
        int w = 0, h = 0;
        std::vector<uint32_t> px;
    };

    // Straight-alpha colour.
    struct Color
    {
        uint8_t r, g, b, a;
    };

    enum class FontId { Small, Large };
    // The last six are the frontend's backdrop (the yellow cloth of the main menu) and the title
    // logo, used by the launcher (launcher/); the settings screen uses the first five.
    enum class Art { StarFull, PauseStrip, MessageFrame, DividerLeft, DividerRight,
                     BackdropWeave, BackdropFold, FoliageTop, FoliageBottom, LightMap, TitleLogo, Count };

    // Button icons in the fonts.
    constexpr char32_t ICON_A = 0xB1, ICON_B = 0xB2, ICON_X = 0xB3, ICON_Y = 0xB4, ICON_BACK = 0xB5, ICON_START = 0xB6,
                       ICON_RT = 0xB7, ICON_RB = 0xB8, ICON_STICK = 0xB9, ICON_LEFT = 0xBA, ICON_RIGHT = 0xBB,
                       ICON_ROTATE = 0xBC, ICON_LT = 0xBD, ICON_LB = 0xBE;

    // Starts reading the art (after vfs::Init) on a thread.
    void StartLoading();
    // Waits up to `waitMs` for the loading to finish; true if the fonts are there.
    bool Ready(int waitMs);
    const Image* GetArt(Art art);  // nullptr if missing

    struct TextStyle
    {
        Color fill, outline;
        float opacity = 1.0f;
    };

    // CPU canvas the screen is drawn into (premultiplied).
    class Canvas
    {
    public:
        void Reset(int width, int height);
        int Width() const { return w_; }
        int Height() const { return h_; }
        void Fill(int x0, int y0, int x1, int y1, Color color);
        // Vertical gradient from `top` to `bottom`.
        void FillGradient(int x0, int y0, int x1, int y1, Color top, Color bottom);
        // `image` (already at its final size) with its top-left at (x, y), its colour multiplied by
        // `tint`, drawn up to column `clipX1`.
        void Blit(const Image& image, int x, int y, Color tint = { 255, 255, 255, 255 }, int clipX1 = 1 << 30);
        // Light-only blit: the image's luminance times its alpha becomes the alpha of `tint`
        // (the game draws the pause-menu swirls additively, so their black parts vanish).
        void BlitGlow(const Image& image, int x, int y, Color tint);
        // A text glyph (alpha only): outline colour at the outline's alpha, fill colour inside.
        void BlitGlyph(const Image& glyph, int x, int y, const TextStyle& style);
        // Straight-alpha copy for the presenter (gpu::overlay).
        void Export(std::vector<uint32_t>& out) const;

    private:
        int w_ = 0, h_ = 0;
        std::vector<uint32_t> px_;
    };

    // `source` rectangle (sx, sy, sw, sh) of `image` resampled to dw x dh (area average when
    // shrinking, bilinear when growing), optionally mirrored; cached until ClearCache.
    const Image& Scaled(const Image& image, int sx, int sy, int sw, int sh, int dw, int dh, bool flipX = false, bool flipY = false);
    const Image& Scaled(const Image& image, int dw, int dh, bool flipX = false, bool flipY = false);
    void ClearCache();

    // Text in a font: `cell` is the line height in pixels (the glyph cells), (x, y) its top-left.
    // Button icons (ICON_*) inside the text are drawn in colour, `iconScale` x the cell high.
    float TextWidth(FontId font, const std::u32string& text, float cell, float iconScale = 1.0f);
    float DrawGameText(Canvas& canvas, FontId font, const std::u32string& text, float x, float y, float cell, const TextStyle& style,
                   float iconScale = 1.0f);
    // One icon `height` pixels high with its top-left at (x, y), optionally mirrored; returns its width.
    float DrawIcon(Canvas& canvas, char32_t icon, float x, float y, float height, float opacity = 1.0f, bool flipX = false);
    float IconWidth(char32_t icon, float height);
    // Whether the font draws `c` (not empty).
    bool HasGlyph(FontId font, char32_t c);
    // `text` in at most `maxWidth`: the cell shrinks down to `minScale` x, then the end is cut with
    // "...". Returns what to draw; `cell` becomes the size to draw it at.
    std::u32string FitText(FontId font, const std::u32string& text, float& cell, float maxWidth, float minScale = 0.8f);

    // The game's message box (the settings' dialogs, the save screens' question, the MULTIPLAYER
    // screens): MSG_Background, its top-left corner (curl, border and black inside), mirrored into the
    // four corners of (left, top)-(right, bottom) in canvas pixels. The curl keeps its aspect, so it
    // grows with the box's height; the straight part is stretched. `fallback` fills the box without the art.
    void DrawMessageFrame(Canvas& canvas, int left, int top, int right, int bottom, Color tint, Color fallback);
    // Where text goes in a message box (x0, y0)-(x1, y1), in any unit, measured on MSG_Background (a
    // mod's frame is followed too): the border's depth at the top (and the bottom); how far from the
    // left (and the right) side a line spanning lineTop-lineBottom must start to clear the border and
    // the curls at those heights (the curls reach far in, over most of the height); the outer size of
    // the box that holds contentW x contentH clear of both, `gap` more on every side.
    float FrameTopInset(float y0, float y1);
    float FrameSideInset(float x0, float y0, float x1, float y1, float lineTop, float lineBottom);
    void FrameOuterSize(float contentW, float contentH, float gap, float& outerW, float& outerH);

    std::u32string Utf32(const std::string& utf8);
}
