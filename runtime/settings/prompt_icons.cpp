// Button icons for the device in use (prompt_icons.h).

#include <windows.h>

#include "prompt_icons.h"
#include "controls.h"
#include "menu_art.h"
#include "settings.h"
#include "../kernel/pc_input.h"
#include "../diagnostics.h"
#include "../gpu/png_writer.h"


#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <climits>

namespace settings::prompts
{
    namespace
    {
        // The game's icon textures: the index textures of each size of Titans_v3_32/48/64/96 and
        // KLEPTO__with_outline_20/24/36 (the 64-pixel B is also the HUD's "press B" sprite), by the
        // HD texture key of their indices as R8, with the icon's opaque area. Computed from the
        // exported (palettized) PNGs, whose palette indices are the game's, flipped (bottom row
        // first); runtime/NOTES.md "Controles de PC".
        struct IconTexture
        {
            uint64_t key;
            char32_t icon;
            int box[4];
        };
        constexpr IconTexture ICON_TEXTURES[] = {
            { 0xE2F2FC4B47EB9AF1ull, 0xB1, { 0, 0, 32, 32 } },  // Titans_v3_48_texture_3
            { 0x82B7E650D376E796ull, 0xB1, { 1, 1, 31, 31 } },  // KLEPTO__with_outline_24_texture_1
            { 0x108736AC265E8EADull, 0xB1, { 1, 1, 63, 63 } },  // Titans_v3_64_texture_6
            { 0x346C4EC10E081C2Eull, 0xB2, { 0, 0, 32, 32 } },  // Titans_v3_48_texture_4
            { 0x9CDA9608D6C0B512ull, 0xB2, { 1, 1, 31, 31 } },  // KLEPTO__with_outline_24_texture_2
            { 0x73517BB165106680ull, 0xB2, { 1, 1, 63, 63 } },  // FE_360_b_button_64
            { 0xB788894EC30D60DEull, 0xB3, { 0, 0, 32, 32 } },  // Titans_v3_48_texture_5
            { 0x99601B55C0594C2Dull, 0xB3, { 1, 1, 31, 31 } },  // KLEPTO__with_outline_24_texture_3
            { 0x39322A2824A980D8ull, 0xB3, { 1, 1, 63, 63 } },  // Titans_v3_64_texture_8
            { 0xBE293163EFDB9317ull, 0xB4, { 0, 0, 32, 32 } },  // Titans_v3_48_texture_6
            { 0xA35F7BE86DFCAB79ull, 0xB4, { 1, 1, 31, 31 } },  // KLEPTO__with_outline_24_texture_4
            { 0x562284E0636B6D6Full, 0xB4, { 1, 1, 63, 63 } },  // Titans_v3_64_texture_9
            { 0x475EE51AD2C55C2Full, 0xB5, { 0, 3, 32, 28 } },  // Titans_v3_48_texture_7
            { 0xED28CE7B887FB614ull, 0xB5, { 2, 5, 30, 26 } },  // KLEPTO__with_outline_24_texture_5
            { 0x670B2A2633FA023Dull, 0xB5, { 1, 7, 63, 56 } },  // Titans_v3_96_texture_22
            { 0xF0F09E2665B7AC13ull, 0xB6, { 0, 3, 32, 28 } },  // Titans_v3_48_texture_8
            { 0x257A0369B1C2AA60ull, 0xB6, { 2, 3, 30, 28 } },  // KLEPTO__with_outline_24_texture_6
            { 0xEC294508885419F7ull, 0xB6, { 1, 7, 63, 56 } },  // Titans_v3_96_texture_23
            { 0x73F598FA0ACE8E91ull, 0xB7, { 2, 3, 30, 29 } },  // KLEPTO__with_outline_24_texture_7
            { 0x9715881296A1E2EBull, 0xB7, { 2, 1, 62, 31 } },  // Titans_v3_48_texture_9
            { 0x380546C221BA4E68ull, 0xB7, { 5, 4, 60, 124 } },  // Titans_v3_96_texture_24
            { 0x4CAADC95C37A9A81ull, 0xB8, { 2, 3, 30, 29 } },  // KLEPTO__with_outline_24_texture_8
            { 0x04C47484598088CBull, 0xB8, { 2, 2, 30, 62 } },  // Titans_v3_32_texture_8
            { 0xBD5F472162A67930ull, 0xB8, { 5, 2, 124, 62 } },  // Titans_v3_96_texture_25
            { 0x4FFD72C1BA6D32EAull, 0xB9, { 2, 3, 58, 58 } },  // Titans_v3_32_texture_9
            { 0x22B06FEEB384F498ull, 0xB9, { 0, 20, 128, 128 } },  // Titans_v3_96_texture_26
            { 0x89F5481562BD469Bull, 0xBA, { 3, 9, 60, 54 } },  // Titans_v3_48_texture_12
            { 0x31C4B33621369FE5ull, 0xBA, { 1, 1, 62, 63 } },  // Titans_v3_96_texture_27
            { 0xC89D5A1329F27780ull, 0xBB, { 4, 11, 61, 54 } },  // Titans_v3_48_texture_13
            { 0xE728E27535D6F925ull, 0xBB, { 9, 5, 59, 59 } },  // Titans_v3_96_texture_28
            { 0x263425361CD53D0Aull, 0xBC, { 17, 6, 110, 61 } },  // Titans_v3_96_texture_29
            { 0x99A0A72A78836072ull, 0xBD, { 4, 3, 61, 125 } },  // Titans_v3_96_texture_30
            { 0x2405CB6CDB0BF97Dull, 0xBE, { 5, 2, 124, 62 } },  // Titans_v3_96_texture_31
        };

