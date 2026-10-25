#include "text_canvas.h"

// TODO(linux): FreeType (or stb_truetype) with a bundled font (DejaVu Sans / Sans Mono cover the
// subtitles' accents). Until then every call fails and the console / subtitles are not drawn.
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
