#include "text_canvas.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace plat
{
    struct TextCanvas::Impl
    {
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HFONT font = nullptr;
        uint32_t* bits = nullptr;
    };

    TextCanvas::TextCanvas() : m_impl(new Impl)
    {
        m_impl->dc = CreateCompatibleDC(nullptr);
        SetBkMode(m_impl->dc, TRANSPARENT);
        SetTextColor(m_impl->dc, RGB(255, 255, 255));
    }

    TextCanvas::~TextCanvas()
    {
        if (m_impl->dc)
            DeleteDC(m_impl->dc);
        if (m_impl->bitmap)
            DeleteObject(m_impl->bitmap);
        if (m_impl->font)
            DeleteObject(m_impl->font);
        delete m_impl;
    }

    bool TextCanvas::SetFont(FontFace face, int pixelHeight, bool semibold)
    {
        HFONT font = face == FontFace::Monospace
                         ? CreateFontW(-pixelHeight, 0, 0, 0, semibold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas")
                         : CreateFontW(-pixelHeight, 0, 0, 0, face == FontFace::UiBold ? FW_HEAVY : semibold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        if (!font || !m_impl->dc)
            return false;
        SelectObject(m_impl->dc, font);
        if (m_impl->font)
            DeleteObject(m_impl->font);
        m_impl->font = font;
        return true;
    }

    void TextCanvas::Metrics(int& charWidth, int& lineHeight) const
    {
        TEXTMETRICW metrics{};
        GetTextMetricsW(m_impl->dc, &metrics);
        charWidth = std::max<int>(metrics.tmAveCharWidth, 1);
        lineHeight = metrics.tmHeight;
    }

    bool TextCanvas::Resize(int width, int height)
    {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;  // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(m_impl->dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap || !bits)
        {
            if (bitmap)
                DeleteObject(bitmap);
            return false;
        }
        SelectObject(m_impl->dc, bitmap);
        if (m_impl->bitmap)
            DeleteObject(m_impl->bitmap);
        m_impl->bitmap = bitmap;
        m_impl->bits = static_cast<uint32_t*>(bits);
        m_width = width;
        m_height = height;
        Clear();
        return true;
    }

    void TextCanvas::Clear()
    {
        if (m_impl->bits)
            memset(m_impl->bits, 0, size_t(m_width) * m_height * 4);
    }

    void TextCanvas::Draw(int x, int y, const std::wstring& text)
    {
        TextOutW(m_impl->dc, x, y, text.c_str(), int(text.size()));
    }

    int TextCanvas::TextWidth(const std::wstring& text) const
    {
        SIZE size{};
        if (!text.empty())
            GetTextExtentPoint32W(m_impl->dc, text.c_str(), int(text.size()), &size);
        return int(size.cx);
    }

    void TextCanvas::MeasureWrapped(const std::wstring& text, int maxWidth, int& width, int& height)
    {
        RECT measure{ 0, 0, maxWidth, 0 };
        DrawTextW(m_impl->dc, text.c_str(), int(text.size()), &measure, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        width = std::min<int>(measure.right - measure.left, maxWidth);
        height = measure.bottom - measure.top;
    }

    void TextCanvas::DrawWrapped(const std::wstring& text, int x, int y, int width, int height)
    {
        RECT box{ x, y, x + width, y + height };
        DrawTextW(m_impl->dc, text.c_str(), int(text.size()), &box, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    }

    void TextCanvas::Flush()
    {
        GdiFlush();
    }

    uint32_t* TextCanvas::Pixels()
    {
        return m_impl->bits;
    }
}