        using input::pc::Device;

        // Preview only: the device and menus/gameplay to draw for (-1: the real ones).
        int g_previewDevice = -1, g_previewMenus = -1;

        Device CurrentDevice()
        {
            return g_previewDevice >= 0 ? Device(g_previewDevice) : input::pc::PromptDevice();
        }

        bool CurrentMenus()
        {
            return g_previewMenus >= 0 ? g_previewMenus != 0 : input::pc::InMenus();
        }
        using settings::Action;

        // What a keyboard icon shows: a key cap with a label, or a mouse with a button lit.
        struct KeyLabel
        {
            std::string text;  // empty: keep the game's icon
            int mouseButton = 0;  // 1 left, 2 right, 3 middle
        };

        KeyLabel LabelOfKey(uint16_t key)
        {
            KeyLabel label;
            switch (key)
            {
            case VK_LBUTTON: label.mouseButton = 1; return label;
            case VK_RBUTTON: label.mouseButton = 2; return label;
            case VK_MBUTTON: label.mouseButton = 3; return label;
            case VK_XBUTTON1: label.text = "M4"; return label;
            case VK_XBUTTON2: label.text = "M5"; return label;
            case VK_LSHIFT: case VK_RSHIFT: label.text = "SHIFT"; return label;
            case VK_LCONTROL: case VK_RCONTROL: label.text = "CTRL"; return label;
            case VK_LMENU: case VK_RMENU: label.text = "ALT"; return label;
            case VK_BACK: label.text = "BKSP"; return label;
            case VK_PRIOR: label.text = "PGUP"; return label;
            case VK_NEXT: label.text = "PGDN"; return label;
            case VK_DELETE: label.text = "DEL"; return label;
            case VK_INSERT: label.text = "INS"; return label;
            case VK_CAPITAL: label.text = "CAPS"; return label;
            case settings::KEY_WHEEL_UP: label.text = "WHL+"; return label;
            case settings::KEY_WHEEL_DOWN: label.text = "WHL-"; return label;
            default: label.text = settings::KeyName(key); return label;
            }
        }

        KeyLabel LabelOfAction(const settings::Controls& controls, Action action)
        {
            for (uint16_t key : controls.keys[size_t(action)])
            {
                if (key)
                    return LabelOfKey(key);
            }
            return {};
        }

        KeyLabel Text(const char* text)
        {
            KeyLabel label;
            label.text = text;
            return label;
        }

