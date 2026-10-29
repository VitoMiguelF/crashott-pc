#include "direct_boot.h"
#include "settings_menu.h"
#include "options.h"
#include "settings.h"
#include "menu_art.h"
#include "../audio/game_volumes.h"
#include "../diagnostics.h"
#include "../gpu/gpu.h"
#include "../gpu/overlay.h"
#include "../gpu/png_writer.h"
#include "../gpu/renderer.h"
#include "../gpu/upscaler.h"
#include "../fs/rcf_overlay.h"
#include "../gpu/window.h"
#include "../kernel/input.h"
#include "../kernel/pad_slots.h"
#include "../kernel/pc_input.h"
#include "../subtitles/subtitles.h"
#include "../platform/platform.h"
#include "../kernel/xinput_types.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace settings::menu
{
    namespace
    {
        using menu_art::Color;
        using menu_art::FontId;
        using menu_art::TextStyle;

        // Layout in a virtual 1280x720 screen, scaled to the window (the menu is drawn at the
        // window's resolution, whatever the internal one or the upscaler).
        constexpr float VIRTUAL_W = 1280.0f, VIRTUAL_H = 720.0f;
        constexpr float TITLE_Y = 18.0f, TITLE_CELL = 44.0f;
        constexpr float TABS_Y = 74.0f, TAB_CELL = 40.0f, TAB_GAP = 40.0f;
        constexpr float ROWS_TOP = 132.0f, ROWS_BOTTOM = 492.0f;  // the rows are centered between these
        constexpr float ROW_PITCH = 44.0f, ROW_CELL = 36.0f;
        constexpr float LABEL_RIGHT = 608.0f;  // labels end here (right-aligned, as in the pause menu)
        constexpr float VALUE_X = 650.0f;      // values start here
        constexpr float PANEL_L = 150.0f, PANEL_R = 1130.0f;
        constexpr float HELP_Y = 500.0f, HELP_H = 88.0f, HELP_CELL = 28.0f;
        // The footer is on the line of the game's own B BACK hint (bottom left, x 235-375 at 720p).
        constexpr float FOOTER_Y = 624.0f, FOOTER_CELL = 34.0f, NOTE_CENTER = 622.0f;
        constexpr int STARS = 10;

        // Opening and closing: the game plays its screen transition to the (emptied) PS2 options
        // screen first; the settings then fade in rising a little. Closing presses B for the game
        // (the options screen's ExitBack) while the settings fade out.
        constexpr uint64_t OPEN_DELAY_MS = 120, FADE_IN_MS = 220, FADE_OUT_MS = 110, BACK_PRESS_MS = 200;
        constexpr float RISE = 18.0f;  // virtual pixels
        constexpr uint64_t RESTART_DELAY_MS = 250;  // "restarting..." on screen before the restart

        // The game's frontend colours, measured in its screens. On the frontend's cloth
        // (Old Runs\build\ptbr\08_menu_episodio.png, menu_settings.png): items orange with a dark
        // brown outline, the selected one light cyan with a teal outline; the title is orange too
        // (the game's dark brown titles vanish on the dark top of the cloth). Over the dimmed menu (if the options screen could not be emptied,
        // see frontend_patches.cpp), the pause menu's (12_pausa_opcoes.png): orange on dark brown,
        // white titles with a black outline, and its swirls at the sides.
        struct Palette
        {
            bool dim;
            TextStyle title, normal, selected, info, help, warning;
            Color frame, divider, starEmpty, line;
        };
        constexpr Palette CLOTH{
            false,
            { { 236, 150, 20, 255 }, { 70, 28, 0, 255 } },
            { { 214, 120, 14, 255 }, { 70, 28, 0, 255 } },
            { { 150, 218, 212, 255 }, { 30, 104, 110, 255 } },
            { { 132, 84, 26, 255 }, { 84, 42, 2, 110 } },
            { { 238, 226, 196, 255 }, { 52, 28, 4, 255 } },
            { { 232, 64, 24, 255 }, { 80, 16, 0, 255 } },
            { 236, 222, 190, 200 }, { 214, 120, 14, 230 }, { 84, 42, 2, 80 }, { 84, 42, 2, 90 },
        };
        constexpr Palette DIMMED{
            true,
            { { 248, 248, 248, 255 }, { 0, 0, 0, 255 } },
            { { 232, 160, 0, 255 }, { 104, 48, 8, 255 } },
            { { 144, 216, 232, 255 }, { 24, 104, 128, 255 } },
            { { 160, 128, 80, 255 }, { 60, 36, 8, 255 } },
            { { 238, 226, 196, 255 }, { 52, 28, 4, 255 } },
            { { 255, 128, 72, 255 }, { 96, 24, 0, 255 } },
            { 236, 222, 190, 190 }, { 236, 210, 160, 230 }, { 96, 64, 24, 200 }, { 236, 200, 140, 70 },
        };
        constexpr Color DIM_TOP{ 14, 8, 0, 232 }, DIM_BOTTOM{ 8, 4, 0, 242 };  // hides the main menu behind
        // In a game the settings' tabs hide the game's options screen (its texts and hints) completely.
        constexpr Color DIM_TOP_IN_GAME{ 14, 8, 0, 252 }, DIM_BOTTOM_IN_GAME{ 8, 4, 0, 252 };
        constexpr Color SWIRL{ 240, 228, 200, 120 };
        const Palette* g_palette = &CLOTH;
        const Palette* g_previewPalette = nullptr;  // --settings-preview

        constexpr uint64_t REPEAT_DELAY_MS = 380;
        constexpr uint64_t REPEAT_MS = 90;

        // The items, the tabs and their rows, names, values and help: options.h (the launcher's too).
        using options::ActionOf;
        using options::IsBinding;
        using options::IsHeader;
        using options::Item;
        using options::Tab;
        using options::TAB_COUNT;
        using options::Text;

        // A tab shows at most this many rows; longer ones (CONTROLS) scroll with the selection.
        constexpr size_t MAX_ROWS = 9;

        const Tab* const TABS = options::Tabs();
        constexpr int GAME_TAB = 0, DEFINITIVE_TAB = 5;

        bool Selectable(Item item)
        {
            return item != Item::Storage && !IsHeader(item);
        }

        std::atomic<bool> g_available{ false };
        std::atomic<bool> g_requested{ false };
        std::atomic<bool> g_open{ false };
        std::atomic<bool> g_releasing{ false };
        std::atomic<uint64_t> g_backUntil{ 0 };  // B for the game until then (closing)
        std::atomic<bool> g_fromMainMenu{ true };  // where RequestOpen came from (the restart warning)
        std::atomic<bool> g_overGame{ false };     // RequestOpenOverGame: no game screen behind to leave

        // During a game the screen opens with the pause menu's OPTIONS, over the game's in-game
        // options screen (InGameOptions in Frontend.bfig), which it hides: its CO-OP, INVERT AXIS
        // and AUTOSAVE are in the GAME tab (game_options.cpp) and its DIALOGUE / MUSIC / SFX
        // stars are the AUDIO tab's volumes (audio/game_volumes.h). The node of that screen and the
        // node shown now (NoteFrontendNode).
        std::atomic<uint32_t> g_optionsNode{ UINT32_MAX };
        std::atomic<uint32_t> g_frontendNode{ UINT32_MAX };

        // Render-thread state.
        Values g_values;
        int g_tab = 0;
        int g_row[TAB_COUNT] = {};  // selected row of each tab
        bool g_capturing = false;   // CONTROLS: waiting for the key of the selected binding (pc_input.h)
        uint16_t g_previousButtons = 0;
        uint16_t g_heldButton = 0;
        uint64_t g_heldSince = 0, g_lastRepeat = 0;
        bool g_dirty = true;
        menu_art::Canvas g_canvas;
        std::vector<uint32_t> g_pixels;
        uint64_t g_version = 0;
        uint64_t g_openedAt = 0, g_closedAt = 0;
        bool g_closing = false;  // fading out
        // Leaving with options that need a restart: "restart now?" (A restart, B later), then
        // "restarting..." for a moment before Restart.
        bool g_confirm = false;
        uint64_t g_restartAt = 0;
        std::vector<uint32_t> g_faded;  // g_pixels at the fade's opacity
        uint64_t g_fadedVersion = 0;
        int g_drawnW = 0, g_drawnH = 0;
        int g_first[TAB_COUNT] = {};  // first row shown of each tab (long tabs scroll to keep the selection in view)

        // The mouse: what each part of the screen does, as last drawn (virtual 1280x720 units).
        enum class Zone : uint8_t { Tab, TabPrev, TabNext, Row, ArrowLeft, ArrowRight, Stars, Back, Accept, Yes, No };
        struct HitZone
        {
            float x0, y0, x1, y1;
            Zone zone;
            int index;  // the tab or the row
        };
        std::vector<HitZone> g_zones;
        uint32_t g_mouseMoves = 0, g_mouseClicks = 0;  // pc_input counters already seen
        int g_dragRow = -1;  // the stars being dragged (row), or -1

        // Texts -----------------------------------------------------------------------------------

        bool Portuguese()
        {
            return g_values.language.rfind("pt", 0) == 0;
        }

        const char* T(const Text& text)
        {
            return Portuguese() ? text.pt : text.en;
        }

        // Key(item): the setting behind each item, as in settings.toml (also the name in the log).
        // RestartOnly(item): the game reads it at boot; it applies after a restart, which leaving
        // the screen offers (marked with the game's "rotate" icon). Resolution and upscaler apply
        // when the screen closes (the renderer rebuilds its targets). Both from options.h.
        std::string Label(Item item)
        {
            return options::Label(item, Portuguese());
        }

        // The game restarts by itself (CRASH_RESTART is not "off").
        bool CanRestart()
        {
            return GetRestartMode() != RestartMode::Off;
        }

        // Opened from the pause menu's OPTIONS, during a game.
        bool InGame()
        {
            return !g_fromMainMenu;
        }

        // Frontend.bfig: the InGameOptions node is the owner of the first exit after its name
        // ([0xBC27359D][u32 length][name, NUL, padded to 4][u32 owner node]...). Read-only.
        bool FindOptionsNode(std::vector<uint8_t>& data)
        {
            auto be32 = [&](size_t at) {
                return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) | (uint32_t(data[at + 2]) << 8) | data[at + 3];
            };
            const std::string name = std::string("InGameOptions") + '\0';
            const uint8_t record[4] = { 0xBC, 0x27, 0x35, 0x9D };
            auto at = std::search(data.begin(), data.end(), name.begin(), name.end());
            if (at != data.end())
                at = std::search(at, std::min(data.end(), at + 0x800), std::begin(record), std::end(record));
            size_t offset = size_t(at - data.begin());
            if (at != data.end() && offset + 8 <= data.size())
            {
                size_t fields = (offset + 8 + be32(offset + 4) + 1 + 3) & ~size_t(3);
                if (fields + 4 <= data.size())
                {
                    g_optionsNode = be32(fields);
                    Log("[settings] pause menu: in-game options screen is frontend node %u (opens the settings)", g_optionsNode.load());
                    return true;  // nothing changed
                }
            }
            Log("[settings] pause menu: InGameOptions not found in Frontend.bfig; the settings open only from the main menu");
            return false;
        }

        // What the options' texts depend on here (options.h).
        options::Context OptionContext()
        {
            options::Context context;
            context.pt = Portuguese();
            context.canRestart = CanRestart();
            context.dlss = gpu::upscaler::Available(Upscaler::Dlss);
            context.fsr = gpu::upscaler::Available(Upscaler::Fsr);
            context.haveSubtitles = subtitles::SoundCount() > 0;
            return context;
        }

        // Two lines about the selected item.
        std::pair<std::string, std::string> Help(Item item)
        {
            if (IsBinding(item))
            {
                if (g_capturing)
                    return { T({ "Press the new key or mouse button.", "Pressione a nova tecla ou botão do mouse." }),
                             T({ "Esc cancels.", "Esc cancela." }) };
                return { T({ "A / Enter: choose a key (it takes over the first one).", "A / Enter: escolher a tecla (substitui a primeira)." }),
                         T({ "X / Backspace: back to the default.", "X / Backspace: volta ao padrão." }) };
            }
            return options::Help(item, g_values, OptionContext());
        }

        // Sliders: value, minimum, maximum and step.
        int* Percent(Item item, int* minimum = nullptr, int* maximum = nullptr, int* step = nullptr)
        {
            return options::Percent(item, g_values, minimum, maximum, step);
        }

        // [controls] player1 / player2: "AUTO", or the controller's name ("CONTROLE 2", "DUALSENSE (USB)",
        // "+ JOGADOR 2" when it is on the other player now, "(DESCONECTADO)" when it is not connected).
        std::string PlayerDeviceText(const std::string& key)
        {
            if (key == "auto")
                return T({ "AUTO", "AUTO" });
            if (key == "keyboard")
                return T({ "KEYBOARD", "TECLADO" });
            for (const input::slots::Device& device : input::slots::Devices())
            {
                if (device.key != key)
                    continue;
                std::string name = device.name;
                if (key.compare(0, 4, "xbox") == 0 && name.compare(0, 9, "Controle ") == 0)
                    name = T({ "CONTROLLER ", "CONTROLE " }) + name.substr(9);
                for (char& c : name)
                    c = char(toupper(uint8_t(c)));
                return name;
            }
            std::string name = key.compare(0, 4, "xbox") == 0 ? std::string(T({ "CONTROLLER ", "CONTROLE " })) + key.substr(4)
                                                              : "PLAYSTATION " + key.substr(11);
            return name + T({ " (NOT CONNECTED)", " (DESCONECTADO)" });
        }

        std::string Value(Item item)
        {
            if (IsBinding(item) && g_capturing && item == TABS[g_tab].items[size_t(g_row[g_tab])])
                return T({ "PRESS A KEY...", "PRESSIONE UMA TECLA..." });
            if (item == Item::Player1 || item == Item::Player2)
                return PlayerDeviceText(g_values.controls.players[item == Item::Player2]);
            return options::Value(item, g_values, OptionContext());
        }

        bool NeedsRestart()
        {
            return settings::NeedsRestart(g_values);
        }

        // The changed options that need a restart, by name ("LANGUAGE, MUSIC").
        std::string RestartItems()
        {
            const Values& boot = AtStartup();
            std::string list;
            auto add = [&](bool changed, Item item) {
                if (changed)
                    list += (list.empty() ? "" : ", ") + Label(item);
            };
            add(g_values.language != boot.language, Item::Language);
            add(g_values.ps2MojoColors != boot.ps2MojoColors, Item::MojoColors);
            add(g_values.hdRemaster != boot.hdRemaster, Item::HdRemaster);
            return list;
        }

        Item SelectedItem()
        {
            return TABS[g_tab].items[size_t(g_row[g_tab])];
        }

        // Drawing ---------------------------------------------------------------------------------

        // Virtual 1280x720 -> window pixels.
        struct View
        {
            float s, ox, oy;
            float X(float x) const { return ox + x * s; }
            float Y(float y) const { return oy + y * s; }
            int IX(float x) const { return int(std::lround(X(x))); }
            int IY(float y) const { return int(std::lround(Y(y))); }
        };

        float Width(FontId font, const std::string& text, float cell)
        {
            return menu_art::TextWidth(font, menu_art::Utf32(text), cell);
        }

        // Draws `text` in virtual units; `align` 0 = left, 1 = right, 0.5 = centered; shrinks it to
        // `maxWidth` (virtual) when wider. Returns the virtual x after it.
        float Draw(const View& v, FontId font, const std::string& text, float x, float y, float cell, const TextStyle& style,
                   float align = 0.0f, float maxWidth = 1e9f)
        {
            std::u32string u = menu_art::Utf32(text);
            float width = menu_art::TextWidth(font, u, cell);
            if (width > maxWidth && width > 0.0f)
            {
                float fitted = cell * maxWidth / width;
                y += (cell - fitted) * 0.5f;
                cell = fitted, width = maxWidth;
            }
            float left = x - width * align;
            menu_art::DrawGameText(g_canvas, font, u, v.X(left), v.Y(y), cell * v.s, style);
            return left + width;
        }

        // The pause menu's swirls (InGame_Pause_Background, 64x256, drawn additively by the game),
        // repeated down both edges of the window.
        void DrawSwirls(const View& v, int width, int height)
        {
            const menu_art::Image* strip = menu_art::GetArt(menu_art::Art::PauseStrip);
            if (!strip)
                return;
            float k = 1.75f * v.s;
            int w = int(std::lround(strip->w * k)), h = int(std::lround(strip->h * k));
            const menu_art::Image& left = menu_art::Scaled(*strip, w, h, true);
            const menu_art::Image& right = menu_art::Scaled(*strip, w, h);
            int inset = int(std::lround(10.0f * v.s));
            for (int y = -h / 3; y < height; y += h)
            {
                g_canvas.BlitGlow(left, -inset, y, SWIRL);
                g_canvas.BlitGlow(right, width - w + inset, y, SWIRL);
            }
        }

        // MSG_Background (the top-left corner of the game's message boxes: curl, edge and black
        // inside), mirrored into the four corners; the straight part is stretched.
        void DrawFrame(const View& v, float x0, float y0, float x1, float y1)
        {
            menu_art::DrawMessageFrame(g_canvas, v.IX(x0), v.IY(y0), v.IX(x1), v.IY(y1), g_palette->frame, { 0, 0, 0, 150 });
        }

        // Volume-style sliders: ten stars (FE_star_open, bright when filled, dark when empty), as the
        // pause menu's DIALOGUE / MUSIC / EFFECTS; the last one partly filled.
        float DrawStars(const View& v, float x, float y, int value, int maximum, const TextStyle& style)
        {
            const menu_art::Image* full = menu_art::GetArt(menu_art::Art::StarFull);
            float size = 26.0f, pitch = 27.0f;
            float filled = float(value) / float(maximum) * STARS;
            for (int i = 0; i < STARS; i++)
            {
                float sx = x + i * pitch, sy = y + (ROW_CELL - size) * 0.5f;
                int px = v.IX(sx), py = v.IY(sy), ps = int(std::lround(size * v.s));
                if (!full)
                {
                    g_canvas.Fill(px, py + ps / 3, px + ps - 2, py + ps * 2 / 3, g_palette->starEmpty);
                    if (filled > i)
                        g_canvas.Fill(px, py + ps / 3, px + int((ps - 2) * std::min(1.0f, filled - i)), py + ps * 2 / 3, style.fill);
                    continue;
                }
                // Empty: the same star, smaller and faint (FE_star_closed is too faint to see).
                int emptySize = int(std::lround(ps * 0.7f));
                g_canvas.Blit(menu_art::Scaled(*full, emptySize, emptySize), px + (ps - emptySize) / 2, py + (ps - emptySize) / 2,
                              g_palette->starEmpty);
                if (filled > i)
                {
                    // Filled: in the text's outline colour, a little bigger, then in its fill colour
                    // (the star's white middle takes the colour, its grey rim a darker shade).
                    int clip = px + int(std::lround(ps * std::min(1.0f, filled - i)));
                    int rim = std::max(1, int(std::lround(1.5f * v.s)));
                    g_canvas.Blit(menu_art::Scaled(*full, ps + rim * 2, ps + rim * 2), px - rim, py - rim, style.outline, clip + rim);
                    g_canvas.Blit(menu_art::Scaled(*full, ps, ps), px, py, style.fill, clip);
                }
            }
            return x + STARS * pitch;
        }

        // Footer: the restart marker's legend; (B) BACK and SELECT (A) are the options screen's own
        // hints (drawn by the game), unless the menu is drawn dimmed.
        void DrawFooter(const View& v, const Palette& pal)
        {
            // The mouse: B BACK and SELECT A act as the buttons (the game's hints are at x 232-375
            // and 873-1040 at 720p; in Portuguese VOLTAR ends at ~410 and SELECIONAR starts at ~786).
            constexpr float HINT_T = FOOTER_Y - 10.0f, HINT_B = FOOTER_Y + FOOTER_CELL + 14.0f;
            if (pal.dim)
            {
                float end = Draw(v, FontId::Small, std::string("\xC2\xB2") + T({ "BACK", "VOLTAR" }), PANEL_L, FOOTER_Y, FOOTER_CELL, pal.normal);
                g_zones.push_back({ PANEL_L - 8.0f, HINT_T, end + 8.0f, HINT_B, Zone::Back, 0 });
            }
            else
            {
                g_zones.push_back({ 222.0f, HINT_T, Portuguese() ? 418.0f : 384.0f, HINT_B, Zone::Back, 0 });
                g_zones.push_back({ Portuguese() ? 778.0f : 864.0f, HINT_T, 1048.0f, HINT_B, Zone::Accept, 0 });
            }
            if (g_confirm)
                return;  // the dialog says it
            std::string note = CanRestart() ? NeedsRestart() ? T({ "THE GAME WILL RESTART", "O JOGO VAI REINICIAR" })
                                                             : T({ "RESTARTS THE GAME", "REINICIA O JOGO" })
                                            : NeedsRestart() ? T({ "RESTART THE GAME TO APPLY", "REINICIE O JOGO PARA APLICAR" })
                                                             : T({ "APPLIES ON RESTART", "VALE AO REINICIAR" });
            // Between the game's hints (B BACK ends at x ~375, SELECT A starts at ~870 at 720p; in
            // Portuguese VOLTAR ends at ~410 and SELECIONAR A starts at ~786).
            float cell = FOOTER_CELL - 8.0f, h = cell * 0.7f, iconW = menu_art::IconWidth(menu_art::ICON_ROTATE, h * v.s) / v.s;
            float center = Portuguese() ? 598.0f : NOTE_CENTER, room = Portuguese() ? 350.0f : 428.0f;
            float noteW = std::min(Width(FontId::Small, note, cell), room - iconW - 8.0f);
            float left = center - (iconW + 8.0f + noteW) * 0.5f;
            menu_art::DrawIcon(g_canvas, menu_art::ICON_ROTATE, v.X(left), v.Y(FOOTER_Y + 4.0f + (cell - h) * 0.5f), h * v.s);
            Draw(v, FontId::Small, note, left + iconW + 8.0f, FOOTER_Y + 4.0f, cell, NeedsRestart() ? pal.warning : pal.normal, 0.0f, noteW);
        }

        // "Some options only apply after a restart: ... Restart the game now?", in one of the
        // game's message boxes, where the rows are.
        void DrawRestartDialog(const View& v, const Palette& pal)
        {
            constexpr float BOX_T = 184.0f, LINE = 42.0f, CELL = 30.0f, GAP = 10.0f, MAX_W = 1180.0f;
            bool warn = !g_fromMainMenu;
            std::vector<std::pair<std::string, const TextStyle*>> lines = {
                { T({ "Some options only apply after a restart:", "Algumas opções só valem depois de reiniciar:" }), &pal.help },
                { RestartItems(), &pal.selected },
            };
            if (warn)
                lines.push_back({ T({ "Progress that was not saved will be lost.", "O progresso não salvo será perdido." }), &pal.warning });
            lines.push_back({ g_restartAt ? T({ "Restarting...", "Reiniciando..." }) : T({ "Restart the game now?", "Reiniciar o jogo agora?" }),
                              &pal.help });
            // A / B with the game's button icons (\xC2\xB1 = A, \xC2\xB2 = B), under the question.
            std::string yes = std::string("\xC2\xB1 ") + T({ "RESTART", "REINICIAR" });
            std::string no = std::string("\xC2\xB2 ") + T({ "LATER", "DEPOIS" });
            float gap = 70.0f, yesW = Width(FontId::Small, yes, CELL), noW = Width(FontId::Small, no, CELL);
            // The box fits the lines, clear of its frame's border and curls (menu_art::FrameSideInset).
            float contentW = g_restartAt ? 0.0f : yesW + gap + noW;
            for (const auto& [text, style] : lines)
                contentW = std::max(contentW, Width(FontId::Small, text, CELL));
            int count = int(lines.size()) + (g_restartAt ? 0 : 1);
            float boxW, boxH;
            menu_art::FrameOuterSize(contentW, LINE * (count - 1) + CELL, GAP, boxW, boxH);
            boxW = std::min(boxW, MAX_W);
            float boxL = (VIRTUAL_W - boxW) * 0.5f, boxR = boxL + boxW, boxB = BOX_T + boxH;
            DrawFrame(v, boxL, BOX_T, boxR, boxB);
            float y = BOX_T + menu_art::FrameTopInset(BOX_T, boxB) + GAP, cx = VIRTUAL_W * 0.5f;
            for (const auto& [text, style] : lines)
            {
                float maxW = boxW - 2.0f * (menu_art::FrameSideInset(boxL, BOX_T, boxR, boxB, y, y + CELL) + GAP);
                Draw(v, FontId::Small, text, cx, y, CELL, *style, 0.5f, maxW);
                y += LINE;
            }
            if (g_restartAt)
                return;
            float left = cx - (yesW + gap + noW) * 0.5f;
            Draw(v, FontId::Small, yes, left, y, CELL, pal.selected);
            Draw(v, FontId::Small, no, left + yesW + gap, y, CELL, pal.normal);
            g_zones.push_back({ left - 12.0f, y - 8.0f, left + yesW + 12.0f, y + CELL + 8.0f, Zone::Yes, 0 });
            g_zones.push_back({ left + yesW + gap - 12.0f, y - 8.0f, left + yesW + gap + noW + 12.0f, y + CELL + 8.0f, Zone::No, 0 });
        }

        void Rasterize(int width, int height)
        {
            View v;
            v.s = std::min(width / VIRTUAL_W, height / VIRTUAL_H);
            v.ox = (width - VIRTUAL_W * v.s) * 0.5f, v.oy = (height - VIRTUAL_H * v.s) * 0.5f;
            g_canvas.Reset(width, height);
            g_zones.clear();
            const Palette& pal = *g_palette;
            if (pal.dim)
            {
                // In a game it hides the game's own options screen behind (opaque).
                g_canvas.FillGradient(0, 0, width, height, InGame() ? DIM_TOP_IN_GAME : DIM_TOP, InGame() ? DIM_BOTTOM_IN_GAME : DIM_BOTTOM);
                DrawSwirls(v, width, height);
            }

            // Title, between the frontend's name dividers.
            std::string title = T({ "SETTINGS", "CONFIGURAÇÕES" });
            float titleW = Width(FontId::Large, title, TITLE_CELL);
            Draw(v, FontId::Large, title, VIRTUAL_W * 0.5f, TITLE_Y, TITLE_CELL, pal.title, 0.5f);
            const menu_art::Image* dividerL = menu_art::GetArt(menu_art::Art::DividerLeft);
            const menu_art::Image* dividerR = menu_art::GetArt(menu_art::Art::DividerRight);
            if (dividerL && dividerR)
            {
                float h = 34.0f, gap = 16.0f, cy = TITLE_Y + TITLE_CELL * 0.5f;
                float wl = h * dividerL->w / dividerL->h, wr = h * dividerR->w / dividerR->h;
                int ph = int(std::lround(h * v.s));
                g_canvas.Blit(menu_art::Scaled(*dividerL, int(std::lround(wl * v.s)), ph),
                              v.IX(VIRTUAL_W * 0.5f - titleW * 0.5f - gap - wl), v.IY(cy - h * 0.5f), pal.divider);
                g_canvas.Blit(menu_art::Scaled(*dividerR, int(std::lround(wr * v.s)), ph), v.IX(VIRTUAL_W * 0.5f + titleW * 0.5f + gap),
                              v.IY(cy - h * 0.5f), pal.divider);
            }

            // Tabs: LB < GAME  AUDIO  VIDEO ... > RB, the current one highlighted.
            // Narrower gaps when the names would push LB / RB off the screen, then smaller letters
            // (the names, at least 16 apart, keep within the room LB / RB leave, 24 from the edges).
            float icon = 46.0f, iconY = TABS_Y + (TAB_CELL - icon) * 0.5f;
            float lbW = menu_art::IconWidth(menu_art::ICON_LB, icon * v.s) / v.s;
            float rbW = menu_art::IconWidth(menu_art::ICON_RB, icon * v.s) / v.s;
            constexpr float MIN_TAB_GAP = 16.0f;
            const float TABS_ROOM = std::min(1060.0f, VIRTUAL_W - 2.0f * (24.0f + 22.0f + std::max(lbW, rbW)));
            float tabCell = TAB_CELL, total = 0.0f;
            for (int i = 0; i < TAB_COUNT; i++)
                total += Width(FontId::Large, T(TABS[i].name), TAB_CELL);
            float namesRoom = TABS_ROOM - MIN_TAB_GAP * (TAB_COUNT - 1);
            if (total > namesRoom)
                tabCell *= namesRoom / total, total = namesRoom;
            float gap = std::clamp((TABS_ROOM - total) / float(TAB_COUNT - 1), MIN_TAB_GAP, TAB_GAP);
            total += gap * (TAB_COUNT - 1);
            float x = (VIRTUAL_W - total) * 0.5f;
            menu_art::DrawIcon(g_canvas, menu_art::ICON_LB, v.X(x - 22.0f - lbW), v.Y(iconY), icon * v.s);
            g_zones.push_back({ x - 22.0f - lbW - 6.0f, iconY - 4.0f, x - 16.0f, iconY + icon + 4.0f, Zone::TabPrev, 0 });
            for (int i = 0; i < TAB_COUNT; i++)
            {
                TextStyle style = i == g_tab ? pal.selected : pal.normal;
                if (i != g_tab)
                    style.opacity = 0.8f;
                float start = x;
                x = Draw(v, FontId::Large, T(TABS[i].name), x, TABS_Y + (TAB_CELL - tabCell) * 0.5f, tabCell, style) + gap;
                g_zones.push_back({ start - gap * 0.5f, TABS_Y - 8.0f, x - gap * 0.5f, TABS_Y + TAB_CELL + 8.0f, Zone::Tab, i });
            }
            menu_art::DrawIcon(g_canvas, menu_art::ICON_RB, v.X(x - gap + 22.0f), v.Y(iconY), icon * v.s);
            g_zones.push_back({ x - gap + 16.0f, iconY - 4.0f, x - gap + 22.0f + rbW + 6.0f, iconY + icon + 4.0f, Zone::TabNext, 0 });
            // A thin line under the tabs, fading at the ends.
            for (int i = 0; i < 16; i++)
            {
                float a = 1.0f - std::abs(i - 7.5f) / 8.0f;
                float x0 = PANEL_L + (PANEL_R - PANEL_L) * i / 16.0f, x1 = PANEL_L + (PANEL_R - PANEL_L) * (i + 1) / 16.0f;
                g_canvas.Fill(v.IX(x0), v.IY(127.0f), v.IX(x1), v.IY(127.0f) + std::max(1, int(std::lround(2.0f * v.s))),
                              { pal.line.r, pal.line.g, pal.line.b, uint8_t(pal.line.a * a) });
            }

            if (g_confirm)
            {
                DrawRestartDialog(v, pal);
                DrawFooter(v, pal);
                return;
            }

            // Rows: LABEL (right-aligned)  [rotate icon]  < VALUE > / stars.
            const Tab& tab = TABS[g_tab];
            // Rows closer together when a tab has more than fit at the full pitch (GRAPHICS: 9).
            // Long tabs (CONTROLS) show MAX_ROWS rows and scroll only to keep the selection in view
            // (the rows stay under the mouse while it points at them).
            size_t shown = std::min(tab.items.size(), MAX_ROWS), first = 0;
            if (tab.items.size() > shown)
            {
                int& top = g_first[g_tab];
                top = std::clamp(top, g_row[g_tab] - int(shown) + 1, g_row[g_tab]);
                top = std::clamp(top, 0, int(tab.items.size() - shown));
                first = size_t(top);
            }
            float pitch = std::min(ROW_PITCH, (ROWS_BOTTOM - ROWS_TOP) / float(shown));
            float rowsY = ROWS_TOP + (ROWS_BOTTOM - ROWS_TOP - shown * pitch) * 0.5f;
            float arrow = ROW_CELL * 0.62f;
            float arrowW = menu_art::IconWidth(menu_art::ICON_RIGHT, arrow * v.s) / v.s;
            for (size_t row = first; row < first + shown; row++)
            {
                Item item = tab.items[row];
                bool selected = int(row) == g_row[g_tab];
                float y = rowsY + (row - first) * pitch;
                if (IsHeader(item))
                {
                    // A sub-group (AUDIO > SUBTITLES, DEFINITIVE EDITION > CAMERA): its name centered
                    // in the title colour, smaller, between lines that fade outwards.
                    float cell = ROW_CELL * 0.8f, cy = y + ROW_CELL * 0.5f, textW = Width(FontId::Small, Label(item), cell);
                    Draw(v, FontId::Small, Label(item), VIRTUAL_W * 0.5f, y + (ROW_CELL - cell) * 0.5f, cell, pal.title, 0.5f);
                    int thick = std::max(1, int(std::lround(2.0f * v.s)));
                    float inner = textW * 0.5f + 18.0f, outer = (PANEL_R - PANEL_L) * 0.5f - 60.0f;
                    for (int i = 0; i < 8; i++)
                    {
                        float a = 1.0f - i / 8.0f;
                        float d0 = inner + (outer - inner) * i / 8.0f, d1 = inner + (outer - inner) * (i + 1) / 8.0f;
                        Color c{ pal.line.r, pal.line.g, pal.line.b, uint8_t(std::min(255.0f, pal.line.a * 2.0f) * a) };
                        g_canvas.Fill(v.IX(VIRTUAL_W * 0.5f - d1), v.IY(cy), v.IX(VIRTUAL_W * 0.5f - d0), v.IY(cy) + thick, c);
                        g_canvas.Fill(v.IX(VIRTUAL_W * 0.5f + d0), v.IY(cy), v.IX(VIRTUAL_W * 0.5f + d1), v.IY(cy) + thick, c);
                    }
                    continue;
                }
                bool unavailable = item == Item::HdRemaster && !HdRemasterAvailable();
                const TextStyle& style = !Selectable(item) || (unavailable && !selected) ? pal.info : selected ? pal.selected : pal.normal;
                float h = ROW_CELL * 0.6f, restartW = menu_art::IconWidth(menu_art::ICON_ROTATE, h * v.s) / v.s;
                float labelStart = LABEL_RIGHT - Width(FontId::Small, Label(item), ROW_CELL);
                Draw(v, FontId::Small, Label(item), LABEL_RIGHT, y, ROW_CELL, style, 1.0f, LABEL_RIGHT - PANEL_L - restartW - 10.0f);
                if (RestartOnly(item))  // before the label: the value column keeps the arrows
                    menu_art::DrawIcon(g_canvas, menu_art::ICON_ROTATE, v.X(std::max(labelStart, PANEL_L + restartW + 10.0f) - 10.0f - restartW),
                                       v.Y(y + (ROW_CELL - h) * 0.5f), h * v.s, selected ? 1.0f : 0.8f);
                float valueX = VALUE_X + arrowW + 8.0f;
                int minimum, maximum;
                float rowT = y - (pitch - ROW_CELL) * 0.5f, rowB = rowT + pitch;
                if (int* percent = Percent(item, &minimum, &maximum))
                {
                    float end = DrawStars(v, valueX, y, *percent, maximum, style);
                    Draw(v, FontId::Small, Value(item), end + 10.0f, y, ROW_CELL, style);
                    g_zones.push_back({ valueX - 14.0f, rowT, end + 4.0f, rowB, Zone::Stars, int(row) });
                    g_zones.push_back({ PANEL_L, rowT, PANEL_R, rowB, Zone::Row, int(row) });
                    continue;
                }
                // A value this PC can't run (DLSS without an NVIDIA RTX GPU) is greyed out.
                const TextStyle& valueStyle = options::Unavailable(item, g_values, OptionContext()) ? pal.info : style;
                float end = Draw(v, FontId::Small, Value(item), valueX, y, ROW_CELL, valueStyle, 0.0f, PANEL_R - valueX - arrowW - 10.0f);
                if (Selectable(item) && !unavailable && !IsBinding(item))
                {
                    // Where the arrows are (drawn on the selected row only); clicking them on any
                    // row selects it and changes it.
                    g_zones.push_back({ VALUE_X - 10.0f, rowT, VALUE_X + arrowW + 4.0f, rowB, Zone::ArrowLeft, int(row) });
                    g_zones.push_back({ end + 4.0f, rowT, end + 8.0f + arrowW + 10.0f, rowB, Zone::ArrowRight, int(row) });
                }
                if (selected && Selectable(item) && !unavailable)
                {
                    float ay = y + (ROW_CELL - arrow) * 0.5f;
                    // The font's left arrow is only an outline: the solid right one, mirrored.
                    menu_art::DrawIcon(g_canvas, menu_art::ICON_RIGHT, v.X(VALUE_X), v.Y(ay), arrow * v.s, 1.0f, true);
                    menu_art::DrawIcon(g_canvas, menu_art::ICON_RIGHT, v.X(end + 8.0f), v.Y(ay), arrow * v.s);
                }
                // The whole row last: the arrows and the stars win over it (ZoneAt takes the first).
                g_zones.push_back({ PANEL_L, rowT, PANEL_R, rowB, Zone::Row, int(row) });
            }

            // Help, in one of the game's message boxes.
            DrawFrame(v, PANEL_L, HELP_Y, PANEL_R, HELP_Y + HELP_H);
            auto [help1, help2] = Help(SelectedItem());
            // Clear of the frame's curls (menu_art::FrameSideInset).
            float helpMax = PANEL_R - PANEL_L - 2.0f * (menu_art::FrameSideInset(PANEL_L, HELP_Y, PANEL_R, HELP_Y + HELP_H, HELP_Y + 12.0f, HELP_Y + 12.0f + 2.0f * HELP_CELL + 4.0f) + 10.0f);
            Draw(v, FontId::Small, help1, VIRTUAL_W * 0.5f, HELP_Y + 12.0f, HELP_CELL, pal.help, 0.5f, helpMax);
            Draw(v, FontId::Small, help2, VIRTUAL_W * 0.5f, HELP_Y + 12.0f + HELP_CELL + 4.0f, HELP_CELL, pal.help, 0.5f, helpMax);

            DrawFooter(v, pal);
        }

        // Behaviour ---------------------------------------------------------------------------------

        void Apply(const Values& before)
        {
            Set(g_values);
            if (g_values.window != before.window)
                gpu::window::SetMode(g_values.window);
            if (g_values.overlay != before.overlay)
                gpu::overlay::SetMode(g_values.overlay);
            if (g_values.fpsLimit != before.fpsLimit)
                gpu::SetFrameLimit(uint32_t(g_values.fpsLimit));
            if (g_values.musicVolume != before.musicVolume || g_values.effectsVolume != before.effectsVolume ||
                g_values.voiceVolume != before.voiceVolume)
                audio::volumes::SetPercent(g_values.musicVolume, g_values.effectsVolume, g_values.voiceVolume);
            // The renderer reads the graphics options (anti-aliasing, motion blur, ...) every frame.
        }

        void Change(int direction)
        {
            Item item = SelectedItem();
            Values before = g_values;
            if (item == Item::Player1 || item == Item::Player2)
            {
                // AUTO, the keyboard, then the controllers connected now (and the saved one, if it is not).
                std::string& chosen = g_values.controls.players[item == Item::Player2];
                std::vector<std::string> keys = { "auto", "keyboard" };
                for (const input::slots::Device& device : input::slots::Devices())
                    keys.push_back(device.key);
                if (std::find(keys.begin(), keys.end(), chosen) == keys.end())
                    keys.push_back(chosen);
                size_t index = size_t(std::find(keys.begin(), keys.end(), chosen) - keys.begin());
                chosen = keys[(index + keys.size() + direction) % keys.size()];
            }
            else if (!options::Change(item, g_values, direction, OptionContext()))
                return;
            if (!(g_values == before))
            {
                Apply(before);
                Log("[settings] %s -> %s", Key(item), Value(item).c_str());
            }
        }

        // `key` becomes the first key of `action` (the old first one moves to second), and leaves
        // the other actions that had it.
        void Bind(Action action, uint16_t key)
        {
            Values before = g_values;
            for (size_t a = 0; a < ACTION_COUNT; a++)
            {
                auto& keys = g_values.controls.keys[a];
                if (a == size_t(action))
                    continue;
                for (size_t k = 0; k < KEYS_PER_ACTION; k++)
                {
                    if (keys[k] == key)
                        keys[k] = 0;
                }
                if (!keys[0] && keys[1])
                    keys[0] = keys[1], keys[1] = 0;
            }
            auto& keys = g_values.controls.keys[size_t(action)];
            if (keys[0] != key)
                keys[1] = keys[0], keys[0] = key;
            Apply(before);
            Log("[settings] %s -> %s", ActionId(action), Value(Item(int(Item::Bind0) + int(action))).c_str());
        }

        void Move(int direction)
        {
            const std::vector<Item>& items = TABS[g_tab].items;
            int count = int(items.size()), next = g_row[g_tab];
            for (int i = 0; i < count; i++)
            {
                next = (next + count + direction) % count;
                if (Selectable(items[size_t(next)]))
                    break;
            }
            g_row[g_tab] = next;
        }

        void SwitchTab(int direction)
        {
            g_tab = (g_tab + TAB_COUNT + direction) % TAB_COUNT;
            Log("[settings] tab -> %s", TABS[g_tab].id);
        }

        // `leaveScreen`: press B for the game so that it leaves the screen the settings are drawn
        // over (the options screen it went to); false when the game already left it.
        void Close(bool leaveScreen = true)
        {
            if (g_capturing)
            {
                input::pc::CancelKeyCapture();
                g_capturing = false;
            }
            Set(g_values);
            Save();
            // Resolution and upscaler: the renderer rebuilds its targets after this frame.
            if (RendererDiffers(g_values))
            {
                Log("[settings] renderer options changed: applying them now");
                gpu::renderer::RequestReconfigure();
            }
            if (NeedsRestart())
                Log("[settings] %s apply on the next launch", RestartItems().c_str());
            g_confirm = false;
            g_restartAt = 0;
            g_open = false;
            g_releasing = true;
            g_closing = true;
            g_closedAt = plat::TickCountMs();
            // The game leaves the options screen with its own transition (ExitBack): the PS2
            // options screen of the main menu's SETTINGS, or the in-game options.
            if (leaveScreen && !g_overGame)
                g_backUntil = g_closedAt + BACK_PRESS_MS;
            Log("[settings] screen closed%s", leaveScreen ? "" : " (the game left its options screen)");
        }

        // Buttons that went down this frame, plus auto-repeat of a held direction.
        uint16_t Pressed(uint16_t buttons)
        {
            uint64_t now = plat::TickCountMs();
            uint16_t pressed = buttons & ~g_previousButtons;
            constexpr uint16_t DIRECTIONS = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
            if (pressed & DIRECTIONS)
            {
                g_heldButton = pressed & DIRECTIONS;
                g_heldSince = g_lastRepeat = now;
            }
            else if (g_heldButton && (buttons & g_heldButton) == g_heldButton)
            {
                if (now - g_heldSince >= REPEAT_DELAY_MS && now - g_lastRepeat >= REPEAT_MS)
                {
                    pressed |= g_heldButton;
                    g_lastRepeat = now;
                }
            }
            else
            {
                g_heldButton = 0;
            }
            g_previousButtons = buttons;
            return pressed;
        }

        const HitZone* ZoneAt(float x, float y)
        {
            for (const HitZone& zone : g_zones)
            {
                if (x >= zone.x0 && x < zone.x1 && y >= zone.y0 && y < zone.y1)
                    return &zone;
            }
            return nullptr;
        }

        // The stars of the selected row set from the cursor's x (a click or a drag on them).
        void SetStars(const HitZone& zone, float x)
        {
            Item item = SelectedItem();
            int minimum, maximum, step;
            int* percent = Percent(item, &minimum, &maximum, &step);
            if (!percent || step <= 0)
                return;
            float x0 = zone.x0 + 14.0f, x1 = zone.x1 - 4.0f;  // the stars themselves (see the zone)
            // Whole stars, as drawn: a point on the n-th star fills it and the ones before (left of
            // the first one = the minimum).
            float t = std::clamp((x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
            int stars = int(std::ceil(t * STARS - 0.05f));
            int value = std::clamp(int(std::lround(float(stars) / STARS * maximum / step)) * step, minimum, maximum);
            if (value == *percent)
                return;
            Values before = g_values;
            *percent = value;
            Apply(before);
            g_dirty = true;
            Log("[settings] %s -> %s (mouse)", Key(item), Value(item).c_str());
        }

        // The mouse on the screen, like a PC game's menu: pointing at a row selects it, a click
        // on a tab opens it, on LB / RB steps the tabs, on the arrows changes the value, on the
        // stars sets it (dragging too), on a row is A (changes it, or waits for a key in CONTROLS),
        // on (B) BACK / SELECT (A) and the restart question's answers is that button; the wheel
        // moves between rows. Returns the buttons the clicks stand for (as presses this frame).
        uint16_t Mouse()
        {
            uint32_t moves = input::pc::CursorMoves(), clicks = input::pc::LeftClicks();
            bool moved = moves != g_mouseMoves, clicked = clicks != g_mouseClicks;
            g_mouseMoves = moves, g_mouseClicks = clicks;
            int wheel = input::pc::TakeSettingsWheel();
            input::pc::TakeMenuClick();  // a click made here never reaches the game's menus
            bool held = input::pc::LeftHeld();
            if (!held)
                g_dragRow = -1;
            float u, v;
            if (g_restartAt || g_capturing || !g_values.controls.mouseMenus || g_drawnW <= 0 || g_drawnH <= 0 || !input::pc::CursorInClient(u, v))
                return 0;
            float s = std::min(g_drawnW / VIRTUAL_W, g_drawnH / VIRTUAL_H);
            float x = (u * g_drawnW - (g_drawnW - VIRTUAL_W * s) * 0.5f) / s, y = (v * g_drawnH - (g_drawnH - VIRTUAL_H * s) * 0.5f) / s;
            const HitZone* zone = ZoneAt(x, y);
            const std::vector<Item>& items = TABS[g_tab].items;
            auto selectRow = [&](int row) {
                if (row < 0 || row >= int(items.size()) || row == g_row[g_tab] || !Selectable(items[size_t(row)]))
                    return;
                g_row[g_tab] = row;
                g_dirty = true;
                Log("[settings] mouse: row -> %s", Key(SelectedItem()));
            };
            bool onRow = zone && !g_confirm &&
                         (zone->zone == Zone::Row || zone->zone == Zone::ArrowLeft || zone->zone == Zone::ArrowRight || zone->zone == Zone::Stars);
            if (moved && onRow && g_dragRow < 0)
                selectRow(zone->index);
            if (moved && held && g_dragRow >= 0 && g_dragRow == g_row[g_tab])
            {
                for (const HitZone& stars : g_zones)
                {
                    if (stars.zone == Zone::Stars && stars.index == g_dragRow)
                        SetStars(stars, x);
                }
            }
            uint16_t pressed = 0;
            if (clicked && zone)
            {
                bool rowOk = onRow && Selectable(items[size_t(zone->index)]);
                switch (zone->zone)
                {
                case Zone::Tab:
                    if (!g_confirm && zone->index != g_tab)
                    {
                        g_tab = zone->index;
                        g_dirty = true;
                        Log("[settings] tab -> %s (mouse)", TABS[g_tab].id);
                    }
                    break;
                case Zone::TabPrev: if (!g_confirm) pressed |= XINPUT_GAMEPAD_LEFT_SHOULDER; break;
                case Zone::TabNext: if (!g_confirm) pressed |= XINPUT_GAMEPAD_RIGHT_SHOULDER; break;
                case Zone::Row: if (rowOk) selectRow(zone->index), pressed |= XINPUT_GAMEPAD_A; break;
                case Zone::ArrowLeft: if (rowOk) selectRow(zone->index), pressed |= XINPUT_GAMEPAD_DPAD_LEFT; break;
                case Zone::ArrowRight: if (rowOk) selectRow(zone->index), pressed |= XINPUT_GAMEPAD_DPAD_RIGHT; break;
                case Zone::Stars:
                    if (rowOk)
                    {
                        selectRow(zone->index);
                        g_dragRow = zone->index;
                        SetStars(*zone, x);
                    }
                    break;
                case Zone::Back: case Zone::No: pressed |= XINPUT_GAMEPAD_B; break;
                case Zone::Accept: case Zone::Yes: pressed |= XINPUT_GAMEPAD_A; break;
                }
            }
            if (wheel && !g_confirm)
            {
                for (int i = 0; i < std::min(std::abs(wheel), 8); i++)
                    Move(wheel > 0 ? -1 : 1);  // wheel up = the row above
                g_dirty = true;
                Log("[settings] mouse: wheel %+d -> %s", wheel, Key(SelectedItem()));
            }
            return pressed;
        }

        void Reset()
        {
            g_tab = 0;
            g_dragRow = -1;
            g_mouseMoves = input::pc::CursorMoves(), g_mouseClicks = input::pc::LeftClicks();
            input::pc::TakeSettingsWheel();
            for (int tab = 0; tab < TAB_COUNT; tab++)
            {
                const std::vector<Item>& items = TABS[tab].items;
                g_row[tab] = 0;
                g_first[tab] = 0;
                while (g_row[tab] + 1 < int(items.size()) && !Selectable(items[size_t(g_row[tab])]))
                    g_row[tab]++;
            }
            g_confirm = false;
            g_restartAt = 0;
            g_dirty = true;
        }

        // Opens/closes, reads input and applies changes; true while the screen is visible.
        bool Update()
        {
            if (!g_open && !g_releasing && !g_requested && !g_closing)
                return false;  // nothing to do: the controller is not polled every frame
            uint16_t buttons = input::MenuButtons();
            if (g_requested.exchange(false) && !g_open)
            {
                g_values = Get();
                g_values.overlay = gpu::overlay::Mode();  // F1 and Alt+Enter may have changed these
                g_values.window = gpu::window::Mode();
                Reset();
                g_previousButtons = buttons;  // the A that selected SETTINGS is not a press here
                g_heldButton = 0;
                g_open = true;
                g_closing = false;
                g_openedAt = plat::TickCountMs();
                // The art is read at startup; give it a moment if the screen opens very early.
                menu_art::Ready(2000);
                Log("[settings] screen opened%s", InGame() ? " (pause menu OPTIONS)" : "");
            }
            if (!g_open)
            {
                if (g_releasing && !buttons && plat::TickCountMs() >= g_backUntil)
                    g_releasing = false;
                g_previousButtons = buttons;
                if (g_closing && plat::TickCountMs() - g_closedAt >= FADE_OUT_MS)
                    g_closing = false;
                return g_closing;
            }
            // In a game: the game left its options screen by itself (it should not: its input is
            // blocked while the settings are open).
            if (InGame() && !g_overGame && g_frontendNode != g_optionsNode && !g_restartAt)
            {
                Close(false);
                return true;  // fading out
            }
            // Input only once the screen is shown (the game's transition first).
            if (plat::TickCountMs() - g_openedAt < OPEN_DELAY_MS)
            {
                g_previousButtons = buttons;
                return true;
            }

            uint16_t pressed = Pressed(buttons) | Mouse();
            if (pressed)
                g_dirty = true;

            if (g_restartAt)
            {
                // "Restarting..." was drawn: restart (returns only if it failed).
                if (plat::TickCountMs() >= g_restartAt)
                {
                    Set(g_values);
                    Restart();
                    Close();
                }
                return true;
            }
            if (g_confirm)
            {
                if (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START))
                {
                    Log("[settings] restart accepted (%s)", RestartItems().c_str());
                    g_restartAt = plat::TickCountMs() + RESTART_DELAY_MS;
                }
                else if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK))
                {
                    Log("[settings] restart declined");
                    Close();  // also leaves the game's options screen
                }
                return true;
            }
            // CONTROLS: the key of a binding (A / Enter), its default (X / Backspace).
            if (g_capturing)
            {
                uint16_t key;
                if (input::pc::CapturedKey(key))
                {
                    g_capturing = false;
                    g_dirty = true;
                    if (key != VK_ESCAPE)
                        Bind(ActionOf(SelectedItem()), key);
                }
                else if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK))  // a pad's B cancels too
                {
                    input::pc::CancelKeyCapture();
                    g_capturing = false;
                }
                return true;
            }
            if (IsBinding(SelectedItem()))
            {
                if (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START))
                {
                    g_capturing = true;
                    input::pc::BeginKeyCapture();
                    return true;
                }
                if (pressed & XINPUT_GAMEPAD_X)
                {
                    Values before = g_values;
                    Action action = ActionOf(SelectedItem());
                    g_values.controls.keys[size_t(action)] = Controls().keys[size_t(action)];
                    Apply(before);
                    Log("[settings] %s -> %s (default)", Key(SelectedItem()), Value(SelectedItem()).c_str());
                    return true;
                }
            }
            if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK))
            {
                if (CanRestart() && NeedsRestart())
                {
                    // Options the game reads at boot changed: ask before leaving.
                    g_confirm = true;
                    Log("[settings] restart needed for %s: asking", RestartItems().c_str());
                    return true;
                }
                Close();
                return true;  // fading out
            }
            if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER)
                SwitchTab(-1);
            if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER)
                SwitchTab(1);
            if (pressed & XINPUT_GAMEPAD_DPAD_UP)
                Move(-1);
            if (pressed & XINPUT_GAMEPAD_DPAD_DOWN)
                Move(1);
            if (pressed & XINPUT_GAMEPAD_DPAD_LEFT)
                Change(-1);
            if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT)
                Change(1);
            if ((pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START)) && !Percent(SelectedItem()))
                Change(1);
            return true;
        }

        // The screen at `width` x `height` in g_pixels (straight alpha), redrawn when something changed.
        void Render(int width, int height)
        {
            if (width != g_drawnW || height != g_drawnH)
            {
                menu_art::ClearCache();  // glyphs and images are cached at the window's scale
                g_drawnW = width, g_drawnH = height;
                g_dirty = true;
            }
            const Palette* palette = g_previewPalette ? g_previewPalette : InGame() ? &DIMMED : SettingsScreenEmptied() ? &CLOTH : &DIMMED;
            if (palette != g_palette)
                g_palette = palette, g_dirty = true;
            if (!g_dirty)
                return;
            Rasterize(width, height);
            g_canvas.Export(g_pixels);
            g_version++;
            g_dirty = false;
        }
    }

    void Init()
    {
        menu_art::StartLoading();
        rcf_overlay::RegisterBuiltinPatch("default", "fighttrees\\frontend.bfig", "no da tela OPCOES da pausa (so leitura)", FindOptionsNode);
    }

    void SetAvailable(bool available)
    {
        g_available = available;
    }

    void RequestOpen(bool fromMainMenu)
    {
        if (!g_available)
        {
            Log("[settings] the settings screen needs the renderer; ignored");
            return;
        }
        g_fromMainMenu = fromMainMenu;
        g_overGame = false;
        g_requested = true;
    }

    void RequestOpenOverGame()
    {
        if (!g_available)
            return;
        g_fromMainMenu = false;
        g_overGame = true;
        g_requested = true;
    }

    bool IsOpen()
    {
        return g_open;
    }

    uint32_t FrontendNode()
    {
        return g_frontendNode.load();
    }

    void NoteFrontendNode(uint32_t node)
    {
        static const bool trace = getenv("CRASH_FRONTEND_TRACE") != nullptr;
        uint32_t previous = g_frontendNode.exchange(node);
        if (node == previous)
            return;
        if (trace)
            Log("[settings] frontend node %u (input clock %.1f s)", node, double(input::ClockMs()) / 1000.0);
        // The pause menu's OPTIONS: the settings open over the game's options screen.
        if (node == g_optionsNode && !g_open && !g_requested)
        {
            Log("[settings] pause menu OPTIONS selected");
            RequestOpen(false);
        }
    }

    bool BlocksGameInput()
    {
        return g_open || g_releasing || g_requested;
    }

    uint16_t GameButtons()
    {
        return (plat::TickCountMs() < g_backUntil ? XINPUT_GAMEPAD_B : 0) | MenuAutopilotButtons() | boot::Buttons();
    }

    void Draw(uint32_t width, uint32_t height)
    {
        if (!Update() || !width || !height)
            return;
        Render(int(width), int(height));
        // Fade (and rise) in after the game's transition; fade out when closing.
        uint64_t now = plat::TickCountMs();
        float t = g_closing ? 1.0f - float(now - g_closedAt) / FADE_OUT_MS
                            : (float(now - g_openedAt) - OPEN_DELAY_MS) / FADE_IN_MS;
        t = std::clamp(t, 0.0f, 1.0f);
        if (t <= 0.0f)
            return;
        float eased = 1.0f - (1.0f - t) * (1.0f - t);
        int rise = g_closing ? 0 : int(std::lround((1.0f - eased) * RISE * std::min(width / VIRTUAL_W, height / VIRTUAL_H)));
        const uint32_t* pixels = g_pixels.data();
        uint64_t version = g_version;
        if (t < 1.0f)
        {
            // A copy at the fade's opacity (the presenter's shader has no opacity of its own).
            uint32_t k = uint32_t(std::lround(eased * 256.0f));
            g_faded.resize(g_pixels.size());
            for (size_t i = 0; i < g_pixels.size(); i++)
                g_faded[i] = (g_pixels[i] & 0x00FFFFFFu) | (((g_pixels[i] >> 24) * k >> 8) << 24);
            pixels = g_faded.data();
            version = (1ull << 63) | ++g_fadedVersion;
        }
        gpu::overlay::DrawBitmap(width, height, pixels, int(width), int(height), 0, rise, version,
                                 gpu::overlay::BITMAP_SETTINGS);
    }

    int Preview(const std::filesystem::path& directory)
    {
        if (!menu_art::Ready(10000))
            Log("[settings] preview: the game's fonts are missing; drawing with the overlay font");
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const Values saved = Get();
        g_previewPalette = &CLOTH;
        struct Size
        {
            int w, h;
        };
        int written = 0;
        for (const char* language : { "english", "pt-br" })
        {
            g_values = saved;
            g_values.language = language;
            if (LanguageById(language).id != std::string(language))
                continue;  // no PT-BR pack
            for (Size size : { Size{ 1280, 720 }, Size{ 1920, 1080 }, Size{ 3840, 2160 }, Size{ 2560, 1080 } })
            {
                // Each tab with its second row selected; then AUDIO with the SUBTITLES row, DEFINITIVE
                // EDITION with the FREE CAMERA row and with the lighting rows (all on), the restart
                // dialog (mojo colours changed) and,
                // as opened from the pause menu, GAME with the game's own options (CO-OP selected).
                enum { LEGENDAS = TAB_COUNT, CAMERA, ILUMINACAO, REINICIAR, PAUSA };
                auto rowOf = [](int tab, Item item) {
                    const std::vector<Item>& items = TABS[tab].items;
                    return int(std::find(items.begin(), items.end(), item) - items.begin());
                };
                for (int shot = 0; shot <= PAUSA; shot++)
                {
                    int tab = shot < TAB_COUNT ? shot : shot == LEGENDAS ? 1 : shot == PAUSA ? GAME_TAB : DEFINITIVE_TAB;
                    bool inGame = shot == PAUSA;
                    g_fromMainMenu = !inGame;
                    g_previewPalette = inGame ? &DIMMED : &CLOTH;
                    Reset();
                    g_tab = tab;
                    if (shot < TAB_COUNT)
                        g_row[tab] = std::min<int>(1, int(TABS[tab].items.size()) - 1);
                    else if (shot == LEGENDAS)
                        g_row[tab] = rowOf(tab, Item::Subtitles);
                    else if (shot == CAMERA)
                        g_row[tab] = rowOf(tab, Item::FreeCamera);
                    else if (shot == ILUMINACAO)
                        g_row[tab] = rowOf(tab, Item::LightSsao);
                    else if (shot == PAUSA)
                        g_row[tab] = rowOf(tab, Item::CoopMode);
                    Values kept = g_values;
                    if (shot == ILUMINACAO)
                    {
                        g_values.ssao = AoQuality::High;
                        g_values.gi = g_values.contactShadows = g_values.softShadows = true;
                    }
                    if (shot == REINICIAR)
                    {
                        g_confirm = true;
                        g_values.ps2MojoColors = !AtStartup().ps2MojoColors;
                    }
                    Render(size.w, size.h);
                    g_values = kept;
                    // Over a flat colour like the main menu's cloth (a dark green, like a level, for
                    // the pause menu), so the PNG shows what the window does.
                    uint32_t back[3] = { 205, 140, 20 };
                    if (inGame)
                        back[0] = 30, back[1] = 70, back[2] = 40;
                    std::vector<uint32_t> image(g_pixels.size());
                    for (size_t i = 0; i < image.size(); i++)
                    {
                        uint32_t p = g_pixels[i], a = p >> 24;
                        auto mix = [&](uint32_t c, uint32_t b) { return (c * a + b * (255 - a) + 127) / 255; };
                        image[i] = mix(p & 0xFF, back[0]) | (mix((p >> 8) & 0xFF, back[1]) << 8) | (mix((p >> 16) & 0xFF, back[2]) << 16) | (255u << 24);
                    }
                    char name[96];
                    snprintf(name, sizeof(name), "%s_%dx%d_%d_%s%s.png", language, size.w, size.h, tab + 1, TABS[tab].id,
                             shot < TAB_COUNT     ? ""
                             : shot == LEGENDAS   ? "_legendas"
                             : shot == CAMERA     ? "_camera"
                             : shot == ILUMINACAO ? "_iluminacao"
                             : shot == REINICIAR  ? "_reiniciar"
                                                  : "_pausa");
                    if (gpu::WritePng(directory / name, reinterpret_cast<const uint8_t*>(image.data()), uint32_t(size.w), uint32_t(size.h),
                                      uint32_t(size.w) * 4))
                        written++;
                }
            }
        }
        g_fromMainMenu = true;
        Log("[settings] preview: %d images in %s", written, directory.string().c_str());
        printf("settings preview: %d images in %s\n", written, directory.string().c_str());
        return written ? 0 : 1;
    }
}
