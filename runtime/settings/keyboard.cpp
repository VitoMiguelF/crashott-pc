// The on-screen keyboard (keyboard.h): made for the name entry (save_screens.cpp), shared with the
// MULTIPLAYER fields.

#include "keyboard.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <tuple>

namespace settings::keyboard
{
    namespace
    {
        using menu_art::Color;
        using menu_art::FontId;
        using menu_art::TextStyle;

        constexpr float VIRTUAL_W = 1280.0f, WIDE_W = 248.0f, WIDE_GAP = 12.0f;

        // The frontend's colours on its cloth (settings_menu.cpp, CLOTH).
        constexpr TextStyle KEY_NORMAL{ { 214, 120, 14, 255 }, { 70, 28, 0, 255 } };
        constexpr TextStyle KEY_SELECTED{ { 150, 218, 212, 255 }, { 30, 104, 110, 255 } };
        constexpr TextStyle KEY_OFF{ { 132, 84, 26, 255 }, { 84, 42, 2, 110 } };
        constexpr Color PLATE{ 92, 44, 4, 70 }, PLATE_EDGE{ 70, 28, 0, 120 };
        constexpr Color PLATE_SELECTED{ 250, 236, 200, 120 }, PLATE_SELECTED_EDGE{ 30, 104, 110, 220 };

        std::vector<Key> MakeKeys()
        {
            static const char* ROWS[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
            std::vector<Key> keys;
            for (int row = 0; row < 4; row++)
            {
                int n = int(strlen(ROWS[row]));
                float left = VIRTUAL_W * 0.5f - ((n - 1) * PITCH_X + KEY_W) * 0.5f;
                for (int i = 0; i < n; i++)
                    keys.push_back({ char32_t(ROWS[row][i]), row, left + i * PITCH_X, row * PITCH_Y, KEY_W, KEY_H });
            }
            float left = VIRTUAL_W * 0.5f - WIDE_W - WIDE_GAP * 0.5f, y = 4 * PITCH_Y;
            keys.push_back({ ERASE, 4, left, y, WIDE_W, KEY_H });
            keys.push_back({ DONE, 4, left + WIDE_W + WIDE_GAP, y, WIDE_W, KEY_H });
            return keys;
        }

        // A rounded plate (the keys), antialiased; cached by size and colour.
        const menu_art::Image& Plate(int w, int h, int radius, Color fill, Color edge, int edgeWidth)
        {
            static std::map<std::tuple<int, int, int, uint32_t, uint32_t, int>, menu_art::Image> cache;
            auto pack = [](Color c) { return uint32_t(c.r) | uint32_t(c.g) << 8 | uint32_t(c.b) << 16 | uint32_t(c.a) << 24; };
            auto [it, inserted] = cache.try_emplace({ w, h, radius, pack(fill), pack(edge), edgeWidth });
            menu_art::Image& image = it->second;
            if (!inserted)
                return image;
            image.w = w, image.h = h;
            image.px.assign(size_t(w) * h, 0);
            auto distance = [&](float x, float y) {
                // Signed distance to the rounded rectangle's border (negative inside).
                float qx = std::fabs(x - w * 0.5f) - (w * 0.5f - radius), qy = std::fabs(y - h * 0.5f) - (h * 0.5f - radius);
                float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
                return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
            };
            for (int y = 0; y < h; y++)
            {
                for (int x = 0; x < w; x++)
                {
                    float d = distance(x + 0.5f, y + 0.5f);
                    float cover = std::clamp(0.5f - d, 0.0f, 1.0f);
                    if (cover <= 0.0f)
                        continue;
                    float t = std::clamp(-d - edgeWidth + 0.5f, 0.0f, 1.0f);  // 0 on the edge, 1 inside
                    float a = (edge.a * (1.0f - t) + fill.a * t) / 255.0f * cover;
                    float r = edge.r * (1.0f - t) + fill.r * t, g = edge.g * (1.0f - t) + fill.g * t, b = edge.b * (1.0f - t) + fill.b * t;
                    auto p = [&](float c) { return uint32_t(std::lround(c * a)); };
                    image.px[size_t(y) * w + x] = p(r) | p(g) << 8 | p(b) << 16 | uint32_t(std::lround(a * 255.0f)) << 24;
                }
            }
            return image;
        }
    }