        // The keyboard icon of each game icon, in menus (the fixed menu keys, pc_input.h) or in gameplay.
        KeyLabel KeyboardLabel(char32_t icon, bool menus, const settings::Controls& c)
        {
            switch (icon)
            {
            // Typing a name (save_screens.cpp): Enter is DONE (Y) and Space selects (A).
            case 0xB1: return menus ? Text(input::pc::TextEntry() ? "SPACE" : "ENTER") : LabelOfAction(c, Action::Jump);
            case 0xB2: return menus ? Text("ESC") : LabelOfAction(c, Action::Interact);
            case 0xB3: return menus ? Text("BKSP") : LabelOfAction(c, Action::LightAttack);
            case 0xB4: return menus ? Text(input::pc::TextEntry() ? "ENTER" : "TAB") : LabelOfAction(c, Action::HeavyAttack);
            case 0xB5: return LabelOfAction(c, Action::Map);
            case 0xB6: return menus ? Text("P") : LabelOfAction(c, Action::Pause);
            case 0xB7: return LabelOfAction(c, Action::Block);
            case 0xB8: return menus ? Text("E") : LabelOfAction(c, Action::Finisher);
            case 0xB9:
            {
                // The four movement keys, when they are single letters / digits (WASD), else the first.
                std::string keys;
                for (Action a : { Action::MoveForward, Action::MoveLeft, Action::MoveBack, Action::MoveRight })
                {
                    KeyLabel l = LabelOfAction(c, a);
                    if (l.text.size() != 1)
                        return Text("MOVE");
                    keys += l.text;
                }
                return Text(keys.c_str());
            }
            case 0xBC: return LabelOfAction(c, Action::RotateStick);
            case 0xBD: return LabelOfAction(c, Action::LeftTrigger);
            case 0xBE: return menus ? Text("Q") : LabelOfAction(c, Action::Modifier);
            default: return {};  // < > stay
            }
        }

        uint32_t Hash(uint32_t h, const std::string& text)
        {
            for (char ch : text)
                h = (h ^ uint8_t(ch)) * 16777619u;
            return (h ^ 0xFF) * 16777619u;
        }

        // ---- drawing (premultiplied float RGBA, anti-aliased with signed distances) ----

        struct Color
        {
            float r, g, b, a;
        };

        struct Canvas
        {
            int w, h;
            std::vector<Color> px;
            Canvas(int width, int height) : w(width), h(height), px(size_t(width) * height, Color{ 0, 0, 0, 0 }) {}

            // Paints `color` with coverage from the signed distance `d(x, y)` (pixels, < 0 inside).
            template <typename F>
            void Shape(Color color, F d)
            {
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                    {
                        float coverage = std::clamp(0.5f - d(x + 0.5f, y + 0.5f), 0.0f, 1.0f) * color.a;
                        if (coverage <= 0.0f)
                            continue;
                        Color& p = px[size_t(y) * w + x];
                        p.r = color.r * coverage + p.r * (1 - coverage);
                        p.g = color.g * coverage + p.g * (1 - coverage);
                        p.b = color.b * coverage + p.b * (1 - coverage);
                        p.a = coverage + p.a * (1 - coverage);
                    }
            }
        };

