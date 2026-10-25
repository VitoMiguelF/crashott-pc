// Rasterizes the current subtitle (subtitles.h) with the OS (platform/text_canvas.h) at the output resolution and draws it over
// the game image, bottom centre: white text (system UI font, so every accent is there), a black
// outline and an optional translucent box (game.subtitle_background). Colour tags in the text,
// {cor=amarelo}...{/cor} or {cor=#RRGGBB}...{/cor} (they nest), colour parts of a line.

#include "subtitle_draw.h"
#include "subtitles.h"

#include "../diagnostics.h"
#include "../gpu/overlay.h"
#include "../settings/settings.h"

#include "../platform/text_canvas.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <string>
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

        constexpr uint32_t WHITE = 0xFFFFFF;  // 0xRRGGBB

        // Names accepted by {cor=...} (any case), picked to read well over the black outline. Each
        // has an ...Escuro (38% towards black) and a ...Claro (45% towards white) variant.
        struct NamedColor
        {
            const wchar_t* name;
            uint32_t rgb;
        };
        constexpr NamedColor NAMED_COLORS[] = {
            { L"branco", 0xFFFFFF },
            { L"amarelo", 0xFFD84A },      { L"amareloEscuro", 0x9E862E },      { L"amareloClaro", 0xFFEA9B },
            { L"laranja", 0xFF9B3D },      { L"laranjaEscuro", 0x9E6026 },      { L"laranjaClaro", 0xFFC894 },
            { L"vermelho", 0xFF5C52 },     { L"vermelhoEscuro", 0x9E3933 },     { L"vermelhoClaro", 0xFFA5A0 },
            { L"rosa", 0xFF85CC },         { L"rosaEscuro", 0x9E527E },         { L"rosaClaro", 0xFFBCE3 },
            { L"roxo", 0xC090FF },         { L"roxoEscuro", 0x77599E },         { L"roxoClaro", 0xDCC2FF },
            { L"azul", 0x6FB6FF },         { L"azulEscuro", 0x45719E },         { L"azulClaro", 0xB0D7FF },
            { L"ciano", 0x5FE3EE },        { L"cianoEscuro", 0x3B8D94 },        { L"cianoClaro", 0xA7F0F6 },
            { L"verde", 0x7FDF7A },        { L"verdeEscuro", 0x4F8A4C },        { L"verdeClaro", 0xB9EDB6 },
            { L"cinza", 0xB8B8B8 },        { L"cinzaEscuro", 0x727272 },        { L"cinzaClaro", 0xD8D8D8 },
            { L"verdeAzulado", 0x94B3B1 }, { L"verdeAzuladoEscuro", 0x5C6F6E }, { L"verdeAzuladoClaro", 0xC4D5D4 },
            { L"cinzaAzulado", 0xBCC5CE }, { L"cinzaAzuladoEscuro", 0x757A80 }, { L"cinzaAzuladoClaro", 0xDADFE4 },
        };

        std::wstring LowerAscii(std::wstring s)
        {
            for (wchar_t& c : s)
                c = wchar_t(towlower(wint_t(c)));
            return s;
        }

        bool ParseColor(std::wstring value, uint32_t& rgb)
        {
            value = LowerAscii(value);
            for (const NamedColor& named : NAMED_COLORS)
                if (value == LowerAscii(named.name))
                    return rgb = named.rgb, true;
            if (!value.empty() && value[0] == L'#')
                value.erase(0, 1);
            if (value.size() != 6 || value.find_first_not_of(L"0123456789abcdef") != std::wstring::npos)
                return false;
            rgb = uint32_t(wcstoul(value.c_str(), nullptr, 16));
            return true;
        }

        // The text without its colour tags, and the colour of each character. A brace that isn't a
        // valid tag stays as text.
        struct Styled
        {
            std::wstring text;
            std::vector<uint32_t> colors;
        };

        Styled ParseTags(const std::wstring& raw)
        {
            Styled out;
            std::vector<uint32_t> stack{ WHITE };
            for (size_t i = 0; i < raw.size(); i++)
            {
                if (raw[i] == L'{')
                {
                    size_t close = raw.find(L'}', i);
                    std::wstring tag = close != std::wstring::npos && close - i <= 32 ? raw.substr(i + 1, close - i - 1) : std::wstring();
                    uint32_t rgb;
                    if (tag == L"/cor")
                    {
                        if (stack.size() > 1)
                            stack.pop_back();
                        i = close;
                        continue;
                    }
                    if (tag.rfind(L"cor=", 0) == 0 && ParseColor(tag.substr(4), rgb))
                    {
                        stack.push_back(rgb);
                        i = close;
                        continue;
                    }
                }
                out.text.push_back(raw[i]);
                out.colors.push_back(stack.back());
            }
            return out;
        }

        // Lines of the text as [begin, end) ranges: breaks at "\n" and wraps at spaces to fit
        // `maxWidth` (centred, like DrawText DT_CENTER | DT_WORDBREAK; a word wider than that stays whole).
        struct Line
        {
            size_t begin, end;
            int width;
        };

        int TextWidth(const plat::TextCanvas& canvas, const std::wstring& text, size_t begin, size_t end)
        {
            return end > begin ? canvas.TextWidth(text.substr(begin, end - begin)) : 0;
        }

        std::vector<Line> Layout(const plat::TextCanvas& canvas, const std::wstring& text, int maxWidth)
        {
            std::vector<Line> lines;
            size_t paragraph = 0;
            while (paragraph <= text.size())
            {
                size_t paragraphEnd = std::min(text.find(L'\n', paragraph), text.size());
                size_t start = paragraph;
                do
                {
                    size_t fit = start, pos = start;
                    while (pos < paragraphEnd)
                    {
                        size_t wordEnd = std::min(text.find(L' ', pos), paragraphEnd);
                        if (fit != start && TextWidth(canvas, text, start, wordEnd) > maxWidth)
                            break;
                        fit = pos = wordEnd;
                        while (pos < paragraphEnd && text[pos] == L' ')
                            pos++;
                    }
                    lines.push_back({ start, fit, TextWidth(canvas, text, start, fit) });
                    start = fit;
                    while (start < paragraphEnd && text[start] == L' ')
                        start++;
                } while (start < paragraphEnd);
                paragraph = paragraphEnd + 1;
            }
            return lines;
        }

        // RGBA8 (R in the low byte), straight alpha: box, then the outline, then the text.
        Image Rasterize(const std::wstring& raw, uint32_t outputWidth, uint32_t outputHeight, int sizePercent, int backgroundPercent)
        {
            Image image;
            int fontPx = std::max(12, int(std::lround(outputHeight * 0.043 * sizePercent / 100.0)));
            int outline = std::max(2, fontPx / 12);
            int padX = fontPx / 2 + outline, padY = fontPx / 5 + outline;
            int maxWidth = int(std::min(double(outputWidth), outputHeight * 16.0 / 9.0) * 0.82);  // lines of the 16:9 area on wide screens

            const Styled styled = ParseTags(raw);
            const std::wstring& text = styled.text;
            if (text.find_first_not_of(L" \n") == std::wstring::npos)
                return image;  // only tags
            plat::TextCanvas canvas;
            if (!canvas.SetFont(plat::FontFace::Ui, fontPx, true))
                return image;
            int charWidth = 0, lineHeight = 0;
            canvas.Metrics(charWidth, lineHeight);
            const std::vector<Line> lines = Layout(canvas, text, maxWidth);
            int textW = 0;
            for (const Line& line : lines)
                textW = std::max(textW, line.width);
            int textH = lineHeight * int(lines.size());
            image.width = textW + padX * 2;
            image.height = textH + padY * 2;
            if (!textW || !canvas.Resize(image.width, image.height))
            {
                image.width = image.height = 0;
                return image;
            }
            // The text is drawn white (its coverage); each run of one colour also paints its box in
            // `tint`, the colour the text takes there.
            const int w = image.width, h = image.height;
            std::vector<uint32_t> tint(size_t(w) * h, WHITE);
            for (size_t n = 0; n < lines.size(); n++)
            {
                const Line& line = lines[n];
                int lineX = padX + (textW - line.width) / 2, y = padY + int(n) * lineHeight;
                for (size_t run = line.begin; run < line.end;)
                {
                    size_t runEnd = run;
                    while (runEnd < line.end && styled.colors[runEnd] == styled.colors[run])
                        runEnd++;
                    int x = lineX + TextWidth(canvas, text, line.begin, run);
                    canvas.Draw(x, y, text.substr(run, runEnd - run));
                    if (styled.colors[run] != WHITE)
                    {
                        int xEnd = std::min(w, lineX + TextWidth(canvas, text, line.begin, runEnd));
                        for (int py = std::max(0, y); py < std::min(h, y + lineHeight); py++)
                            std::fill(tint.begin() + (size_t(py) * w + std::max(0, x)), tint.begin() + (size_t(py) * w + std::max(0, xEnd)),
                                      styled.colors[run]);
                    }
                    run = runEnd;
                }
            }
            canvas.Flush();

            // Coverage of the text (grey antialiasing: R = G = B).
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
                // Box (black), outline (black), text (its tint), straight alpha "over".
                float a = boxAlpha;
                float o = std::min(1.0f, ring[i] * 1.6f);
                a = o + a * (1.0f - o);
                float t = cover[i];
                float alpha = t + a * (1.0f - t);
                float k = alpha > 0.0f ? t / alpha : 0.0f;
                uint32_t r = uint32_t(std::lround(k * float((tint[i] >> 16) & 0xFF)));
                uint32_t g = uint32_t(std::lround(k * float((tint[i] >> 8) & 0xFF)));
                uint32_t b = uint32_t(std::lround(k * float(tint[i] & 0xFF)));
                uint32_t al = uint32_t(std::lround(alpha * 255.0f));
                image.pixels[i] = r | (g << 8) | (b << 16) | (al << 24);
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
