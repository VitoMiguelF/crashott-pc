#include "text_canvas.h"

// FreeType renders, fontconfig picks the font: "sans-serif" (semibold when asked) for Segoe UI and
// "monospace" for Consolas, so the distribution's own fonts are used (Noto Sans / DejaVu Sans cover
// every accent of the subtitles). Both are loaded at run time (libfreetype.so.6, libfontconfig.so.1),
// like Vulkan and FFmpeg: a Steam Deck has the libraries (its desktop uses them) but no headers, so
// the build needs only FreeType's headers (thirdparty/freetype, setup.py) and fontconfig's few calls
// are declared here. Without the headers (CRASH_HAVE_FREETYPE unset) or the libraries, every call
// fails and the console / subtitles are not drawn.
//
// Android has neither: stb_truetype (Dear ImGui's copy) renders the system's own fonts there
// (Roboto, Droid Sans Mono), the same glyph cache and drawing.
#if CRASH_HAVE_FREETYPE || defined(__ANDROID__)
#include "platform.h"
#include "../diagnostics.h"

#if CRASH_HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#else
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../thirdparty/imgui/imstb_truetype.h"
#include <cmath>
#include <fstream>
#endif

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace plat
{
    namespace
    {
        struct Glyph
        {
            int left = 0, top = 0, width = 0, height = 0, advance = 0;  // top: above the baseline
            std::vector<uint8_t> coverage;
        };

#if CRASH_HAVE_FREETYPE
        // fontconfig's ABI (fontconfig.h): opaque patterns, and the few constants used here.
        struct FcPattern;
        using FcChar8 = unsigned char;
        using FcBool = int;
        constexpr int FcMatchPattern = 0, FcResultMatch = 0, FC_WEIGHT_REGULAR = 80, FC_WEIGHT_SEMIBOLD = 180, FC_WEIGHT_BLACK = 210;

        struct Api
        {
            FT_Error (*FT_Init_FreeType)(FT_Library*);
            FT_Error (*FT_New_Face)(FT_Library, const char*, FT_Long, FT_Face*);
            FT_Error (*FT_Set_Pixel_Sizes)(FT_Face, FT_UInt, FT_UInt);
            FT_UInt (*FT_Get_Char_Index)(FT_Face, FT_ULong);
            FT_Error (*FT_Load_Glyph)(FT_Face, FT_UInt, FT_Int32);
            FcBool (*FcInit)();
            FcPattern* (*FcPatternCreate)();
            FcBool (*FcPatternAddString)(FcPattern*, const char*, const FcChar8*);
            FcBool (*FcPatternAddInteger)(FcPattern*, const char*, int);
            FcBool (*FcPatternAddBool)(FcPattern*, const char*, FcBool);
            FcBool (*FcConfigSubstitute)(void*, FcPattern*, int);
            void (*FcDefaultSubstitute)(FcPattern*);
            FcPattern* (*FcFontMatch)(void*, FcPattern*, int*);
            int (*FcPatternGetString)(const FcPattern*, const char*, int, FcChar8**);
            void (*FcPatternDestroy)(FcPattern*);
        };

        template <typename T>
        bool Resolve(void* library, const char* name, T& function)
        {
            function = reinterpret_cast<T>(library ? FindSymbol(library, name) : nullptr);
            return function != nullptr;
        }

        // Loads both libraries once; null (and one log line) when either is missing.
        const Api* LoadApi()
        {
            static Api api;
            static const Api* loaded = [] () -> const Api* {
                void* ft = OpenLibrary("libfreetype.so.6");
                void* fc = OpenLibrary("libfontconfig.so.1");
                bool ok = Resolve(ft, "FT_Init_FreeType", api.FT_Init_FreeType) && Resolve(ft, "FT_New_Face", api.FT_New_Face) &&
                          Resolve(ft, "FT_Set_Pixel_Sizes", api.FT_Set_Pixel_Sizes) &&
                          Resolve(ft, "FT_Get_Char_Index", api.FT_Get_Char_Index) && Resolve(ft, "FT_Load_Glyph", api.FT_Load_Glyph) &&
                          Resolve(fc, "FcInit", api.FcInit) && Resolve(fc, "FcPatternCreate", api.FcPatternCreate) &&
                          Resolve(fc, "FcPatternAddString", api.FcPatternAddString) &&
                          Resolve(fc, "FcPatternAddInteger", api.FcPatternAddInteger) &&
                          Resolve(fc, "FcPatternAddBool", api.FcPatternAddBool) &&
                          Resolve(fc, "FcConfigSubstitute", api.FcConfigSubstitute) &&
                          Resolve(fc, "FcDefaultSubstitute", api.FcDefaultSubstitute) && Resolve(fc, "FcFontMatch", api.FcFontMatch) &&
                          Resolve(fc, "FcPatternGetString", api.FcPatternGetString) && Resolve(fc, "FcPatternDestroy", api.FcPatternDestroy);
                if (!ok)
                    Log("[text] libfreetype.so.6 / libfontconfig.so.1 not usable (%s): no subtitles or console text",
                        LastErrorText().c_str());
                return ok ? &api : nullptr;
            }();
            return loaded;
        }

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
                const Api& ft = *LoadApi();  // a Font exists only once the API loaded
                FT_UInt index = ft.FT_Get_Char_Index(face, c);
                if (index && ft.FT_Load_Glyph(face, index, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0)
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
            const Api& fc = *LoadApi();
            if (fc.FcInit())
            {
                FcPattern* pattern = fc.FcPatternCreate();
                fc.FcPatternAddString(pattern, "family", reinterpret_cast<const FcChar8*>(face == FontFace::Monospace ? "monospace" : "sans-serif"));
                fc.FcPatternAddInteger(pattern, "weight", face == FontFace::UiBold ? FC_WEIGHT_BLACK : semibold ? FC_WEIGHT_SEMIBOLD : FC_WEIGHT_REGULAR);
                fc.FcPatternAddBool(pattern, "scalable", 1);
                fc.FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
                fc.FcDefaultSubstitute(pattern);
                int result;
                if (FcPattern* match = fc.FcFontMatch(nullptr, pattern, &result))
                {
                    FcChar8* path = nullptr;
                    if (fc.FcPatternGetString(match, "file", 0, &path) == FcResultMatch && path)
                        file = reinterpret_cast<const char*>(path);
                    fc.FcPatternDestroy(match);
                }
                fc.FcPatternDestroy(pattern);
                Log("[text] %s%s: %s", face == FontFace::Monospace ? "monospace" : "sans-serif", semibold ? " semibold" : "",
                    file.empty() ? "no font found" : file.c_str());
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
            const Api* ft = LoadApi();
            if (!ft || (!g_library && ft->FT_Init_FreeType(&g_library) != 0))
                return nullptr;
            std::string file = FontFile(face, semibold);
            auto font = std::make_unique<Font>();
            if (file.empty() || ft->FT_New_Face(g_library, file.c_str(), 0, &font->face) != 0)
                return nullptr;
            // GDI's negative height: the character (em) height, without the internal leading.
            ft->FT_Set_Pixel_Sizes(font->face, 0, FT_UInt(pixelHeight));
            const FT_Size_Metrics& metrics = font->face->size->metrics;
            font->ascent = int((metrics.ascender + 63) >> 6);
            font->height = font->ascent + int((-metrics.descender + 63) >> 6);
            font->averageWidth = std::max(font->Get('x').advance, 1);  // GDI's tmAveCharWidth is about an 'x'
            Font* result = font.get();
            g_fonts[key] = std::move(font);
            return result;
        }
#else
        // One face at one size, with its rendered glyphs (as FreeType's above).
        struct Font
        {
            std::vector<unsigned char> data;
            stbtt_fontinfo info{};
            float scale = 1.0f;
            int ascent = 0, height = 0, averageWidth = 1;
            std::unordered_map<uint32_t, Glyph> glyphs;
            std::mutex mutex;

            const Glyph& Get(uint32_t c)
            {
                auto it = glyphs.find(c);
                if (it != glyphs.end())
                    return it->second;
                Glyph& glyph = glyphs[c];
                int index = stbtt_FindGlyphIndex(&info, int(c));
                if (!index)
                    return glyph;
                int advance = 0, bearing = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                stbtt_GetGlyphHMetrics(&info, index, &advance, &bearing);
                stbtt_GetGlyphBitmapBox(&info, index, scale, scale, &x0, &y0, &x1, &y1);
                glyph.advance = int(std::lround(advance * scale));
                glyph.left = x0;
                glyph.top = -y0;
                glyph.width = std::max(x1 - x0, 0);
                glyph.height = std::max(y1 - y0, 0);
                glyph.coverage.resize(size_t(glyph.width) * glyph.height);
                if (!glyph.coverage.empty())
                    stbtt_MakeGlyphBitmap(&info, glyph.coverage.data(), glyph.width, glyph.height, glyph.width, scale, scale, index);
                return glyph;
            }
        };

        std::mutex g_mutex;
        std::map<std::tuple<int, int, bool>, std::unique_ptr<Font>> g_fonts;

        // Android's own fonts (Roboto: a variable font on newer versions, one file for every weight).
        std::string FontFile(FontFace face, bool semibold)
        {
            static const std::vector<const char*> regular = { "/system/fonts/Roboto-Regular.ttf", "/system/fonts/DroidSans.ttf" };
            static const std::vector<const char*> bold = { "/system/fonts/Roboto-Bold.ttf", "/system/fonts/Roboto-Medium.ttf",
                                                           "/system/fonts/Roboto-Regular.ttf", "/system/fonts/DroidSans.ttf" };
            static const std::vector<const char*> monospace = { "/system/fonts/DroidSansMono.ttf", "/system/fonts/CutiveMono.ttf",
                                                                "/system/fonts/Roboto-Regular.ttf" };
            for (const char* file : face == FontFace::Monospace ? monospace : semibold || face == FontFace::UiBold ? bold : regular)
                if (access(file, R_OK) == 0)
                    return file;
            return {};
        }

        Font* GetFont(FontFace face, int pixelHeight, bool semibold)
        {
            std::lock_guard lock(g_mutex);
            auto key = std::make_tuple(int(face), pixelHeight, semibold);
            if (auto it = g_fonts.find(key); it != g_fonts.end())
                return it->second.get();
            std::string file = FontFile(face, semibold);
            auto font = std::make_unique<Font>();
            std::ifstream in(file, std::ios::binary);
            font->data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (file.empty() || font->data.empty() || !stbtt_InitFont(&font->info, font->data.data(), stbtt_GetFontOffsetForIndex(font->data.data(), 0)))
            {
                Log("[text] %s: no usable font (%s): no subtitles or console text", face == FontFace::Monospace ? "monospace" : "sans-serif",
                    file.empty() ? "none found" : file.c_str());
                return nullptr;
            }
            // GDI's negative height: the character (em) height, without the internal leading.
            font->scale = stbtt_ScaleForMappingEmToPixels(&font->info, float(pixelHeight));
            int ascent = 0, descent = 0, gap = 0;
            stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &gap);
            font->ascent = int(std::ceil(ascent * font->scale));
            font->height = font->ascent + int(std::ceil(-descent * font->scale));
            font->averageWidth = std::max(font->Get('x').advance, 1);  // GDI's tmAveCharWidth is about an 'x'
            Font* result = font.get();
            g_fonts[key] = std::move(font);
            return result;
        }
#endif

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

    std::string SystemFontFile(FontFace face, bool bold)
    {
#if CRASH_HAVE_FREETYPE
        if (!LoadApi())
            return {};
#endif
        std::lock_guard lock(g_mutex);
        return FontFile(face, bold);
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

    int TextCanvas::TextWidth(const std::wstring& text) const
    {
        if (!m_impl->font)
            return 0;
        std::lock_guard lock(m_impl->font->mutex);
        return LineWidth(*m_impl->font, text);
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
#include <unistd.h>

namespace plat
{
    std::string SystemFontFile(FontFace, bool)
    {
        return {};
    }

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
    int TextCanvas::TextWidth(const std::wstring&) const { return 0; }

    void TextCanvas::MeasureWrapped(const std::wstring&, int, int& width, int& height)
    {
        width = height = 0;
    }

    void TextCanvas::DrawWrapped(const std::wstring&, int, int, int, int) {}
    void TextCanvas::Flush() {}
    uint32_t* TextCanvas::Pixels() { return nullptr; }
}
#endif