        Color Rgb(uint32_t rgb, float a = 1.0f)
        {
            return { ((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a };
        }

        float Circle(float x, float y, float cx, float cy, float r)
        {
            return std::hypot(x - cx, y - cy) - r;
        }

        float RoundRect(float x, float y, float cx, float cy, float hw, float hh, float radius)
        {
            float qx = std::abs(x - cx) - hw + radius, qy = std::abs(y - cy) - hh + radius;
            return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - radius;
        }

        float Segment(float x, float y, float ax, float ay, float bx, float by, float radius)
        {
            float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
            float t = std::clamp((px * dx + py * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
            return std::hypot(px - dx * t, py - dy * t) - radius;
        }

        // The area of the game's icon (alpha > 25 %), so ours sits where it did.
        void OpaqueBox(const uint32_t* original, int w, int h, float& x0, float& y0, float& x1, float& y1)
        {
            int minX = w, minY = h, maxX = -1, maxY = -1;
            for (int y = 0; original && y < h; y++)
                for (int x = 0; x < w; x++)
                    if ((original[size_t(y) * w + x] >> 24) > 64)
                        minX = std::min(minX, x), maxX = std::max(maxX, x), minY = std::min(minY, y), maxY = std::max(maxY, y);
            if (maxX < 0)
                minX = 0, minY = 0, maxX = w - 1, maxY = h - 1;
            x0 = float(minX), y0 = float(minY), x1 = float(maxX + 1), y1 = float(maxY + 1);
        }

        // Text in the game's large Titans font, centred on (cx, cy), at most `maxWidth` wide.
        void Label(menu_art::Canvas& canvas, const std::string& text, float cx, float cy, float height, float maxWidth, menu_art::Color color)
        {
            std::u32string u = menu_art::Utf32(text);
            // The font's cells have room above and below the capitals: the cell is ~1.45x their height.
            float cell = height * 1.45f;
            float width = menu_art::TextWidth(menu_art::FontId::Large, u, cell);
            if (width > maxWidth && width > 0)
            {
                cell *= maxWidth / width;
                width = maxWidth;
            }
            // Fill only: the font's outline (41 % alpha around each letter) makes small labels blobby.
            menu_art::TextStyle style{ color, { color.r, color.g, color.b, 0 }, 1.0f };
            menu_art::DrawGameText(canvas, menu_art::FontId::Large, u, cx - width * 0.5f, cy - cell * 0.5f, cell, style);
        }

        menu_art::Image ToImage(const Canvas& c)
        {
            menu_art::Image image;
            image.w = c.w, image.h = c.h;
            image.px.resize(c.px.size());
            for (size_t i = 0; i < c.px.size(); i++)
            {
                const Color& p = c.px[i];
                auto q = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
                image.px[i] = q(p.r) | q(p.g) << 8 | q(p.b) << 16 | q(p.a) << 24;  // premultiplied
            }
            return image;
        }

        // PlayStation face button: dark disc, coloured symbol.
        void FaceButton(Canvas& c, char32_t icon, float cx, float cy, float r)
        {
            c.Shape(Rgb(0x0E0E10), [&](float x, float y) { return Circle(x, y, cx, cy, r); });
            c.Shape(Rgb(0x3A3B40), [&](float x, float y) { return Circle(x, y, cx, cy, r * 0.90f); });
            c.Shape(Rgb(0x26272B), [&](float x, float y) { return Circle(x, y, cx, cy - r * 0.04f, r * 0.80f); });
            float s = r * 0.42f, t = std::max(1.2f, r * 0.11f);
            switch (icon)
            {
            case 0xB1:  // Cross
                c.Shape(Rgb(0x8FB8EE), [&](float x, float y) {
                    return std::min(Segment(x, y, cx - s, cy - s, cx + s, cy + s, t), Segment(x, y, cx - s, cy + s, cx + s, cy - s, t));
                });
                break;
            case 0xB2:  // Circle
                c.Shape(Rgb(0xF0717A), [&](float x, float y) { return std::abs(Circle(x, y, cx, cy, s * 1.05f)) - t; });
                break;
            case 0xB3:  // Square
                c.Shape(Rgb(0xE79BD6), [&](float x, float y) { return std::abs(RoundRect(x, y, cx, cy, s * 0.92f, s * 0.92f, t * 0.6f)) - t; });
                break;
            case 0xB4:  // Triangle
                c.Shape(Rgb(0x5FD3B4), [&](float x, float y) {
                    float ax = cx, ay = cy - s * 1.05f, bx = cx - s * 1.08f, by = cy + s * 0.75f, dx = cx + s * 1.08f, dy = by;
                    return std::min({ Segment(x, y, ax, ay, bx, by, t), Segment(x, y, bx, by, dx, dy, t), Segment(x, y, dx, dy, ax, ay, t) });
                });
                break;
            }
        }

        // A light rounded key (keyboard keys, PlayStation shoulder / system buttons).
        void Key(Canvas& c, float x0, float y0, float x1, float y1, float radius)
        {
            float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hw = (x1 - x0) * 0.5f, hh = (y1 - y0) * 0.5f;
            float edge = std::max(1.0f, std::min(hw, hh) * 0.12f);
            c.Shape(Rgb(0x141414), [&](float x, float y) { return RoundRect(x, y, cx, cy, hw, hh, radius); });
            c.Shape(Rgb(0x8C8C8C), [&](float x, float y) { return RoundRect(x, y, cx, cy, hw - edge, hh - edge, radius * 0.8f); });
            c.Shape(Rgb(0xEDEDED), [&](float x, float y) { return RoundRect(x, y, cx, cy - edge * 0.6f, hw - edge * 1.6f, hh - edge * 2.0f, radius * 0.7f); });
        }

        // A mouse with button `button` (1 left, 2 right, 3 middle) lit.
        void Mouse(Canvas& c, float x0, float y0, float x1, float y1, int button)
        {
            float h = y1 - y0, w = std::min(x1 - x0, h * 0.72f);
            float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
            float line = std::max(1.0f, w * 0.07f);
            float split = cy - hh * 0.15f;
            c.Shape(Rgb(0x141414), [&](float x, float y) { return RoundRect(x, y, cx, cy, hw, hh, hw * 0.95f); });
            c.Shape(Rgb(0xEDEDED), [&](float x, float y) { return RoundRect(x, y, cx, cy, hw - line, hh - line, hw * 0.9f); });
            Color lit = Rgb(0xF08A1C);
            auto inside = [&](float x, float y) { return RoundRect(x, y, cx, cy, hw - line, hh - line, hw * 0.9f); };
            if (button == 1 || button == 2)
                c.Shape(lit, [&](float x, float y) {
                    float half = button == 1 ? (x - cx) : (cx - x);  // < 0 on the lit side
                    return std::max({ inside(x, y), y - split, half + line * 0.5f });
                });
            // Button split and wheel.
            c.Shape(Rgb(0x141414), [&](float x, float y) { return std::min(Segment(x, y, cx - hw, split, cx + hw, split, line * 0.5f),
                                                                            Segment(x, y, cx, cy - hh, cx, split, line * 0.5f)); });
            c.Shape(button == 3 ? lit : Rgb(0x6E6E6E), [&](float x, float y) {
                return RoundRect(x, y, cx, cy - hh * 0.48f, w * 0.09f, hh * 0.2f, w * 0.09f);
            });
        }

        void Export(const menu_art::Canvas& canvas, std::vector<uint32_t>& out)
        {
            canvas.Export(out);
        }
    }

    char32_t IconOfTexture(uint64_t key, int box[4])
    {
        for (const IconTexture& t : ICON_TEXTURES)
        {
            if (t.key == key)
            {
                if (box)
                    std::copy(t.box, t.box + 4, box);
                return t.icon;
            }
        }
        return 0;
    }

    uint32_t Style()
    {
        Device device = CurrentDevice();
        if (device == Device::Xbox || !menu_art::Ready(0))
            return 0;
        uint32_t h = 2166136261u;
        h = (h ^ uint32_t(device)) * 16777619u;
        if (device == Device::Keyboard)
        {
            bool menus = CurrentMenus();
            h = (h ^ (menus ? 0x55u : 0xAAu)) * 16777619u;
            settings::Controls controls = settings::Get().controls;
            for (char32_t icon = 0xB1; icon <= 0xBE; icon++)
            {
                KeyLabel label = KeyboardLabel(icon, menus, controls);
                h = Hash(h, label.text + char('0' + label.mouseButton));
            }
        }
        return h ? h : 1;
    }

    namespace
    {
        bool DrawIn(char32_t icon, uint32_t style, int w, int h, const uint32_t* original, const int* box, std::vector<uint32_t>& out);
    }

    bool Draw(char32_t icon, uint32_t style, int w, int h, const uint32_t* original, std::vector<uint32_t>& out)
    {
        return DrawIn(icon, style, w, h, original, nullptr, out);
    }

    bool DrawPalettized(char32_t icon, uint32_t style, int w, int h, const int box[4], std::vector<uint8_t>& indices,
                        std::vector<uint32_t>& palette)
    {
        std::vector<uint32_t> rgba;
        if (!DrawIn(icon, style, w, h, nullptr, box, rgba))
            return false;
        // Up to 256 colours: the most used ones on a 5-bit-per-channel grid (+ 4-bit alpha), the
        // fully transparent one first; every pixel takes the nearest. The game's shader blends
        // the 4 nearest texels' palette colours, so edges stay smooth.
        auto cell = [](uint32_t p) {
            uint32_t a = p >> 24;
            if (a < 8)
                return 0u;
            return 1u + ((p >> 3) & 0x1F) + (((p >> 11) & 0x1F) << 5) + (((p >> 19) & 0x1F) << 10) + ((a >> 4) << 15);
        };
        struct Cell
        {
            uint64_t sum[5] = {};  // r g b a, count
        };
        std::unordered_map<uint32_t, Cell> cells;
        for (uint32_t p : rgba)
        {
            Cell& c = cells[cell(p)];
            c.sum[0] += p & 0xFF, c.sum[1] += (p >> 8) & 0xFF, c.sum[2] += (p >> 16) & 0xFF, c.sum[3] += p >> 24, c.sum[4]++;
        }
        std::vector<std::pair<uint64_t, uint32_t>> order;  // count, cell
        for (auto& [id, c] : cells)
            order.push_back({ id == 0 ? UINT64_MAX : c.sum[4], id });
        std::sort(order.rbegin(), order.rend());
        palette.assign(256, 0);
        size_t used = std::min<size_t>(order.size(), 256);
        for (size_t i = 0; i < used; i++)
        {
            const Cell& c = cells[order[i].second];
            uint64_t n = c.sum[4];
            palette[i] = order[i].second == 0 ? 0u
                       : uint32_t(c.sum[0] / n) | uint32_t(c.sum[1] / n) << 8 | uint32_t(c.sum[2] / n) << 16 | uint32_t(c.sum[3] / n) << 24;
        }
        std::unordered_map<uint32_t, uint8_t> nearest;
        indices.assign(size_t(w) * h, 0);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                uint32_t p = rgba[size_t(y) * w + x];
                auto it = nearest.find(p);
                if (it == nearest.end())
                {
                    int best = 0;
                    long bestDistance = LONG_MAX;
                    for (size_t i = 0; i < used; i++)
                    {
                        uint32_t q = palette[i];
                        long d = 0;
                        for (int ch = 0; ch < 4; ch++)
                        {
                            long diff = long((p >> (8 * ch)) & 0xFF) - long((q >> (8 * ch)) & 0xFF);
                            d += diff * diff * (ch == 3 ? 2 : 1);
                        }
                        if (d < bestDistance)
                            bestDistance = d, best = int(i);
                    }
                    it = nearest.emplace(p, uint8_t(best)).first;
                }
                indices[size_t(h - 1 - y) * w + x] = it->second;  // bottom row first, as the game's
            }
        return true;
    }

    namespace
    {
    bool DrawIn(char32_t icon, uint32_t style, int w, int h, const uint32_t* original, const int* box, std::vector<uint32_t>& out)
    {
        if (!style || w <= 0 || h <= 0 || !menu_art::Ready(0))
            return false;
        Device device = CurrentDevice();
        float x0, y0, x1, y1;
        if (box)
            x0 = float(box[0]), y0 = float(box[1]), x1 = float(box[2]), y1 = float(box[3]);
        else
            OpaqueBox(original, w, h, x0, y0, x1, y1);
        float bw = x1 - x0, bh = y1 - y0, cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
        Canvas shapes(w, h);
        std::string text;
        menu_art::Color textColor{ 24, 24, 26, 255 };
        float textHeight = 0, textWidth = 0, textY = cy;

        if (device == Device::PlayStation)
        {
            switch (icon)
            {
            case 0xB1: case 0xB2: case 0xB3: case 0xB4:
                FaceButton(shapes, icon, cx, cy, std::min(bw, bh) * 0.5f);
                break;
            case 0xB5:  // BACK = the touchpad click (Create / Share also)
            case 0xB6:  // START = Options
            {
                float hh = std::min(bh, bw * 0.6f) * 0.5f;
                Key(shapes, x0, cy - hh, x1, cy + hh, hh * 0.9f);
                float line = std::max(1.0f, hh * 0.13f);
                if (icon == 0xB6)
                    for (int i = -1; i <= 1; i++)
                        shapes.Shape(Rgb(0x1A1A1C), [&](float x, float y) {
                            return Segment(x, y, cx - bw * 0.18f, cy - line * 0.6f + i * hh * 0.34f, cx + bw * 0.18f, cy - line * 0.6f + i * hh * 0.34f, line * 0.6f);
                        });
                else
                    shapes.Shape(Rgb(0x1A1A1C), [&](float x, float y) {
                        return std::abs(RoundRect(x, y, cx, cy - line * 0.6f, bw * 0.22f, hh * 0.42f, hh * 0.12f)) - line * 0.55f;
                    });
                break;
            }
            case 0xB7: case 0xB8: case 0xBD: case 0xBE:  // R2 R1 L2 L1
            {
                bool trigger = icon == 0xB7 || icon == 0xBD;
                // Triggers stand up (like the game's RT / LT), bumpers lie down.
                float kw = trigger ? std::min(bw, bh * 0.62f) : bw, kh = trigger ? bh : std::min(bh, bw * 0.55f);
                Key(shapes, cx - kw * 0.5f, cy - kh * 0.5f, cx + kw * 0.5f, cy + kh * 0.5f, std::min(kw, kh) * 0.35f);
                text = icon == 0xB7 ? "R2" : icon == 0xB8 ? "R1" : icon == 0xBD ? "L2" : "L1";
                textHeight = std::min(kw, kh) * 0.42f;
                textWidth = kw * 0.78f;
                textY = trigger ? cy + kh * 0.18f : cy;
                break;
            }
            default:
                return false;  // stick, arrows, rotate: generic, kept
            }
        }
        else
        {
            KeyLabel label = KeyboardLabel(icon, CurrentMenus(), settings::Get().controls);
            if (label.mouseButton)
                Mouse(shapes, x0, y0, x1, y1, label.mouseButton);
            else if (!label.text.empty())
            {
                // A key cap: at least as wide as high, wider for long names, within the icon's area.
                float kh = std::min(bh, bw);
                float kw = std::clamp(kh * (0.62f + 0.34f * float(label.text.size())), kh, std::max(bw, kh));
                if (bw < kw)
                    kw = bw;
                Key(shapes, cx - kw * 0.5f, cy - kh * 0.5f, cx + kw * 0.5f, cy + kh * 0.5f, kh * 0.22f);
                text = label.text;
                textHeight = kh * (label.text.size() <= 2 ? 0.46f : 0.36f);
                textWidth = kw * 0.8f;
                textY = cy - kh * 0.06f;
            }
            else
                return false;
        }

        menu_art::Canvas canvas;
        canvas.Reset(w, h);
        canvas.Blit(ToImage(shapes), 0, 0);
        if (!text.empty())
            Label(canvas, text, cx, textY, textHeight, textWidth, textColor);
        Export(canvas, out);
        return true;
    }
    }

    int Preview(const std::filesystem::path& directory)
    {
        if (!menu_art::Ready(10000))
        {
            Log("[prompts] preview: the game's fonts could not be read");
            return 1;
        }
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        // Rows: the game's icons, PlayStation, keyboard (gameplay), keyboard (menus); one sheet per height.
        struct Row
        {
            int device, menus;
        };
        const Row rows[] = { { int(Device::Xbox), 0 }, { int(Device::PlayStation), 0 }, { int(Device::Keyboard), 0 }, { int(Device::Keyboard), 1 } };
        for (int height : { 32, 64, 128 })
        {
            int gap = height / 4, sheetW = gap, sheetH = gap + int(std::size(rows)) * (height + gap);
            for (char32_t icon = 0xB1; icon <= 0xBE; icon++)
                sheetW += int(std::ceil(menu_art::IconWidth(icon, float(height)))) + gap;
            std::vector<uint32_t> sheet(size_t(sheetW) * sheetH, 0xFF403830);
            for (size_t r = 0; r < std::size(rows); r++)
            {
                int x = gap, y = gap + int(r) * (height + gap);
                for (char32_t icon = 0xB1; icon <= 0xBE; icon++)
                {
                    int w = int(std::ceil(menu_art::IconWidth(icon, float(height))));
                    menu_art::Canvas canvas;
                    canvas.Reset(w, height);
                    g_previewDevice = int(Device::Xbox);  // the game's own icon (DrawIcon follows the device too)
                    menu_art::DrawIcon(canvas, icon, 0, 0, float(height));
                    g_previewDevice = -1;
                    std::vector<uint32_t> original, ours;
                    canvas.Export(original);
                    const std::vector<uint32_t>* image = &original;
                    if (r > 0)
                    {
                        g_previewDevice = rows[r].device, g_previewMenus = rows[r].menus;
                        if (Draw(icon, Style(), w, height, original.data(), ours))
                            image = &ours;
                        g_previewDevice = g_previewMenus = -1;
                    }
                    for (int py = 0; py < height; py++)
                        for (int px = 0; px < w; px++)
                        {
                            uint32_t s = (*image)[size_t(py) * w + px], a = s >> 24;
                            uint32_t& d = sheet[size_t(y + py) * sheetW + x + px];
                            uint32_t out = 0xFF000000;
                            for (int c = 0; c < 3; c++)
                            {
                                uint32_t sc = (s >> (8 * c)) & 0xFF, dc = (d >> (8 * c)) & 0xFF;
                                out |= ((sc * a + dc * (255 - a)) / 255) << (8 * c);
                            }
                            d = out;
                        }
                    x += w + gap;
                }
            }
            std::filesystem::path file = directory / ("icones_" + std::to_string(height) + ".png");
            gpu::WritePng(file, reinterpret_cast<const uint8_t*>(sheet.data()), uint32_t(sheetW), uint32_t(sheetH), uint32_t(sheetW * 4));
            Log("[prompts] preview: %s", file.string().c_str());
        }
        return 0;
    }
}
