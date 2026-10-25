#include "text_canvas.h"

// FreeType renders, fontconfig picks the font: "sans-serif" (semibold when asked) for Segoe UI and
// "monospace" for Consolas, so the distribution's own fonts are used (Noto Sans / DejaVu Sans cover
// every accent of the subtitles). Built without them (CRASH_HAVE_FREETYPE unset, as in the zig
// cross-build), every call fails and the console / subtitles are not drawn.
#if CRASH_HAVE_FREETYPE
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace plat
{
    namespace
    {
        struct Glyph
        {
            int left = 0, top = 0, width = 0, height = 0, advance = 0;
            std::vector<uint8_t> coverage;
        };

        // One face at one size, with its rendered glyphs. Shared by every canvas (the subtitles make a
        // new canvas per line) and kept for the run: a few fonts at a few sizes.
        struct Font
        {
            FT_Face face = nullptr;
            int ascent = 0, height = 0, averageWidth = 1;
            std::unordered_map<uint32_t, Glyph> glyphs;
            std::mutex mutex;

            const Glyph& Get(uint32_t c)
            {
                auto it = glyphs.find(c);
                if (it != glyphs.end())
                    return it->second;
                Glyph& glyph = glyphs[c];
                FT_UInt index = FT_Get_Char_Index(face, c);
                if (index && FT_Load_Glyph(face, index, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0)
                {
                    FT_GlyphSlot slot = face->glyph;
                    const FT_Bitmap& bitmap = slot->bitmap;
                    glyph.left = slot->bitmap_left;
                    glyph.top = slot->bitmap_top;
                    glyph.width = int(bitmap.width);
                    glyph.height = int(bitmap.rows);
                    glyph.advance = int((slot->advance.x + 32) >> 6);
                    glyph.coverage.resize(size_t(glyph.width) * glyph.height);
                    for (int y = 0; y < glyph.height; y++)
                        memcpy(&glyph.coverage[size_t(y) * glyph.width], bitmap.buffer + ptrdiff_t(y) * bitmap.pitch, size_t(glyph.width));
                }
                return glyph;
            }
        };

        std::mutex g_mutex;
        FT_Library g_library = nullptr;
        std::map<std::tuple<int, int, bool>, std::unique_ptr<Font>> g_fonts;
        std::map<std::pair<int, bool>, std::string> g_files;

        // The font file fontconfig picks for a face (empty if none).
        std::string FontFile(FontFace face, bool semibold)
        {
            auto key = std::make_pair(int(face), semibold);
            if (auto it = g_files.find(key); it != g_files.end())
                return it->second;
            std::string file;
            if (FcInit())
            {
                FcPattern* pattern = FcPatternCreate();
                FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(face == FontFace::Monospace ? "monospace" : "sans-serif"));
                FcPatternAddInteger(pattern, FC_WEIGHT, semibold ? FC_WEIGHT_SEMIBOLD : FC_WEIGHT_REGULAR);
                FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
                FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
                FcDefaultSubstitute(pattern);
                FcResult result;
                if (FcPattern* match = FcFontMatch(nullptr, pattern, &result))
                {
                    FcChar8* path = nullptr;
                    if (FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch && path)
                        file = reinterpret_cast<const char*>(path);
                    FcPatternDestroy(match);
                }
                FcPatternDestroy(pattern);
            }
            g_files[key] = file;
            return file;
        }

        Font* GetFont(FontFace face, int pixelHeight, bool semibold)
        {
            std::lock_guard lock(g_mutex);
            auto key = std::make_tuple(int(face), pixelHeight, semibold);
            if (auto it = g_fonts.find(key); it != g_fonts.end())
                return it->second.get();
            if (!g_library && FT_Init_FreeType(&g_library) != 0)
                return nullptr;
            std::string file = FontFile(face, semibold);
            auto font = std::make_unique<Font>();
            if (file.empty() || FT_New_Face(g_library, file.c_str(), 0, &font->face) != 0)
                return nullptr;
            // GDI's negative height: the character (em) height, without the internal leading.
            FT_Set_Pixel_Sizes(font->face, 0, FT_UInt(pixelHeight));
            const FT_Size_Metrics& metrics = font->face->size->metrics;
            font->ascent = int((metrics.ascender + 63) >> 6);
            font->height = font->ascent + int((-metrics.descender + 63) >> 6);
            font->averageWidth = std::max(font->Get('x').advance, 1);  // GDI's tmAveCharWidth is about an 'x'
            Font* result = font.get();
            g_fonts[key] = std::move(font);
            return result;
        }

        int LineWidth(Font& font, const std::wstring& line)
        {
            int width = 0;
            for (wchar_t c : line)
                width += font.Get(uint32_t(c)).advance;
            return width;
        }

        // DrawText's DT_WORDBREAK: lines broken at spaces to fit `maxWidth` (a longer word stays whole),
        // and at line breaks.
        std::vector<std::wstring> Wrap(Font& font, const std::wstring& text, int maxWidth)
        {
            std::vector<std::wstring> lines;
            size_t start = 0;
            while (start <= text.size())
            {
                size_t end = text.find(L'\n', start);
                std::wstring paragraph = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
                if (!paragraph.empty() && paragraph.back() == L'\r')
                    paragraph.pop_back();
                std::wstring line;
                size_t at = 0;
                while (at < paragraph.size())
                {
                    size_t space = paragraph.find(L' ', at);
                    std::wstring word = paragraph.substr(at, space == std::wstring::npos ? std::wstring::npos : space - at);
                    std::wstring candidate = line.empty() ? word : line + L' ' + word;
                    if (!line.empty() && LineWidth(font, candidate) > maxWidth)
                    {
                        lines.push_back(line);
                        line = word;
                    }
                    else
                        line = candidate;
                    if (space == std::wstring::npos)
                        break;
                    at = space + 1;
                }
                lines.push_back(line);
                if (end == std::wstring::npos)
                    break;
                start = end + 1;
            }
            return lines;
        }
    }

    struct TextCanvas::Impl
    {
        Font* font = nullptr;
        std::vector<uint32_t> bits;

        // White text over what is there (the coverage, max), clipped to the bitmap and to [clipTop, clipBottom).
        void DrawLine(int width, int height, int x, int y, const std::wstring& text, int clipTop, int clipBottom)
        {
            std::lock_guard lock(font->mutex);
            int baseline = y + font->ascent;
            for (wchar_t c : text)
            {
                const Glyph& glyph = font->Get(uint32_t(c));
                for (int gy = 0; gy < glyph.height; gy++)
                {
                    int py = baseline - glyph.top + gy;
                    if (py < std::max(clipTop, 0) || py >= std::min(clipBottom, height))
                        continue;
                    for (int gx = 0; gx < glyph.width; gx++)
                    {
                        int px = x + glyph.left + gx;
                        if (px < 0 || px >= width)
                            continue;
                        uint32_t value = glyph.coverage[size_t(gy) * glyph.width + gx];
                        uint32_t& pixel = bits[size_t(py) * width + px];
                        if (value > (pixel & 0xFF))
                            pixel = value << 16 | value << 8 | value;
                    }
                }
                x += glyph.advance;
            }
        }
    };

    TextCanvas::TextCanvas() : m_impl(new Impl) {}

    TextCanvas::~TextCanvas()
    {
        delete m_impl;
    }

    bool TextCanvas::SetFont(FontFace face, int pixelHeight, bool semibold)
    {
        Font* font = GetFont(face, std::max(pixelHeight, 1), semibold);
        if (!font)
            return false;
        m_impl->font = font;
        return true;
    }

    void TextCanvas::Metrics(int& charWidth, int& lineHeight) const
    {
        charWidth = m_impl->font ? m_impl->font->averageWidth : 1;
        lineHeight = m_impl->font ? m_impl->font->height : 1;
    }

    bool TextCanvas::Resize(int width, int height)
    {
        if (width <= 0 || height <= 0)
            return false;
        m_impl->bits.assign(size_t(width) * height, 0);
        m_width = width;
        m_height = height;
        return true;
    }

    void TextCanvas::Clear()
    {
        std::fill(m_impl->bits.begin(), m_impl->bits.end(), 0u);
    }

    void TextCanvas::Draw(int x, int y, const std::wstring& text)
    {
        if (m_impl->font && !m_impl->bits.empty())
            m_impl->DrawLine(m_width, m_height, x, y, text, 0, m_height);
    }

    void TextCanvas::MeasureWrapped(const std::wstring& text, int maxWidth, int& width, int& height)
    {
        width = height = 0;
        if (!m_impl->font)
            return;
        std::lock_guard lock(m_impl->font->mutex);
        std::vector<std::wstring> lines = Wrap(*m_impl->font, text, maxWidth);
        for (const std::wstring& line : lines)
            width = std::max(width, LineWidth(*m_impl->font, line));
        width = std::min(width, maxWidth);
        height = int(lines.size()) * m_impl->font->height;
    }

    void TextCanvas::DrawWrapped(const std::wstring& text, int x, int y, int width, int height)
    {
        if (!m_impl->font || m_impl->bits.empty())
            return;
        std::vector<std::wstring> lines;
        std::vector<int> widths;
        {
            std::lock_guard lock(m_impl->font->mutex);
            lines = Wrap(*m_impl->font, text, width);
            for (const std::wstring& line : lines)
                widths.push_back(LineWidth(*m_impl->font, line));
        }
        int lineY = y;
        for (size_t i = 0; i < lines.size() && lineY < y + height; i++, lineY += m_impl->font->height)
            m_impl->DrawLine(m_width, m_height, x + (width - widths[i]) / 2, lineY, lines[i], y, y + height);  // DT_CENTER
    }

    void TextCanvas::Flush() {}

    uint32_t* TextCanvas::Pixels()
    {
        return m_impl->bits.empty() ? nullptr : m_impl->bits.data();
    }
}
#else
namespace plat
{
    struct TextCanvas::Impl
    {
    };

    TextCanvas::TextCanvas() : m_impl(nullptr) {}
    TextCanvas::~TextCanvas() {}
    bool TextCanvas::SetFont(FontFace, int, bool) { return false; }

    void TextCanvas::Metrics(int& charWidth, int& lineHeight) const
    {
        charWidth = 1;
        lineHeight = 1;
    }

    bool TextCanvas::Resize(int, int) { return false; }
    void TextCanvas::Clear() {}
    void TextCanvas::Draw(int, int, const std::wstring&) {}

    void TextCanvas::MeasureWrapped(const std::wstring&, int, int& width, int& height)
    {
        width = height = 0;
    }

    void TextCanvas::DrawWrapped(const std::wstring&, int, int, int, int) {}
    void TextCanvas::Flush() {}
    uint32_t* TextCanvas::Pixels() { return nullptr; }
}
#endif
