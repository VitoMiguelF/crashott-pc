#pragma once

#include <cstdint>
#include <string>

// Text drawn by the OS into a CPU bitmap, for the overlays the runtime draws itself (developer
// console, subtitles): white text, greyscale antialiasing, on black. GDI on Windows
// (text_canvas_win32.cpp); FreeType with the font fontconfig picks elsewhere (text_canvas_posix.cpp;
// built without them every call fails and those overlays stay hidden).
namespace plat
{
    enum class FontFace : uint8_t
    {
        Ui,        // Segoe UI (every accent of the subtitles)
        Monospace, // Consolas
    };

    class TextCanvas
    {
    public:
        TextCanvas();
        ~TextCanvas();
        TextCanvas(const TextCanvas&) = delete;
        TextCanvas& operator=(const TextCanvas&) = delete;

        // The font for the next calls: `pixelHeight` is the character height (no internal leading).
        bool SetFont(FontFace face, int pixelHeight, bool semibold = false);
        // Average character width and line height of the font.
        void Metrics(int& charWidth, int& lineHeight) const;
        // The bitmap, cleared to black. False if it could not be made.
        bool Resize(int width, int height);
        void Clear();
        // One line, top-left at (x, y).
        void Draw(int x, int y, const std::wstring& text);
        // Word-wrapped at `maxWidth`, lines centred: the size it takes, and the drawing.
        void MeasureWrapped(const std::wstring& text, int maxWidth, int& width, int& height);
        void DrawWrapped(const std::wstring& text, int x, int y, int width, int height);
        // Finishes the drawing; then Pixels() holds it (0x00RRGGBB per pixel, rows top-down, the
        // coverage in every channel).
        void Flush();
        uint32_t* Pixels();
        int Width() const { return m_width; }
        int Height() const { return m_height; }

    private:
        struct Impl;
        Impl* m_impl;
        int m_width = 0, m_height = 0;
    };
}
