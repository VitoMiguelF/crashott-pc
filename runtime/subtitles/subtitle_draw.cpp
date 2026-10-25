// Rasterizes the current subtitle (subtitles.h) with the OS (platform/text_canvas.h) at the output resolution and draws it over
// the game image, bottom centre: white text (system UI font, so every accent is there), a black
// outline and an optional translucent box (game.subtitle_background).

#include "subtitle_draw.h"
#include "subtitles.h"

#include "../diagnostics.h"
#include "../gpu/overlay.h"
#include "../settings/settings.h"

#include "../platform/text_canvas.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace subtitles
{
    namespace
    {
        struct Image
        {
            std::vector<uint32_t> pixels;
            int width = 0, height = 0;
        };

        // RGBA8 (R in the low byte), straight alpha: box, then the outline, then the text.
        Image Rasterize(const std::wstring& text, uint32_t outputWidth, uint32_t outputHeight, int sizePercent, int backgroundPercent)
        {
            Image image;
            int fontPx = std::max(12, int(std::lround(outputHeight * 0.043 * sizePercent / 100.0)));
            int outline = std::max(2, fontPx / 12);
            int padX = fontPx / 2 + outline, padY = fontPx / 5 + outline;
            int maxWidth = int(std::min(double(outputWidth), outputHeight * 16.0 / 9.0) * 0.82);  // lines of the 16:9 area on wide screens

            plat::TextCanvas canvas;
            int textW = 0, textH = 0;
            if (canvas.SetFont(plat::FontFace::Ui, fontPx, true))
                canvas.MeasureWrapped(text, maxWidth, textW, textH);
            image.width = textW + padX * 2;
            image.height = textH + padY * 2;
            if (!textW || !canvas.Resize(image.width, image.height))
            {
                image.width = image.height = 0;
                return image;
            }
            canvas.DrawWrapped(text, padX, padY, textW, textH);
            canvas.Flush();

            // Coverage of the text (grey antialiasing: R = G = B).
            const int w = image.width, h = image.height;
            const uint32_t* source = canvas.Pixels();
            std::vector<float> cover(size_t(w) * h);
            for (size_t i = 0; i < cover.size(); i++)
                cover[i] = float((source[i] >> 8) & 0xFF) / 255.0f;

            // Outline: the coverage dilated by a square (separable max, rows then columns), so a change
            // of text costs a few ms even at 4K.
            std::vector<float> rows(cover.size()), ring(cover.size());
            for (int y = 0; y < h; y++)
            {
                const float* line = &cover[size_t(y) * w];
                float* out = &rows[size_t(y) * w];
                for (int x = 0; x < w; x++)
                {
                    float m = 0.0f;
                    for (int sx = std::max(0, x - outline), end = std::min(w - 1, x + outline); sx <= end; sx++)
                        m = std::max(m, line[sx]);
                    out[x] = m;
                }
            }
            for (int y = 0; y < h; y++)
            {
                float* out = &ring[size_t(y) * w];
                for (int sy = std::max(0, y - outline), end = std::min(h - 1, y + outline); sy <= end; sy++)
                {
                    const float* line = &rows[size_t(sy) * w];
                    for (int x = 0; x < w; x++)
                        out[x] = std::max(out[x], line[x]);
                }
            }

            image.pixels.resize(size_t(w) * h);
            const float boxAlpha = std::clamp(backgroundPercent, 0, 100) / 100.0f * 0.85f;
            for (size_t i = 0; i < image.pixels.size(); i++)
            {
                // Box (black), outline (black), text (white), straight alpha "over".
                float a = boxAlpha;
                float o = std::min(1.0f, ring[i] * 1.6f);
                a = o + a * (1.0f - o);
                float t = cover[i];
                float alpha = t + a * (1.0f - t);
                float white = alpha > 0.0f ? t / alpha : 0.0f;
                uint32_t c = uint32_t(std::lround(white * 255.0f));
                uint32_t al = uint32_t(std::lround(alpha * 255.0f));
                image.pixels[i] = c | (c << 8) | (c << 16) | (al << 24);
            }
            return image;
        }
    }

    void Draw(uint32_t width, uint32_t height)
    {
        static uint64_t frame;
        static settings::Values values;
        // settings::Get copies the whole struct under a lock: once every 15 frames is enough.
        if (frame++ % 15 == 0)
            values = settings::Get();
        if (!values.subtitles || !SoundCount())
            return;

        uint64_t textVersion;
        bool gameplay;
        std::wstring text = CurrentText(textVersion, gameplay);
        if (text.empty())
            return;

        static Image image;
        static uint64_t drawnText = ~0ull, imageVersion;
        static uint32_t drawnW, drawnH;
        static int drawnSize, drawnBackground;
        if (textVersion != drawnText || width != drawnW || height != drawnH || values.subtitleSize != drawnSize ||
            values.subtitleBackground != drawnBackground)
        {
            image = Rasterize(text, width, height, values.subtitleSize, values.subtitleBackground);
            if (getenv("CRASH_SUBTITLE_TRACE"))
                Log("[subtitles] drawn: %dx%d at output %ux%u", image.width, image.height, width, height);
            drawnText = textVersion, drawnW = width, drawnH = height;
            drawnSize = values.subtitleSize, drawnBackground = values.subtitleBackground;
            imageVersion++;
        }
        if (image.width <= 0 || image.height <= 0)
            return;
        int x = (int(width) - image.width) / 2;
        // Cutscenes: just above the bottom letterbox bar. In-game lines: above the game's hint box
        // and HUD at the bottom of the screen.
        int y = int(height) - image.height - int(height * (gameplay ? 0.19 : 0.055));
        gpu::overlay::DrawBitmap(width, height, image.pixels.data(), image.width, image.height, x, std::max(0, y), imageVersion);
    }
}
