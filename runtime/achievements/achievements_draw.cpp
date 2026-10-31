// The achievements' drawing (achievements.h), on the render thread: the hint on the main menu and
// the pause, the screen (the game's message box, like the cheat menu and the save screens'
// questions) and the unlock popup (the Xbox 360's: a dark rounded bar at the bottom), each a CPU
// canvas through its own overlay bitmap channel. Also --achievements-preview.

#include "achievements_internal.h"
#include "../diagnostics.h"
#include "../gpu/overlay.h"
#include "../gpu/png_writer.h"
#include "../platform/platform.h"
#include "../settings/prompt_icons.h"
#include "../settings/settings.h"
#include "../settings/ui_text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace achievements
{
    using namespace detail;

    namespace
    {
        using settings::UiLanguage;
        using settings::UiText;
        using settings::menu_art::Color;
        using settings::menu_art::FontId;
        using settings::menu_art::Image;
        using settings::menu_art::TextStyle;
        namespace menu_art = settings::menu_art;

        // ---- texts -------------------------------------------------------------------------------
        // "\xC2\xB2" / "\xC2\xB5" are the fonts' B / BACK icons (menu_art.h ICON_B, ICON_BACK).
        const UiText T_TITLE = { "ACHIEVEMENTS", "CONQUISTAS", "SUCCÈS", "ERFOLGE", "LOGROS", "OBIETTIVI", "PRESTATIES" };
        const UiText T_UNLOCKED_POPUP = { "Achievement unlocked", "Conquista desbloqueada", "Succès débloqué", "Erfolg freigeschaltet",
                                          "Logro desbloqueado", "Obiettivo sbloccato", "Prestatie ontgrendeld" };
        const UiText T_COUNT = { "%d of %d unlocked", "%d de %d desbloqueadas", "%d sur %d débloqués", "%d von %d freigeschaltet",
                                 "%d de %d desbloqueados", "%d di %d sbloccati", "%d van %d ontgrendeld" };
        const UiText T_UNLOCKED_ON = { "Unlocked %s", "Desbloqueada em %s", "Débloqué le %s", "Freigeschaltet am %s", "Desbloqueado el %s",
                                       "Sbloccato il %s", "Ontgrendeld op %s" };
        const UiText T_LOCKED = { "LOCKED", "BLOQUEADA", "VERROUILLÉ", "GESPERRT", "BLOQUEADO", "BLOCCATO", "VERGRENDELD" };
        const UiText T_UNLOCKED = { "UNLOCKED", "DESBLOQUEADA", "DÉBLOQUÉ", "FREIGESCHALTET", "DESBLOQUEADO", "SBLOCCATO", "ONTGRENDELD" };
        const UiText T_SECRET = { "Secret achievement", "Conquista secreta", "Succès secret", "Geheimer Erfolg", "Logro secreto",
                                  "Obiettivo segreto", "Geheime prestatie" };
        const UiText T_SECRET_HOW = { "Keep playing to unlock this secret achievement.", "Continue jogando para desbloquear esta conquista secreta.",
                                      "Continuez à jouer pour débloquer ce succès secret.", "Spiel weiter, um diesen geheimen Erfolg freizuschalten.",
                                      "Sigue jugando para desbloquear este logro secreto.", "Continua a giocare per sbloccare questo obiettivo segreto.",
                                      "Blijf spelen om deze geheime prestatie te ontgrendelen." };
        const UiText T_BACK = { "BACK", "VOLTAR", "RETOUR", "ZURÜCK", "ATRÁS", "INDIETRO", "TERUG" };

        const UiLanguage* g_language = nullptr;  // Preview sets each

        const char* T(const UiText& text)
        {
            static const UiLanguage startup = settings::UiLanguageOf(settings::StartupLanguage().id);
            return settings::Tr(text, g_language ? *g_language : startup);
        }

        // "YYYY-MM-DD HH:MM:SS" as the language writes a date.
        std::string FormatDate(const std::string& stored)
        {
            int y, mo, d, h, mi;
            if (sscanf(stored.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5)
                return stored;
            UiLanguage language = g_language ? *g_language : settings::UiLanguageOf(settings::StartupLanguage().id);
            char text[48];
            if (language == UiLanguage::English)
                snprintf(text, sizeof(text), "%02d/%02d/%04d %02d:%02d", mo, d, y, h, mi);
            else if (language == UiLanguage::German)
                snprintf(text, sizeof(text), "%02d.%02d.%04d %02d:%02d", d, mo, y, h, mi);
            else if (language == UiLanguage::Dutch)
                snprintf(text, sizeof(text), "%02d-%02d-%04d %02d:%02d", d, mo, y, h, mi);
            else
                snprintf(text, sizeof(text), "%02d/%02d/%04d %02d:%02d", d, mo, y, h, mi);
            return text;
        }

        std::string Fill(const char* format, const std::string& value)
        {
            std::string out = format;
            if (size_t at = out.find("%s"); at != std::string::npos)
                out.replace(at, 2, value);
            return out;
        }

        // ---- drawing helpers ---------------------------------------------------------------------
        constexpr float VIRTUAL_W = 1280.0f, VIRTUAL_H = 720.0f;

        // The save screens' / cheat menu's colours over a message frame.
        constexpr TextStyle MSG_TEXT{ { 238, 226, 196, 255 }, { 52, 28, 4, 255 } };
        constexpr TextStyle MSG_ITEM{ { 232, 160, 0, 255 }, { 104, 48, 8, 255 } };
        constexpr TextStyle MSG_SELECTED{ { 144, 216, 232, 255 }, { 24, 104, 128, 255 } };
        constexpr TextStyle KEY_NORMAL{ { 214, 120, 14, 255 }, { 70, 28, 0, 255 } };
        constexpr TextStyle STATE_ON{ { 150, 232, 96, 255 }, { 28, 84, 12, 255 } };
        constexpr TextStyle STATE_OFF{ { 176, 160, 136, 255 }, { 52, 36, 20, 255 } };
        constexpr Color FRAME{ 236, 222, 190, 190 }, DIM{ 14, 8, 0, 150 };
        // The popup: the Xbox 360's notification (white text on a dark grey bar).
        constexpr TextStyle POPUP_TITLE{ { 196, 196, 196, 255 }, { 16, 16, 16, 255 } };
        constexpr TextStyle POPUP_NAME{ { 255, 255, 255, 255 }, { 16, 16, 16, 255 } };

        struct View
        {
            float s, ox, oy;
            float X(float x) const { return ox + x * s; }
            float Y(float y) const { return oy + y * s; }
            int IX(float x) const { return int(std::lround(X(x))); }
            int IY(float y) const { return int(std::lround(Y(y))); }
        };

        View FullView(int width, int height)
        {
            View v;
            v.s = std::min(width / VIRTUAL_W, height / VIRTUAL_H);
            v.ox = (width - VIRTUAL_W * v.s) * 0.5f, v.oy = (height - VIRTUAL_H * v.s) * 0.5f;
            return v;
        }

        float Width(FontId font, const std::string& text, float cell)
        {
            return menu_art::TextWidth(font, menu_art::Utf32(text), cell);
        }

        float Text(menu_art::Canvas& canvas, const View& v, FontId font, const std::string& text, float x, float y, float cell, const TextStyle& style,
                   float align = 0.0f, float maxWidth = 1e9f)
        {
            float fitted = cell;
            std::u32string u = menu_art::FitText(font, menu_art::Utf32(text), fitted, maxWidth, 0.7f);
            y += (cell - fitted) * 0.5f, cell = fitted;
            float width = menu_art::TextWidth(font, u, cell);
            float left = x - width * align;
            menu_art::DrawGameText(canvas, font, u, v.X(left), v.Y(y), cell * v.s, style);
            return left + width;
        }

        std::vector<std::string> Wrap(const std::string& text, FontId font, float cell, float width)
        {
            std::vector<std::string> lines;
            std::string line;
            size_t at = 0;
            while (at < text.size())
            {
                size_t end = text.find(' ', at);
                std::string word = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
                at = end == std::string::npos ? text.size() : end + 1;
                std::string tried = line.empty() ? word : line + " " + word;
                if (!line.empty() && Width(font, tried, cell) > width)
                {
                    lines.push_back(line);
                    line = word;
                }
                else
                    line = tried;
            }
            if (!line.empty())
                lines.push_back(line);
            return lines;
        }

        // A rounded rectangle w x h (pixels), corner radius r, `fill` inside a 1-pixel `border`;
        // premultiplied, anti-aliased.
        Image RoundedRect(int w, int h, float r, Color fill, Color border, float borderWidth)
        {
            Image image;
            image.w = std::max(1, w), image.h = std::max(1, h);
            image.px.resize(size_t(image.w) * image.h);
            for (int y = 0; y < image.h; y++)
            {
                for (int x = 0; x < image.w; x++)
                {
                    // Distance from the edge, inside > 0 (the corners are circles of radius r).
                    float px = x + 0.5f, py = y + 0.5f;
                    float dx = std::max({ r - px, px - (image.w - r), 0.0f }), dy = std::max({ r - py, py - (image.h - r), 0.0f });
                    float inside = (dx > 0 && dy > 0) ? r - std::sqrt(dx * dx + dy * dy) : std::min({ px, py, image.w - px, image.h - py });
                    float coverage = std::clamp(inside + 0.5f, 0.0f, 1.0f);
                    float edge = std::clamp(borderWidth - inside + 0.5f, 0.0f, 1.0f);  // 1 on the border
                    if (coverage <= 0)
                        continue;
                    float rr = fill.r + (border.r - fill.r) * edge, gg = fill.g + (border.g - fill.g) * edge, bb = fill.b + (border.b - fill.b) * edge;
                    float aa = (fill.a + (border.a - fill.a) * edge) / 255.0f * coverage;
                    auto c = [&](float value) { return uint32_t(std::lround(value * aa)); };
                    image.px[size_t(y) * image.w + x] = c(rr) | c(gg) << 8 | c(bb) << 16 | uint32_t(std::lround(aa * 255)) << 24;
                }
            }
            return image;
        }

        // `icon` at size x size with its corners rounded (the dashboard's tiles).
        void DrawIconTile(menu_art::Canvas& canvas, const Image& icon, int x, int y, int size)
        {
            if (icon.w <= 0)
                return;
            const Image& scaled = menu_art::Scaled(icon, size, size);
            Image tile = scaled;
            float r = size * 0.12f;
            for (int py = 0; py < size; py++)
            {
                for (int px = 0; px < size; px++)
                {
                    float fx = px + 0.5f, fy = py + 0.5f;
                    float dx = std::max({ r - fx, fx - (size - r), 0.0f }), dy = std::max({ r - fy, fy - (size - r), 0.0f });
                    if (dx <= 0 || dy <= 0)
                        continue;
                    float coverage = std::clamp(r - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
                    uint32_t& p = tile.px[size_t(py) * size + px];
                    auto k = [&](uint32_t c) { return uint32_t(std::lround(c * coverage)); };
                    p = k(p & 0xFF) | k((p >> 8) & 0xFF) << 8 | k((p >> 16) & 0xFF) << 16 | k(p >> 24) << 24;
                }
            }
            canvas.Blit(tile, x, y);
        }

        struct Entry
        {
            const Achievement* a;
            bool unlocked;
            std::string date;
        };

        // ---- the screen ----------------------------------------------------------------------------
        // The title, "X of Y unlocked  -  G / total G", the grid of icons at the left and the
        // selected one at the right (big icon, gamerscore, name, description, date), the hint.
        void RasterizeScreen(menu_art::Canvas& canvas, const Visual& vis, const std::map<uint16_t, std::string>& unlocked, int width, int height,
                             std::vector<HitZone>& zones)
        {
            const Data& data = GetData();
            View v = FullView(width, height);
            canvas.Reset(width, height);
            zones.clear();
            canvas.Fill(0, 0, width, height, DIM);

            std::vector<Entry> entries;
            int count = 0, score = 0;
            for (const Achievement& a : data.list)
            {
                auto it = unlocked.find(a.id);
                bool on = it != unlocked.end();
                entries.push_back({ &a, on, on ? it->second : std::string() });
                if (on)
                    count++, score += a.gamerscore;
            }
            const int rows = (int(entries.size()) + COLUMNS - 1) / COLUMNS;

            constexpr float TITLE = 44.0f, SUMMARY = 24.0f, HINT = 28.0f, ICON = 54.0f, PITCH = 64.0f, GAP = 10.0f, DETAIL_W = 400.0f, BETWEEN = 34.0f;
            const float gridW = PITCH * COLUMNS - (PITCH - ICON), gridH = PITCH * rows - (PITCH - ICON);
            const float contentW = gridW + BETWEEN + DETAIL_W;
            const float contentH = TITLE + 8.0f + SUMMARY + 16.0f + gridH + 16.0f + HINT;
            float frameW, frameH;
            menu_art::FrameOuterSize(contentW, contentH, GAP, frameW, frameH);
            frameW = std::min(frameW, 1260.0f), frameH = std::min(frameH, 716.0f);
            const float Y0 = std::round((VIRTUAL_H - frameH) * 0.5f), Y1 = Y0 + frameH;
            const float X0 = std::round((VIRTUAL_W - frameW) * 0.5f), X1 = VIRTUAL_W - X0;
            menu_art::DrawMessageFrame(canvas, v.IX(X0), v.IY(Y0), v.IX(X1), v.IY(Y1), FRAME, { 0, 0, 0, 190 });
            auto inset = [&](float y, float cell) { return menu_art::FrameSideInset(X0, Y0, X1, Y1, y, y + cell) + GAP; };
            const float cx = VIRTUAL_W * 0.5f;

            float y = Y0 + menu_art::FrameTopInset(Y0, Y1) + GAP;
            Text(canvas, v, FontId::Large, T(T_TITLE), cx, y, TITLE, MSG_TEXT, 0.5f);
            y += TITLE + 8.0f;
            char counted[96];
            snprintf(counted, sizeof(counted), T(T_COUNT), count, int(entries.size()));
            char scored[48];
            snprintf(scored, sizeof(scored), "     %d / %d G", score, data.totalScore);
            Text(canvas, v, FontId::Small, std::string(counted) + scored, cx, y, SUMMARY, MSG_ITEM, 0.5f, X1 - X0 - 2.0f * inset(y, SUMMARY));
            y += SUMMARY + 16.0f;

            // One left edge for the whole grid: clear of the curls over its height.
            const float side = menu_art::FrameSideInset(X0, Y0, X1, Y1, y, y + gridH) + GAP;
            const float room = (X1 - side) - (X0 + side);
            const float gx = X0 + side + std::max(0.0f, (room - contentW) * 0.5f), gy = y;
            for (int i = 0; i < int(entries.size()); i++)
            {
                const Entry& e = entries[i];
                float x = gx + PITCH * float(i % COLUMNS), top = gy + PITCH * float(i / COLUMNS);
                bool selected = vis.selected == i;
                if (selected)
                {
                    // A cyan frame round the selected tile (the game's selected text colour).
                    float pad = 5.0f;
                    Image ring = RoundedRect(int(std::lround((ICON + pad * 2) * v.s)), int(std::lround((ICON + pad * 2) * v.s)), 9.0f * v.s,
                                             { 40, 120, 140, 120 }, { 144, 216, 232, 255 }, std::max(2.0f, 3.0f * v.s));
                    canvas.Blit(ring, v.IX(x - pad), v.IY(top - pad));
                }
                int px = v.IX(x), py = v.IY(top), size = std::max(1, int(std::lround(ICON * v.s)));
                if (e.unlocked)
                    DrawIconTile(canvas, e.a->icon, px, py, size);
                else if (!e.a->secret)
                    DrawIconTile(canvas, e.a->iconLocked, px, py, size);
                else
                {
                    Image tile = RoundedRect(size, size, size * 0.12f, { 34, 30, 26, 255 }, { 70, 62, 54, 255 }, std::max(1.0f, v.s));
                    canvas.Blit(tile, px, py);
                    Text(canvas, v, FontId::Large, "?", x + ICON * 0.5f, top + ICON * 0.18f, ICON * 0.64f, STATE_OFF, 0.5f);
                }
                zones.push_back({ x - 4.0f, top - 4.0f, x + ICON + 4.0f, top + ICON + 4.0f, Zone::Cell, i });
            }

            // The selected one.
            if (vis.selected >= 0 && vis.selected < int(entries.size()))
            {
                const Entry& e = entries[vis.selected];
                const Achievement& a = *e.a;
                const float dx = gx + gridW + BETWEEN, dw = std::min(DETAIL_W, X1 - side - dx);
                constexpr float BIG = 96.0f;
                int size = std::max(1, int(std::lround(BIG * v.s)));
                bool hidden = !e.unlocked && a.secret;
                if (hidden)
                {
                    Image tile = RoundedRect(size, size, size * 0.12f, { 34, 30, 26, 255 }, { 70, 62, 54, 255 }, std::max(1.0f, v.s));
                    canvas.Blit(tile, v.IX(dx), v.IY(gy));
                    Text(canvas, v, FontId::Large, "?", dx + BIG * 0.5f, gy + BIG * 0.18f, BIG * 0.64f, STATE_OFF, 0.5f);
                }
                else
                    DrawIconTile(canvas, e.unlocked ? a.icon : a.iconLocked, v.IX(dx), v.IY(gy), size);
                float tx = dx + BIG + 16.0f;
                Text(canvas, v, FontId::Large, std::to_string(a.gamerscore) + " G", tx, gy + 8.0f, 40.0f, e.unlocked ? STATE_ON : STATE_OFF, 0.0f,
                     dx + dw - tx);
                Text(canvas, v, FontId::Large, T(e.unlocked ? T_UNLOCKED : T_LOCKED), tx, gy + 56.0f, 26.0f, e.unlocked ? STATE_ON : STATE_OFF, 0.0f,
                     dx + dw - tx);
                float ty = gy + BIG + 18.0f;
                for (const std::string& line : Wrap(hidden ? T(T_SECRET) : a.name, FontId::Small, 32.0f, dw))
                {
                    Text(canvas, v, FontId::Small, line, dx, ty, 32.0f, MSG_SELECTED, 0.0f, dw);
                    ty += 36.0f;
                }
                ty += 6.0f;
                const std::string& description = hidden ? std::string(T(T_SECRET_HOW)) : e.unlocked ? a.description : a.howTo;
                for (const std::string& line : Wrap(description, FontId::Small, 25.0f, dw))
                {
                    Text(canvas, v, FontId::Small, line, dx, ty, 25.0f, MSG_TEXT, 0.0f, dw);
                    ty += 29.0f;
                }
                if (e.unlocked)
                {
                    ty += 10.0f;
                    Text(canvas, v, FontId::Small, Fill(T(T_UNLOCKED_ON), FormatDate(e.date)), dx, ty, 23.0f, MSG_ITEM, 0.0f, dw);
                }
            }
            y = gy + gridH + 16.0f;

            // (B) BACK, as the game's hints (with the device's icons).
            std::string back = std::string("\xC2\xB2 ") + T(T_BACK);
            float hs = inset(y, HINT);
            float backEnd = Text(canvas, v, FontId::Large, back, X0 + hs, y, HINT, KEY_NORMAL);
            zones.push_back({ X0 + hs - 10.0f, y - 6.0f, backEnd + 10.0f, y + HINT + 6.0f, Zone::Back, 0 });
        }

        // ---- the popup -----------------------------------------------------------------------------
        constexpr float POPUP_W = 480.0f, POPUP_H = 84.0f, POPUP_BOTTOM = 52.0f;
        constexpr uint64_t POPUP_MS = 5500, FADE_IN_MS = 250, FADE_OUT_MS = 450;

        // The bar alone, at its own size (`v.s` pixels per unit, origin at its top-left).
        void RasterizePopup(menu_art::Canvas& canvas, const Achievement& a, float s)
        {
            int w = int(std::ceil(POPUP_W * s)), h = int(std::ceil(POPUP_H * s));
            canvas.Reset(w, h);
            View v{ s, 0.0f, 0.0f };
            Image bar = RoundedRect(w, h, POPUP_H * 0.32f * s, { 28, 28, 30, 235 }, { 120, 120, 124, 255 }, std::max(1.0f, 1.5f * s));
            canvas.Blit(bar, 0, 0);
            constexpr float ICON = 60.0f;
            const float ix = 20.0f, iy = (POPUP_H - ICON) * 0.5f;
            DrawIconTile(canvas, a.icon, v.IX(ix), v.IY(iy), std::max(1, int(std::lround(ICON * s))));
            const float tx = ix + ICON + 14.0f, tw = POPUP_W - tx - POPUP_H * 0.4f;
            Text(canvas, v, FontId::Small, T(T_UNLOCKED_POPUP), tx, 12.0f, 24.0f, POPUP_TITLE, 0.0f, tw);
            Text(canvas, v, FontId::Small, std::to_string(a.gamerscore) + "G - " + a.name, tx, 40.0f, 30.0f, POPUP_NAME, 0.0f, tw);
        }

        // ---- the hint ------------------------------------------------------------------------------
        // "(BACK) ACHIEVEMENTS" at the bottom, between the game's "(B) BACK" and "SELECT (A)".
        constexpr float HINT_CELL = 44.0f, HINT_TOP = 621.0f;

        std::string HintText()
        {
            return std::string("\xC2\xB5 ") + T(T_TITLE);
        }

        // ---- render-thread state -------------------------------------------------------------------
        menu_art::Canvas g_canvas, g_popupCanvas, g_hintCanvas;
        std::vector<uint32_t> g_pixels, g_popupPixels, g_popupFaded, g_hintPixels;
        std::vector<HitZone> g_newZones;
        Visual g_drawn;
        uint64_t g_drawnUnlocks = UINT64_MAX;
        uint32_t g_drawnStyle = 0;
        int g_drawnW = 0, g_drawnH = 0;
        uint64_t g_version = 0, g_popupVersion = 0, g_hintVersion = 0;

        // The popup on screen: its achievement and when it appeared.
        int g_popupIndex = -1;
        uint64_t g_popupStart = 0;
        float g_popupScale = 0.0f;
        int g_popupAlpha = -1;

        // The hint, drawn for this size / icon style.
        int g_hintW = 0, g_hintH = 0;
        uint32_t g_hintStyle = UINT32_MAX;
        int g_hintPx = 0, g_hintPy = 0;

        void DrawHint(uint32_t width, uint32_t height, uint32_t style)
        {
            View v = FullView(int(width), int(height));
            const std::string text = HintText();
            float textW = Width(FontId::Large, text, HINT_CELL);
            if (int(width) != g_hintW || int(height) != g_hintH || style != g_hintStyle)
            {
                g_hintW = int(width), g_hintH = int(height), g_hintStyle = style;
                int w = int(std::ceil((textW + 8.0f) * v.s)), h = int(std::ceil((HINT_CELL + 8.0f) * v.s));
                g_hintCanvas.Reset(w, h);
                View local{ v.s, 4.0f * v.s, 4.0f * v.s };
                Text(g_hintCanvas, local, FontId::Large, text, 0.0f, 0.0f, HINT_CELL, KEY_NORMAL);
                g_hintCanvas.Export(g_hintPixels);
                g_hintPx = v.IX(VIRTUAL_W * 0.5f - textW * 0.5f - 4.0f), g_hintPy = v.IY(HINT_TOP - 4.0f);
                g_hintVersion++;
                float x0 = VIRTUAL_W * 0.5f - textW * 0.5f;
                SetZones(Zone::Hint, { { x0 - 10.0f, HINT_TOP - 8.0f, x0 + textW + 10.0f, HINT_TOP + HINT_CELL + 8.0f, Zone::Hint, 0 } });
            }
            gpu::overlay::DrawBitmap(width, height, g_hintPixels.data(), g_hintCanvas.Width(), g_hintCanvas.Height(), g_hintPx, g_hintPy, g_hintVersion,
                                     gpu::overlay::BITMAP_ACHIEVEMENT_HINT);
        }

        void DrawPopup(uint32_t width, uint32_t height)
        {
            const Data& data = GetData();
            uint64_t now = plat::TickCountMs();
            if (g_popupIndex >= 0 && now - g_popupStart >= POPUP_MS)
                g_popupIndex = -1;
            if (g_popupIndex < 0)
            {
                uint16_t id;
                if (!TakePopup(id))
                    return;
                auto it = std::find_if(data.list.begin(), data.list.end(), [&](const Achievement& a) { return a.id == id; });
                if (it == data.list.end())
                    return;
                g_popupIndex = int(it - data.list.begin());
                g_popupStart = now;
                g_popupScale = 0.0f;
            }
            View v = FullView(int(width), int(height));
            if (v.s != g_popupScale)
            {
                RasterizePopup(g_popupCanvas, data.list[g_popupIndex], v.s);
                g_popupCanvas.Export(g_popupPixels);
                g_popupScale = v.s;
                g_popupAlpha = -1;
            }
            uint64_t t = now - g_popupStart;
            float fade = t < FADE_IN_MS ? float(t) / FADE_IN_MS : t > POPUP_MS - FADE_OUT_MS ? float(POPUP_MS - t) / FADE_OUT_MS : 1.0f;
            fade = std::clamp(fade, 0.0f, 1.0f);
            int alpha = int(std::lround(fade * 255.0f));
            if (alpha != g_popupAlpha)
            {
                g_popupAlpha = alpha;
                g_popupFaded.resize(g_popupPixels.size());
                for (size_t i = 0; i < g_popupPixels.size(); i++)
                {
                    uint32_t p = g_popupPixels[i];
                    g_popupFaded[i] = (p & 0x00FFFFFF) | (((p >> 24) * uint32_t(alpha) / 255) << 24);
                }
                g_popupVersion++;
            }
            // Rises a little as it appears.
            float rise = (1.0f - std::min(1.0f, float(t) / FADE_IN_MS)) * 16.0f;
            int x = v.IX(VIRTUAL_W * 0.5f - POPUP_W * 0.5f), y = v.IY(VIRTUAL_H - POPUP_BOTTOM - POPUP_H + rise);
            gpu::overlay::DrawBitmap(width, height, g_popupFaded.data(), g_popupCanvas.Width(), g_popupCanvas.Height(), x, y, g_popupVersion,
                                     gpu::overlay::BITMAP_ACHIEVEMENT_POPUP);
        }
    }

    void Draw(uint32_t width, uint32_t height)
    {
        if (!width || !height || !menu_art::Ready(0) || !GetData().ok)
            return;
        uint32_t style = settings::prompts::Style();
        Visual vis = CurrentVisual();
        if (vis.open)
        {
            uint64_t unlocks;
            std::map<uint16_t, std::string> unlocked = Unlocked(&unlocks);
            if (int(width) != g_drawnW || int(height) != g_drawnH)
            {
                g_drawnW = int(width), g_drawnH = int(height);
                g_drawn = {};
            }
            if (!(vis == g_drawn) || style != g_drawnStyle || unlocks != g_drawnUnlocks)
            {
                RasterizeScreen(g_canvas, vis, unlocked, int(width), int(height), g_newZones);
                SetZones(Zone::Cell, g_newZones);
                g_canvas.Export(g_pixels);
                g_drawn = vis;
                g_drawnStyle = style;
                g_drawnUnlocks = unlocks;
                g_version++;
            }
            gpu::overlay::DrawBitmap(width, height, g_pixels.data(), int(width), int(height), 0, 0, g_version, gpu::overlay::BITMAP_ACHIEVEMENTS);
        }
        else
        {
            g_drawn = {};
            if (HintWanted())
                DrawHint(width, height, style);
        }
        DrawPopup(width, height);
    }

    int Preview(const std::filesystem::path& directory)
    {
        if (!menu_art::Ready(10000))
        {
            printf("achievements preview: the game's fonts are missing\n");
            return 1;
        }
        const Data& data = GetData();
        if (!data.ok)
        {
            printf("achievements preview: no achievements in default.xex\n");
            return 1;
        }
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        struct Language
        {
            const char* id;
            UiLanguage language;
        };
        const Language languages[] = { { "english", UiLanguage::English }, { "pt-br", UiLanguage::Portuguese }, { "french", UiLanguage::French },
                                        { "german", UiLanguage::German },   { "spanish", UiLanguage::Spanish },   { "italian", UiLanguage::Italian },
                                        { "dutch", UiLanguage::Dutch } };
        // Every third one unlocked, a secret one among them.
        std::map<uint16_t, std::string> unlocked;
        for (size_t i = 0; i < data.list.size(); i += 3)
            unlocked[data.list[i].id] = "2026-10-31 14:05:12";
        int secret = -1, locked = -1;
        for (size_t i = 0; i < data.list.size(); i++)
        {
            if (data.list[i].secret && secret < 0 && !unlocked.count(data.list[i].id))
                secret = int(i);
            if (!data.list[i].secret && locked < 0 && !unlocked.count(data.list[i].id))
                locked = int(i);
        }
        auto save = [&](const std::vector<uint32_t>& pixels, int w, int h, const char* name) {
            // Over a dark green, like a level behind the pause.
            const uint32_t back[3] = { 30, 70, 40 };
            std::vector<uint32_t> image(pixels.size());
            for (size_t i = 0; i < image.size(); i++)
            {
                uint32_t p = pixels[i], a = p >> 24;
                auto mix = [&](uint32_t c, uint32_t b) { return (c * a + b * (255 - a) + 127) / 255; };
                image[i] = mix(p & 0xFF, back[0]) | (mix((p >> 8) & 0xFF, back[1]) << 8) | (mix((p >> 16) & 0xFF, back[2]) << 16) | (255u << 24);
            }
            return gpu::WritePng(directory / name, reinterpret_cast<const uint8_t*>(image.data()), uint32_t(w), uint32_t(h), uint32_t(w) * 4);
        };
        int written = 0;
        std::vector<HitZone> zones;
        for (const Language& language : languages)
        {
            g_language = &language.language;
            SetPreviewLanguage(g_language);
            menu_art::ClearCache();
            const Data& data = GetData();
            struct Size
            {
                int w, h;
            };
            for (Size size : { Size{ 1280, 720 }, Size{ 1920, 1080 }, Size{ 2560, 1080 } })
            {
                int shot = 0;
                for (int selected : { 0, locked, secret })
                {
                    if (selected < 0)
                        continue;
                    Visual vis;
                    vis.open = true;
                    vis.selected = selected;
                    RasterizeScreen(g_canvas, vis, unlocked, size.w, size.h, zones);
                    g_canvas.Export(g_pixels);
                    char name[96];
                    snprintf(name, sizeof(name), "%s_%dx%d_screen%d.png", language.id, size.w, size.h, shot++);
                    written += save(g_pixels, size.w, size.h, name);
                }
                float s = std::min(size.w / VIRTUAL_W, size.h / VIRTUAL_H);
                RasterizePopup(g_popupCanvas, data.list[0], s);
                g_popupCanvas.Export(g_popupPixels);
                char name[96];
                snprintf(name, sizeof(name), "%s_%dx%d_popup.png", language.id, size.w, size.h);
                written += save(g_popupPixels, g_popupCanvas.Width(), g_popupCanvas.Height(), name);
            }
        }
        g_language = nullptr;
        SetPreviewLanguage(nullptr);
        menu_art::ClearCache();
        printf("achievements preview: %d images in %s\n", written, directory.string().c_str());
        return written ? 0 : 1;
    }
}