    const std::vector<Key>& Keys()
    {
        static const std::vector<Key> keys = MakeKeys();
        return keys;
    }

    int KeyOf(char32_t c)
    {
        const std::vector<Key>& keys = Keys();
        for (size_t i = 0; i < keys.size(); i++)
        {
            if (keys[i].c == c)
                return int(i);
        }
        return -1;
    }

    int Step(int key, int dx, int dy)
    {
        const std::vector<Key>& keys = Keys();
        const Key& from = keys[size_t(key)];
        if (dx)
        {
            std::vector<int> row;
            for (size_t i = 0; i < keys.size(); i++)
            {
                if (keys[i].row == from.row)
                    row.push_back(int(i));
            }
            auto at = std::find(row.begin(), row.end(), key) - row.begin();
            return row[size_t((at + dx + int(row.size())) % int(row.size()))];
        }
        int target = (from.row + dy + 5) % 5;
        float cx = from.x + from.w * 0.5f;
        int best = key;
        float bestDistance = 1e9f;
        for (size_t i = 0; i < keys.size(); i++)
        {
            if (keys[i].row != target)
                continue;
            float d = std::fabs(keys[i].x + keys[i].w * 0.5f - cx);
            if (d < bestDistance)
                bestDistance = d, best = int(i);
        }
        return best;
    }

    void Draw(menu_art::Canvas& canvas, float scale, float ox, float oy, float top, int selected, bool portuguese,
              const std::function<bool(char32_t)>& enabled, const std::function<void(int, float, float, float, float)>& zone)
    {
        auto X = [&](float x) { return int(std::lround(ox + x * scale)); };
        auto Y = [&](float y) { return int(std::lround(oy + y * scale)); };
        const std::vector<Key>& keys = Keys();
        for (size_t i = 0; i < keys.size(); i++)
        {
            const Key& key = keys[i];
            float y = top + key.y;
            bool on = int(i) == selected;
            int px = X(key.x), py = Y(y), pw = X(key.x + key.w) - px, ph = Y(y + key.h) - py;
            int radius = std::max(2, int(std::lround(9.0f * scale))), edge = std::max(1, int(std::lround(2.0f * scale)));
            canvas.Blit(Plate(pw, ph, radius, on ? PLATE_SELECTED : PLATE, on ? PLATE_SELECTED_EDGE : PLATE_EDGE, edge), px, py);
            zone(int(i), key.x - 3.0f, y - 3.0f, key.x + key.w + 3.0f, y + key.h + 3.0f);
            const TextStyle& style = on ? KEY_SELECTED : enabled(key.c) ? KEY_NORMAL : KEY_OFF;
            std::string label;
            float cell;
            if (key.c == ERASE || key.c == DONE)
            {
                // The game's own words for X and Y on the name screen (ERASE / DONE).
                label = key.c == ERASE ? (portuguese ? "APAGAR" : "ERASE") : (portuguese ? "PRONTO" : "DONE");
                cell = on ? 34.0f : 30.0f;
            }
            else
            {
                label = std::string(1, char(key.c));
                cell = on ? 44.0f : 38.0f;
            }
            std::u32string text = menu_art::Utf32(label);
            float width = menu_art::TextWidth(FontId::Large, text, cell);
            float tx = key.x + key.w * 0.5f - width * 0.5f, ty = y + (key.h - cell) * 0.5f + 1.0f;
            menu_art::DrawGameText(canvas, FontId::Large, text, ox + tx * scale, oy + ty * scale, cell * scale, style);
        }
    }
}
