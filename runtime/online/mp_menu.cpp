// The MULTIPLAYER screens (mp_menu.h), drawn like the settings screen (settings/settings_menu.cpp):
// the game's fonts and frames (settings/menu_art.h) in a virtual 1280x720 screen scaled to the window.
// Controller, keyboard, and the mouse / touch (Android's menus are touch as a mouse): a click on a
// row chooses it, on the game's B BACK / SELECT A hints is that button, on an option's left / right
// half changes it, on the HUB panel opens the lobby; code and password have keys on the screen.

#include "mp_menu.h"
#include "mp.h"
#include "mp_colors.h"
#include "mp_lobby.h"
#include "../diagnostics.h"
#include "../dev/dev_tools.h"
#include "../gpu/overlay.h"
#include "../kernel/input.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../kernel/pc_input.h"
#include "../kernel/xinput_types.h"
#include "../platform/platform.h"
#include "../settings/keyboard.h"
#include "../settings/menu_art.h"
#include "../settings/settings.h"
#include "../settings/settings_menu.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

PPC_EXTERN_FUNC(sub_820F5D58);  // Frontend:StartLevel(index)
PPC_EXTERN_FUNC(sub_821FB4F0);  // the managers object
PPC_EXTERN_FUNC(sub_821FA440);  // managers: subsystem n (6 = CFrontendManager)
extern "C" PPC_FUNC(__imp__sub_820F8918);  // Frontend:PauseGame(paused)
extern "C" PPC_FUNC(__imp__sub_821DAAB0);  // CFrontendManager::HaveBothPlayersJoinedGame
extern "C" PPC_FUNC(__imp__sub_821DB680);  // CFrontendManager::UpdateInputAndCheats

namespace mp::menu
{
    namespace
    {
        using settings::menu_art::Color;
        using settings::menu_art::FontId;
        using settings::menu_art::TextStyle;
        namespace art = settings::menu_art;
        namespace keyboard = settings::keyboard;

        // The frontend's nodes and exits (Frontend.bfig; docs/internals/multiplayer.md).
        constexpr uint32_t NODE_MAIN_MENU = 70, NODE_OPTIONS = 99, NODE_NEW_GAME_FADE = 322, NODE_HUB = 575, NODE_IN_GAME = 630;
        constexpr int32_t EXIT_NEW_GAME = 88, EXIT_LOAD_GAME = 89, EXIT_FIRST_LEVEL = 323, EXIT_TO_HUB = 309, EXIT_SKIP_TO_LEVEL = 51;
        // Leaving a level: the pause screen (two-player menu: RESUME, OPTIONS, DROP OUT, EXIT LEVEL), its
        // EXIT LEVEL after the confirmation (save and exit -> EpisodeComplete), the level's end screens'
        // exits to the save screens, the end screen's own way to the HUB without saving, and the HUB's
        // EXIT GAME to the main menu; 91 = the main menu's SETTINGS (the MULTIPLAYER screens' way in).
        // Without a save (a client) the pause is NoSavePauseScreen, whose EXIT LEVEL goes to the HUB
        // without a save screen (1114 -> EpisodeComplete 1115 -> ExitContinue 1116 -> LoadingHUBScreen).
        constexpr uint32_t NODE_GAME_RUNNING = 638, NODE_PAUSE = 1081, NODE_LEAVING_LEVEL = 1088;
        constexpr uint32_t NODE_PAUSE_NO_SAVE = 1109, NODE_LEAVING_LEVEL_NO_SAVE = 1115;
        constexpr int32_t EXIT_DROP_OUT = 1098, EXIT_LEAVE_LEVEL = 1099, EXIT_SAVE_AND_EXIT = 1087;
        constexpr int32_t EXIT_DROP_OUT_NO_SAVE = 1123, EXIT_LEAVE_LEVEL_NO_SAVE = 1124, EXIT_EXIT_NO_SAVE = 1114;
        constexpr int32_t EXIT_CONTINUE_NO_SAVE = 1116;
        constexpr int32_t EXIT_HUB_WITHOUT_SAVING = 1179, EXIT_HUB_TO_MAIN_MENU = 598, EXIT_SETTINGS = 91;
        constexpr int32_t EXIT_PAUSE = 651;  // GameRunning's START: PauseScreen (PauseGame(true))
        // The skin screen (SkinSelectionScreen) and its exits: the title screen's ExitSpecial goes there from
        // anywhere (the frontend takes an exit of any node), ExitConfirmed (SelectSkin) / ExitBack to the HUB.
        constexpr uint32_t NODE_SKINS = 1058;
        constexpr int32_t EXIT_TO_SKINS = 67, EXIT_SKIN_CONFIRMED = 1059, EXIT_SKIN_BACK = 1061;
        constexpr uint32_t SHOWN_SKIN = 0x82497508;  // the skin screen's preview: {skin shown, its strings, template hash}
        constexpr uint32_t FRONTEND_SKIN_TO_DISPLAY = 16640;  // CFrontendManager: the skin the skin screen starts on
        constexpr int32_t SAVE_EXITS[] = { 1090, 1091, 1177, 1178, 1185, 1186, 1199, 1200 };
        constexpr uint32_t MANAGERS = 0x824A7A24, LEVEL_INDEX = 24;

        constexpr float VIRTUAL_W = 1280.0f, VIRTUAL_H = 720.0f;
        constexpr float TITLE_Y = 18.0f, TITLE_CELL = 44.0f, SUB_Y = 84.0f, SUB_CELL = 30.0f;
        constexpr float ROWS_Y = 170.0f, ROW_PITCH = 54.0f, ROW_CELL = 40.0f;
        constexpr float LIST_PITCH = 44.0f, LIST_CELL = 32.0f;
        constexpr float HELP_Y = 540.0f, HELP_CELL = 26.0f;
        constexpr uint64_t OPEN_DELAY_MS = 120, FADE_IN_MS = 220, FADE_OUT_MS = 110, BACK_PRESS_MS = 200, REDRAW_MS = 250;
        constexpr uint64_t REPEAT_DELAY_MS = 380, REPEAT_MS = 90;

        // The settings screen's colours (settings_menu.cpp): CLOTH over the emptied options screen,
        // DIMMED in the game's message frames at the HUB.
        struct Palette
        {
            TextStyle title, normal, selected, info, help, warning;
        };
        constexpr Palette CLOTH{
            { { 236, 150, 20, 255 }, { 70, 28, 0, 255 } },
            { { 214, 120, 14, 255 }, { 70, 28, 0, 255 } },
            { { 150, 218, 212, 255 }, { 30, 104, 110, 255 } },
            { { 132, 84, 26, 255 }, { 84, 42, 2, 110 } },
            { { 238, 226, 196, 255 }, { 52, 28, 4, 255 } },
            { { 232, 64, 24, 255 }, { 80, 16, 0, 255 } },
        };
        constexpr Palette DIMMED{
            { { 248, 248, 248, 255 }, { 0, 0, 0, 255 } },
            { { 232, 160, 0, 255 }, { 104, 48, 8, 255 } },
            { { 144, 216, 232, 255 }, { 24, 104, 128, 255 } },
            { { 160, 128, 80, 255 }, { 60, 36, 8, 255 } },
            { { 238, 226, 196, 255 }, { 52, 28, 4, 255 } },
            { { 255, 128, 72, 255 }, { 96, 24, 0, 255 } },
        };
        constexpr Color FRAME{ 236, 222, 190, 200 };
        constexpr Color DIM_TOP{ 14, 8, 0, 200 }, DIM_BOTTOM{ 8, 4, 0, 220 };

        enum class Screen { Root, HostMode, FindMode, Servers, Code, Password, Waiting, Message, Lobby, HostPassword, LevelMenu, Name };
        enum LevelMenuRow { ROW_RESUME, ROW_SETTINGS, ROW_QUIT, LEVEL_MENU_ROWS };  // a client's menu in a level

        // Between the threads.
        std::atomic<bool> g_requested{ false };
        std::atomic<bool> g_open{ false };       // a screen that has the controller
        std::atomic<bool> g_releasing{ false };  // closed, buttons still held
        std::atomic<uint64_t> g_backUntil{ 0 };
        std::atomic<bool> g_drawing{ false };   // the renderer calls Draw
        std::atomic<int32_t> g_forceExit{ -1 };  // host: NEW / LOAD GAME, taken from the options screen
        std::atomic<bool> g_hostNewGame{ false };  // a hosted new game goes to the HUB
        std::atomic<bool> g_started{ false };   // client: the level start was taken
        // Client: in a level the host started (until the main menu); leaving it (None, Quit: its own
        // QUIT or a closed session, to the main menu; ToLobby: the host went back to its HUB, the client
        // back to the lobby's wait).
        enum class Leaving { None, Quit, ToLobby };
        std::atomic<bool> g_clientInLevel{ false };
        std::atomic<Leaving> g_leaving{ Leaving::None };
        // Client in a level: START opens LevelMenu (the game goes on), whose QUIT leaves the level.
        std::atomic<bool> g_levelMenuRequested{ false };
        std::atomic<bool> g_quitLevel{ false };
        std::atomic<bool> g_hostReachedHub{ false };  // host: back at the main menu after it, the session closes
        std::atomic<bool> g_resumeLobby{ false };  // at the main menu: open the lobby again
        std::atomic<bool> g_resumeOpen{ false };   // the SETTINGS marker opens it at the lobby
        // SKIN AND COLOR: asked (the next frontend condition takes the skin screen), showing (from the host's
        // HUB or a client's wait), its colour box drawn; the host's lobby again at the HUB after it.
        std::atomic<bool> g_pickerRequest{ false }, g_picker{ false }, g_pickerShown{ false }, g_reopenLobby{ false };
        std::atomic<bool> g_pickerHost{ false };
        void OpenPicker(bool host);
        std::atomic<uint32_t> g_node{ UINT32_MAX };
        std::atomic<bool> g_panelShown{ false };  // the HUB panel is on screen

        // Render thread.
        Screen g_screen = Screen::Root;
        int g_row = 0;
        bool g_portuguese = false;
        std::string g_field;      // the code, password or name being typed
        size_t g_cursor = 0;
        int g_key = 0;  // the keyboard's highlighted key (settings/keyboard.h)
        std::string g_joinTarget;  // what the password is for (a code or an address)
        std::string g_message;
        Screen g_afterMessage = Screen::Root;
        uint16_t g_previousButtons = 0, g_heldButton = 0;
        uint64_t g_heldSince = 0, g_lastRepeat = 0;
        uint64_t g_openedAt = 0, g_closedAt = 0, g_lastDraw = 0;
        bool g_closing = false, g_lobbyModal = false, g_levelModal = false;
        bool g_dirty = true;
        art::Canvas g_canvas;
        std::vector<uint32_t> g_pixels, g_faded;
        uint64_t g_version = 0, g_fadedVersion = 0;
        int g_drawnW = 0, g_drawnH = 0;
        const Palette* g_palette = &CLOTH;

        // The mouse / touch: what each part of the screen does, as last drawn (virtual units).
        enum class Hit : uint8_t { Row, RowLeft, RowRight, Back, Accept, Key, Erase, Confirm, Panel, Anywhere, Look, Up, Down };
        struct HitZone
        {
            float x0, y0, x1, y1;
            Hit hit;
            int index;          // the row, or the key's letter
            bool askFirst;      // a click on a row not selected yet only selects it (kick)
        };
        std::vector<HitZone> g_zones;
        uint32_t g_mouseMoves = 0, g_mouseClicks = 0;
        std::u32string g_tapped;  // letters clicked on the screen's keys

        constexpr const char* CODE_LETTERS = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
        constexpr const char* PASSWORD_LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        constexpr size_t SHORT_CODE_LENGTH = 7, CODE_LENGTH = 10, PASSWORD_LENGTH = 12, NAME_LENGTH = 12;  // codes, names: mp_lobby.h

        const char* T(const char* en, const char* pt) { return g_portuguese ? pt : en; }

        // ---- drawing -------------------------------------------------------------------------------

        struct View
        {
            float s, ox, oy;
            float X(float x) const { return ox + x * s; }
            float Y(float y) const { return oy + y * s; }
            int IX(float x) const { return int(std::lround(X(x))); }
            int IY(float y) const { return int(std::lround(Y(y))); }
        };

        float Width(FontId font, const std::string& text, float cell) { return art::TextWidth(font, art::Utf32(text), cell); }

        void AddZone(float x0, float y0, float x1, float y1, Hit hit, int index = 0, bool askFirst = false)
        {
            g_zones.push_back({ x0, y0, x1, y1, hit, index, askFirst });
        }

        // The game's own hints at the bottom of the options screen the full screens are drawn over
        // (x 232-375 and 873-1040 at 720p; Portuguese VOLTAR ends at ~410, SELECIONAR starts at ~786).
        void AddFooterZones()
        {
            AddZone(222.0f, 610.0f, g_portuguese ? 418.0f : 384.0f, 676.0f, Hit::Back);
            AddZone(g_portuguese ? 778.0f : 864.0f, 610.0f, 1048.0f, 676.0f, Hit::Accept);
        }

        // Text at most `maxWidth` wide: a little smaller, then cut with "..." (art::FitText).
        float Draw(const View& v, FontId font, const std::string& text, float x, float y, float cell, const TextStyle& style,
                   float align = 0.0f, float maxWidth = 1e9f)
        {
            float fitted = cell;
            std::u32string u = art::FitText(font, art::Utf32(text), fitted, std::max(maxWidth, 1.0f));
            y += (cell - fitted) * 0.5f;
            float width = art::TextWidth(font, u, fitted);
            float left = x - width * align;
            art::DrawGameText(g_canvas, font, u, v.X(left), v.Y(y), fitted * v.s, style);
            return left + width;
        }

        // Words of `text` in lines no wider than `width`.
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

        // MSG_Background mirrored into the four corners (as the settings' dialogs).
        void DrawFrame(const View& v, float x0, float y0, float x1, float y1)
        {
            art::DrawMessageFrame(g_canvas, v.IX(x0), v.IY(y0), v.IX(x1), v.IY(y1), FRAME, { 0, 0, 0, 170 });
        }

        // A message frame sized by what it holds, and the room for a line in it: clear of the border
        // and of its curls, which grow with the frame's height and reach far in over most of it
        // (art::FrameSideInset). Every framed screen lays its lines out with Top / Left / Right.
        constexpr float BOX_GAP = 10.0f;
        struct Box
        {
            float x0, y0, x1, y1;
            float Top() const { return y0 + art::FrameTopInset(y0, y1) + BOX_GAP; }
            float Inset(float y, float h) const { return art::FrameSideInset(x0, y0, x1, y1, y, y + h) + BOX_GAP; }
            float Left(float y, float h) const { return x0 + Inset(y, h); }
            float Right(float y, float h) const { return x1 - Inset(y, h); }
            float Room(float y, float h) const { return Right(y, h) - Left(y, h); }
        };

        // The box for contentW x contentH: `align` 0.5 centres it on x, 1 puts its right side there;
        // its top at `top`, or centred on the screen when top < 0.
        Box FitBox(float contentW, float contentH, float x, float align, float top)
        {
            float w, h;
            art::FrameOuterSize(contentW, contentH, BOX_GAP, w, h);
            w = std::min(w, VIRTUAL_W - 16.0f), h = std::min(h, VIRTUAL_H - 8.0f);
            Box box;
            box.x0 = std::round(std::clamp(x - w * align, 8.0f, VIRTUAL_W - 8.0f - w)), box.x1 = box.x0 + w;
            box.y0 = std::round(top >= 0.0f ? top : std::max(4.0f, (VIRTUAL_H - h) * 0.5f)), box.y1 = box.y0 + h;
            return box;
        }

        void DrawBox(const View& v, const Box& box) { DrawFrame(v, box.x0, box.y0, box.x1, box.y1); }

        // The last line of a box: B BACK at the left (a zone for the mouse), `right` at the right.
        void DrawBoxFooter(const View& v, const Box& box, float y, const std::string& back, const std::string& right, const TextStyle& backStyle,
                           const TextStyle& rightStyle)
        {
            float left = box.Left(y, HELP_CELL), end = box.Right(y, HELP_CELL);
            float backEnd = Draw(v, FontId::Small, back, left, y, HELP_CELL, backStyle, 0.0f, end - left);
            AddZone(left - 10.0f, y - 8.0f, backEnd + 10.0f, y + HELP_CELL + 8.0f, Hit::Back);
            if (!right.empty())
                Draw(v, FontId::Small, right, end, y, HELP_CELL, rightStyle, 1.0f, end - backEnd - 30.0f);
        }

        // The width of a box's last line (DrawBoxFooter).
        float FooterWidth(const std::string& back, const std::string& right)
        {
            return Width(FontId::Small, back, HELP_CELL) + (right.empty() ? 0.0f : 30.0f + Width(FontId::Small, right, HELP_CELL));
        }

        std::string Upper(std::string text)
        {
            for (char& c : text)
                c = char(toupper(uint8_t(c)));
            return text;
        }

        // MULTIPLAYER's rows: the player's name (A: the name screen), HOST GAME, FIND GAME, the colour.
        enum RootRow { ROOT_NAME, ROOT_HOST, ROOT_FIND, ROOT_COLOUR };

        // ---- the player's colour (mp_colors.h) ----------------------------------------------------

        // The Root screen's last row: COLOR  < BLUE >, left / right (or A) change it.
        constexpr int ROOT_COLOUR_ROW = ROOT_COLOUR;

        std::string ColourRowText()
        {
            const colors::Colour& c = colors::Get(colors::Mine());
            return std::string(T("COLOR", "COR")) + ":  \xC2\xBA " + T(c.en, c.pt) + " \xC2\xBB";
        }

        // A sample of the colour's fur: a square at (x, y) in virtual units, with a dark edge.
        void DrawSwatch(const View& v, float x, float y, float size, uint32_t colour)
        {
            uint8_t rgb[3];
            colors::Swatch(colour, rgb);
            g_canvas.Fill(v.IX(x), v.IY(y), v.IX(x + size), v.IY(y + size), { 30, 14, 0, 255 });
            g_canvas.Fill(v.IX(x + 2.0f), v.IY(y + 2.0f), v.IX(x + size - 2.0f), v.IY(y + size - 2.0f), { rgb[0], rgb[1], rgb[2], 255 });
        }

        // Left / right (A: right) on the player's colour; true if pressed.
        bool StepColour(uint16_t pressed, bool acceptToo)
        {
            int change = (pressed & XINPUT_GAMEPAD_DPAD_LEFT) ? -1
                         : (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) || (acceptToo && (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START))) ? 1 : 0;
            if (change)
                colors::Step(change);
            return change != 0;
        }

        std::vector<std::string> Rows()
        {
            switch (g_screen)
            {
            case Screen::Root:
                return { std::string(T("NAME", "NOME")) + ":  " + lobby::LocalName(), T("HOST GAME", "CRIAR PARTIDA"),
                         T("FIND GAME", "ENCONTRAR PARTIDA"), ColourRowText() };
            case Screen::HostMode: return { T("NEW GAME", "NOVO JOGO"), T("LOAD GAME", "CARREGAR JOGO") };
            case Screen::FindMode: return { T("SERVER LIST", "LISTA DE PARTIDAS"), T("ENTER CODE", "DIGITAR CÓDIGO") };
            default: return {};
            }
        }

        std::string Title()
        {
            switch (g_screen)
            {
            case Screen::HostMode: return T("HOST GAME", "CRIAR PARTIDA");
            case Screen::FindMode: return T("FIND GAME", "ENCONTRAR PARTIDA");
            case Screen::Servers: return T("SERVER LIST", "LISTA DE PARTIDAS");
            case Screen::Code: return T("ENTER CODE", "DIGITAR CÓDIGO");
            case Screen::Password:
            case Screen::HostPassword: return T("PASSWORD", "SENHA");
            case Screen::Name: return T("YOUR NAME", "SEU NOME");
            case Screen::Waiting:
            case Screen::Lobby: return "LOBBY";
            default: return "MULTIPLAYER";
            }
        }

        // The field being typed: the code as XXXX-XXX (XXXXX-XXXXX once it has more than 7 letters), the
        // password or the name as is; '_' where letters are missing, the one under the cursor in the selected colour.
        void DrawField(const View& v, const Palette& pal, float y, bool code)
        {
            size_t length = code ? (g_field.size() > SHORT_CODE_LENGTH ? CODE_LENGTH : SHORT_CODE_LENGTH) : std::max(g_field.size() + 1, size_t(6));
            length = std::min(length, code ? CODE_LENGTH : PASSWORD_LENGTH);
            float cell = 46.0f, pitch = 36.0f, dash = code ? 26.0f : 0.0f;
            float total = length * pitch + dash, x = VIRTUAL_W * 0.5f - total * 0.5f;
            for (size_t i = 0; i < length; i++)
            {
                if (code && i == (length == SHORT_CODE_LENGTH ? 4u : 5u))
                {
                    Draw(v, FontId::Large, "-", x + dash * 0.5f, y, cell, pal.info, 0.5f);
                    x += dash;
                }
                std::string c = i < g_field.size() ? std::string(1, g_field[i]) : "_";
                Draw(v, FontId::Large, c, x + pitch * 0.5f, y, cell, i == g_cursor ? pal.selected : i < g_field.size() ? pal.normal : pal.info, 0.5f);
                x += pitch;
            }
        }
        constexpr float FIELD_CELL = 46.0f;

        // The field's letters and length (the screen typing it).
        const char* FieldLetters() { return g_screen == Screen::Code ? CODE_LETTERS : PASSWORD_LETTERS; }
        size_t FieldMaximum()
        {
            return g_screen == Screen::Code ? CODE_LENGTH : g_screen == Screen::Name ? NAME_LENGTH : PASSWORD_LENGTH;
        }

        // A character as the field takes it: upper case, and in a code O / I / L as 0 / 1 (0 if it takes none).
        char FieldChar(char32_t c)
        {
            char ch = char(toupper(int(c)));
            if (FieldLetters() == CODE_LETTERS)
                ch = ch == 'O' ? '0' : ch == 'I' || ch == 'L' ? '1' : ch;
            return c < 128 && strchr(FieldLetters(), ch) ? ch : 0;
        }

        // The keys (NEW GAME's name entry keyboard) from `top`; a key that does nothing now is dimmed.
        void DrawKeys(const View& v, float top)
        {
            bool full = g_field.size() >= FieldMaximum(), empty = g_field.empty();
            keyboard::Draw(g_canvas, v.s, v.ox, v.oy, top, g_key, g_portuguese,
                           [&](char32_t c) {
                               return c == keyboard::ERASE || c == keyboard::DONE ? !empty : !full && FieldChar(c) != 0;
                           },
                           [&](int key, float x0, float y0, float x1, float y1) {
                               char32_t c = keyboard::Keys()[size_t(key)].c;
                               AddZone(x0, y0, x1, y1, c == keyboard::ERASE ? Hit::Erase : c == keyboard::DONE ? Hit::Confirm : Hit::Key, int(c));
                           });
        }

        // X ERASE / DONE Y as the name entry's, over the options screen's own B BACK / SELECT A.
        void DrawTypingHints(const View& v, const Palette& pal)
        {
            constexpr float Y = 592.0f, CELL = 36.0f, LEFT = 262.0f, RIGHT = 1012.0f;
            std::string erase = std::string("\xC2\xB3 ") + T("ERASE", "APAGAR"), done = std::string(T("DONE", "PRONTO")) + " \xC2\xB4";
            float end = Draw(v, FontId::Large, erase, LEFT, Y, CELL, pal.normal);
            AddZone(LEFT - 10.0f, Y - 6.0f, end + 10.0f, Y + CELL + 6.0f, Hit::Erase);
            float start = RIGHT - Width(FontId::Large, done, CELL);
            Draw(v, FontId::Large, done, RIGHT, Y, CELL, pal.normal, 1.0f);
            AddZone(start - 10.0f, Y - 6.0f, RIGHT + 10.0f, Y + CELL + 6.0f, Hit::Confirm);
        }

        void DrawHelp(const View& v, const Palette& pal, const std::vector<std::string>& lines, float y = HELP_Y)
        {
            for (const std::string& line : lines)
            {
                Draw(v, FontId::Small, line, VIRTUAL_W * 0.5f, y, HELP_CELL, pal.help, 0.5f, 980.0f);
                y += HELP_CELL + 8.0f;
            }
        }

        // The players: "N  name" at the left, HOST or (YOU) the ping at the right. In a box (the host's
        // lobby) each line keeps inside it; on the cloth (a client's wait) between x 400 and 880.
        std::string RosterName(const lobby::Player& p) { return std::to_string(p.slot + 1) + "  " + p.name; }

        std::string RosterRight(const lobby::Player& p)
        {
            std::string right = p.slot == 0 ? std::string("HOST") : std::to_string(p.pingMs) + " ms";
            if (p.me && p.slot != 0)
                right = std::string(T("YOU", "VOCÊ")) + "  " + right;
            return right;
        }

        constexpr float ROSTER_GAP = 24.0f;

        float RosterWidth(const std::vector<lobby::Player>& players, float cell)
        {
            float widest = 0.0f;
            for (const lobby::Player& p : players)
                widest = std::max(widest, cell + 4.0f + Width(FontId::Small, RosterName(p), cell) + ROSTER_GAP + Width(FontId::Small, RosterRight(p), cell));
            return widest;
        }

        void DrawRoster(const View& v, const Palette& pal, float y, bool host, const Box* box = nullptr, float cell = LIST_CELL,
                        float pitch = LIST_PITCH)
        {
            std::vector<lobby::Player> players = lobby::Players();
            for (size_t i = 0; i < players.size(); i++)
            {
                const lobby::Player& p = players[i];
                bool selected = host && g_lobbyModal && g_row == int(i) && p.slot != 0;
                const TextStyle& style = selected || (p.me && !host) ? pal.selected : pal.normal;
                float left = box ? box->Left(y, cell) : 400.0f, right = box ? box->Right(y, cell) : 880.0f;
                std::string side = RosterRight(p);
                float swatch = cell - 4.0f, nameX = left + swatch + 8.0f;
                float sideW = std::min(Width(FontId::Small, side, cell), (right - nameX) * 0.5f);
                if (p.colour < colors::Count())
                    DrawSwatch(v, left, y + 2.0f, swatch, p.colour);
                Draw(v, FontId::Small, RosterName(p), nameX, y, cell, style, 0.0f, right - nameX - sideW - ROSTER_GAP);
                Draw(v, FontId::Small, side, right, y, cell, style, 1.0f, sideW);
                if (host && p.slot != 0)
                    AddZone(left - 20.0f, y - 6.0f, right + 20.0f, y + cell + 6.0f, Hit::Row, int(i), true);
                else if (p.me)  // the player's own row: left / right half change its colour
                {
                    float mid = (left + right) * 0.5f;
                    AddZone(left - 20.0f, y - 6.0f, mid, y + cell + 6.0f, Hit::RowLeft, int(i));
                    AddZone(mid, y - 6.0f, right + 20.0f, y + cell + 6.0f, Hit::RowRight, int(i));
                }
                y += pitch;
            }
        }

        // Lobby (host, at the HUB): its rows after the players.
        enum LobbyRow { ROW_LOOK, ROW_PASSWORD, ROW_MAX_PLAYERS, ROW_KNOCK_OUT, ROW_CLOSE, LOBBY_ROWS };

        int LobbyPlayerRows() { return int(lobby::Players().size()); }

        std::string LobbyRowText(int row)
        {
            lobby::Options o = lobby::GetOptions();
            switch (row)
            {
            case ROW_LOOK: return T("SKIN AND COLOR", "SKIN E COR");
            case ROW_PASSWORD:
                return std::string(T("PASSWORD", "SENHA")) + ":  " + (lobby::Password().empty() ? T("NONE", "NENHUMA") : lobby::Password());
            // \xC2\xBA / \xC2\xBB: the game's < > icons.
            case ROW_MAX_PLAYERS:
                // Past the game's 4: experimental (mp_players.cpp, players 5-8).
                return std::string(T("MAX PLAYERS", "MÁXIMO DE JOGADORES")) + ":  \xC2\xBA " + std::to_string(o.maxPlayers) + " \xC2\xBB" +
                       (o.maxPlayers > lobby::STABLE_PLAYERS ? "  EXPERIMENTAL" : "");
            case ROW_KNOCK_OUT:
                return std::string(T("KNOCK-OUT TIME", "TEMPO NOCAUTEADO")) + ":  \xC2\xBA " + std::to_string(o.knockOutSeconds) + " s \xC2\xBB";
            default: return T("CLOSE SESSION", "ENCERRAR SESSÃO");
            }
        }

        // A message (an error) in a box; A or a click anywhere goes on.
        void DrawMessage(const View& v)
        {
            constexpr float LINE = SUB_CELL + 8.0f;
            std::vector<std::string> lines = Wrap(g_message, FontId::Small, SUB_CELL, 680.0f);
            std::string ok = "\xC2\xB1 OK";
            float contentW = Width(FontId::Small, ok, SUB_CELL);
            for (const std::string& line : lines)
                contentW = std::max(contentW, Width(FontId::Small, line, SUB_CELL));
            Box box = FitBox(contentW, lines.size() * LINE + 14.0f + SUB_CELL, VIRTUAL_W * 0.5f, 0.5f, -1.0f);
            DrawBox(v, box);
            float y = box.Top();
            for (const std::string& line : lines)
            {
                Draw(v, FontId::Small, line, VIRTUAL_W * 0.5f, y, SUB_CELL, DIMMED.warning, 0.5f, box.Room(y, SUB_CELL));
                y += LINE;
            }
            Draw(v, FontId::Small, ok, VIRTUAL_W * 0.5f, y + 14.0f, SUB_CELL, DIMMED.selected, 0.5f);
        }

        // A field typed on the cloth (the code, a password, the name): what it is, the field, the keys and the
        // hints, as NEW GAME's name entry.
        void DrawTyping(const View& v, const Palette& pal, const std::string& about, bool code)
        {
            Draw(v, FontId::Small, about, VIRTUAL_W * 0.5f, SUB_Y + 4.0f, SUB_CELL, pal.help, 0.5f, 980.0f);
            DrawField(v, pal, ROWS_Y - 40.0f, code);
            DrawKeys(v, ROWS_Y + 26.0f);
            DrawTypingHints(v, pal);
        }

        void RasterizeFull(const View& v, int width, int height)
        {
            const Palette& pal = *g_palette;
            if (g_palette == &DIMMED)
                g_canvas.FillGradient(0, 0, width, height, DIM_TOP, DIM_BOTTOM);
            Draw(v, FontId::Large, Title(), VIRTUAL_W * 0.5f, TITLE_Y, TITLE_CELL, pal.title, 0.5f, 980.0f);
            if (g_screen != Screen::Message)
                AddFooterZones();
            std::vector<std::string> rows = Rows();
            if (!rows.empty())
            {
                for (size_t i = 0; i < rows.size(); i++)
                {
                    float y = ROWS_Y + 40.0f + i * ROW_PITCH;
                    float right = Draw(v, FontId::Large, rows[i], VIRTUAL_W * 0.5f, y, ROW_CELL, int(i) == g_row ? pal.selected : pal.normal, 0.5f, 900.0f);
                    float left = VIRTUAL_W - right;
                    if (g_screen == Screen::Root && int(i) == ROOT_COLOUR_ROW)
                    {
                        DrawSwatch(v, right + 16.0f, y + 2.0f, ROW_CELL - 4.0f, colors::Mine());
                        AddZone(left - 20.0f, y - 6.0f, VIRTUAL_W * 0.5f, y + ROW_CELL + 6.0f, Hit::RowLeft, int(i));
                        AddZone(VIRTUAL_W * 0.5f, y - 6.0f, right + 60.0f, y + ROW_CELL + 6.0f, Hit::RowRight, int(i));
                    }
                    else
                        AddZone(left - 20.0f, y - 6.0f, right + 20.0f, y + ROW_CELL + 6.0f, Hit::Row, int(i));
                }
                std::string help = g_screen == Screen::Root
                                       ? g_row == ROOT_NAME ? T("Your name in the games online: A to change it.",
                                                                "O seu nome nas partidas online: A para mudar.")
                                         : g_row == ROOT_COLOUR ? T("Your Crash's color: left / right to change it.",
                                                                    "A cor do seu Crash: esquerda / direita para mudar.")
                                                            : T("Up to 8 players (5-8 experimental): one hosts, the others find the game in the list (same network) or by its code.",
                                                                "Até 8 jogadores (5-8 experimental): um cria a partida, os outros a encontram na lista (mesma rede) ou pelo código.")
                                   : g_screen == Screen::HostMode
                                       ? T("Start or load your game: the players wait for you at the HUB.",
                                           "Comece ou carregue o seu jogo: os jogadores esperam você no HUB.")
                                       : T("The list shows the games on this network; a code also works over the internet.",
                                           "A lista mostra as partidas desta rede; o código serve também pela internet.");
                DrawHelp(v, pal, { help });
                if (g_screen == Screen::HostMode)
                    DrawHelp(v, pal, { std::string(T("Name: ", "Nome: ")) + lobby::LocalName() }, HELP_Y + 40.0f);
                return;
            }
            switch (g_screen)
            {
            case Screen::Servers:
            {
                std::vector<lobby::Server> servers = lobby::Servers();
                if (servers.empty())
                    Draw(v, FontId::Small, T("Looking for games on this network...", "Procurando partidas nesta rede..."), VIRTUAL_W * 0.5f,
                         ROWS_Y + 40.0f, LIST_CELL, pal.info, 0.5f, 980.0f);
                g_row = std::min(g_row, std::max(0, int(servers.size()) - 1));
                float y = ROWS_Y;
                for (size_t i = 0; i < servers.size() && i < 7; i++)
                {
                    const lobby::Server& s = servers[i];
                    const TextStyle& style = int(i) == g_row ? pal.selected : pal.normal;
                    std::string state = std::to_string(s.players) + "/" + std::to_string(s.maxPlayers);
                    if (s.locked)
                        state += std::string("  ") + T("PASSWORD", "SENHA");
                    if (s.inGame)
                        state += std::string("  ") + T("IN A LEVEL", "EM FASE");
                    float stateW = Width(FontId::Small, state, LIST_CELL);
                    Draw(v, FontId::Small, s.name, 300.0f, y, LIST_CELL, style, 0.0f, std::min(360.0f, 980.0f - stateW - 30.0f - 300.0f));
                    Draw(v, FontId::Small, state, 980.0f, y, LIST_CELL, style, 1.0f, 400.0f);
                    AddZone(280.0f, y - 6.0f, 1000.0f, y + LIST_CELL + 6.0f, Hit::Row, int(i));
                    y += LIST_PITCH;
                }
                DrawHelp(v, pal, { T("A: join    B: back", "A: entrar    B: voltar") });
                break;
            }
            case Screen::Code:
                DrawTyping(v, pal, T("The host's code (in its HUB panel)", "O código do host (no painel do HUB dele)"), true);
                break;
            case Screen::Password:
                DrawTyping(v, pal, T("This game has a password", "Esta partida tem senha"), false);
                break;
            case Screen::Name:
                DrawTyping(v, pal, T("Your name in the games online (A-Z, 0-9, up to 12)", "O seu nome nas partidas online (A-Z, 0-9, até 12)"), false);
                break;
            case Screen::Waiting:
            {
                lobby::State state = lobby::GetState();
                if (state == lobby::State::Joining)
                {
                    Draw(v, FontId::Small, std::string(T("Connecting to ", "Conectando a ")) + lobby::Address() + "...", VIRTUAL_W * 0.5f, ROWS_Y,
                         SUB_CELL, pal.help, 0.5f, 980.0f);
                }
                else
                {
                    Draw(v, FontId::Small, std::string(T("Code ", "Código ")) + lobby::InviteCode(), VIRTUAL_W * 0.5f, SUB_Y, SUB_CELL, pal.help, 0.5f,
                         980.0f);
                    DrawRoster(v, pal, ROWS_Y, false);
                    DrawHelp(v, pal, { lobby::HostInGame() ? T("The host is in a level: you join the next one it starts.",
                                                               "O host está numa fase: você entra na próxima que ele começar.")
                                                           : T("Waiting for the host to start a level...", "Esperando o host começar uma fase...") });
                }
                if (lobby::GetState() == lobby::State::Joined)
                {
                    std::string look = std::string("\xC2\xB4 ") + T("SKIN AND COLOR", "SKIN E COR");
                    // Under the players (the waiting text is at the help's place).
                    float y = ROWS_Y + float(lobby::Players().size()) * LIST_PITCH + 24.0f;
                    float right = Draw(v, FontId::Small, look, VIRTUAL_W * 0.5f, y, SUB_CELL, pal.selected, 0.5f, 600.0f);
                    float left = VIRTUAL_W - right;
                    AddZone(left - 20.0f, y - 6.0f, right + 20.0f, y + SUB_CELL + 6.0f, Hit::Look);
                }
                DrawHelp(v, pal, { lobby::GetState() == lobby::State::Joined ? T("Left / right: your color    B: leave", "Esquerda / direita: sua cor    B: sair")
                                                                             : T("B: leave", "B: sair") }, HELP_Y + 40.0f);
                break;
            }
            case Screen::Message:
                AddZone(0.0f, 0.0f, VIRTUAL_W, VIRTUAL_H, Hit::Anywhere);
                DrawMessage(v);
                break;
            default: break;
            }
        }

        // Host, at the HUB: the lobby's password, typed in a box.
        void RasterizeHostPassword(const View& v, const Palette& pal)
        {
            constexpr float TITLE = 38.0f;
            std::string title = T("PASSWORD", "SENHA");
            std::string back = std::string("\xC2\xB2 ") + T("BACK", "VOLTAR"), note = T("Empty = no password", "Vazia = sem senha");
            float keysH = keyboard::HEIGHT;
            float contentW = std::max({ 10 * keyboard::PITCH_X, Width(FontId::Large, title, TITLE), FooterWidth(back, note) });
            Box box = FitBox(contentW, TITLE + 16.0f + FIELD_CELL + 20.0f + keysH + 16.0f + HELP_CELL, VIRTUAL_W * 0.5f, 0.5f, -1.0f);
            DrawBox(v, box);
            float y = box.Top();
            Draw(v, FontId::Large, title, VIRTUAL_W * 0.5f, y, TITLE, pal.title, 0.5f, box.Room(y, TITLE));
            y += TITLE + 16.0f;
            DrawField(v, pal, y, false);
            y += FIELD_CELL + 20.0f;
            DrawKeys(v, y);
            y += keysH + 16.0f;
            DrawBoxFooter(v, box, y, back, note, pal.help, pal.info);
        }

        // Host, at the HUB: the lobby (modal) over the dimmed HUB. With many players the rows get closer
        // so the box (whose frame takes a quarter of its height) stays on the screen.
        void RasterizeLobby(const View& v, int width, int height)
        {
            const Palette& pal = DIMMED;
            g_canvas.FillGradient(0, 0, width, height, DIM_TOP, DIM_BOTTOM);  // the HUB's own texts behind
            if (g_screen == Screen::HostPassword)
            {
                RasterizeHostPassword(v, pal);
                return;
            }
            constexpr float TITLE = 38.0f, CODE = 26.0f, NET = 22.0f, MAX_CONTENT_H = 450.0f;
            std::vector<lobby::Player> list = lobby::Players();
            int players = int(list.size()), rows = players + LOBBY_ROWS;
            std::string code = lobby::InviteCode(), local = lobby::LocalCode();
            std::string codeLine = std::string(T("Code ", "Código ")) + code +
                                   (code != local ? std::string(T("   LAN ", "   rede local ")) + local : "   " + lobby::Address());
            std::string net = lobby::InternetText(g_portuguese);
            std::string back = std::string("\xC2\xB2 ") + T("BACK", "VOLTAR");
            std::string help = g_row < players ? (list[size_t(g_row)].me ? T("Left / right: your color", "Esquerda / direita: sua cor") : T("A: kick", "A: expulsar"))
                                               : T("A / left / right: change", "A / esquerda / direita: mudar");
            float fixed = TITLE + 12.0f + CODE + 6.0f + NET + 16.0f + 4.0f + HELP_CELL;
            float pitch = std::clamp((MAX_CONTENT_H - fixed) / rows, 30.0f, LIST_PITCH), cell = std::min(LIST_CELL, pitch - 8.0f);
            float contentW = std::max({ Width(FontId::Large, "MULTIPLAYER", TITLE), Width(FontId::Small, codeLine, CODE),
                                        Width(FontId::Small, net, NET), RosterWidth(list, cell),
                                        FooterWidth(back, T("A / left / right: change", "A / esquerda / direita: mudar")) });
            for (int row = 0; row < LOBBY_ROWS; row++)
                contentW = std::max(contentW, Width(FontId::Small, LobbyRowText(row), cell));
            contentW = std::clamp(contentW, 480.0f, 760.0f);
            Box box = FitBox(contentW, fixed + rows * pitch, VIRTUAL_W * 0.5f, 0.5f, -1.0f);
            DrawBox(v, box);
            float cx = VIRTUAL_W * 0.5f, y = box.Top();
            Draw(v, FontId::Large, "MULTIPLAYER", cx, y, TITLE, pal.title, 0.5f, box.Room(y, TITLE));
            y += TITLE + 12.0f;
            Draw(v, FontId::Small, codeLine, cx, y, CODE, pal.info, 0.5f, box.Room(y, CODE));
            y += CODE + 6.0f;
            Draw(v, FontId::Small, net, cx, y, NET, pal.help, 0.5f, box.Room(y, NET));
            y += NET + 16.0f;
            DrawRoster(v, pal, y, true, &box, cell, pitch);
            y += players * pitch;
            for (int row = 0; row < LOBBY_ROWS; row++)
            {
                float left = box.Left(y, cell), right = box.Right(y, cell);
                Draw(v, FontId::Small, LobbyRowText(row), cx, y, cell, g_row == players + row ? pal.selected : pal.normal, 0.5f, right - left);
                if (row == ROW_MAX_PLAYERS || row == ROW_KNOCK_OUT)
                {
                    AddZone(left - 20.0f, y - 6.0f, cx, y + cell + 6.0f, Hit::RowLeft, players + row);
                    AddZone(cx, y - 6.0f, right + 20.0f, y + cell + 6.0f, Hit::RowRight, players + row);
                }
                else
                    AddZone(left - 20.0f, y - 6.0f, right + 20.0f, y + cell + 6.0f, Hit::Row, players + row, row == ROW_CLOSE);
                y += pitch;
            }
            DrawBoxFooter(v, box, y + 4.0f, back, help, pal.help, pal.info);
        }

        // A client's menu in a level, over the game (which goes on).
        void RasterizeLevelMenu(const View& v)
        {
            const Palette& pal = DIMMED;
            constexpr float TITLE = 38.0f;
            const char* rows[LEVEL_MENU_ROWS] = { T("RESUME", "CONTINUAR"), T("SETTINGS", "CONFIGURAÇÕES"), T("QUIT TO THE MAIN MENU", "SAIR PARA O MENU PRINCIPAL") };
            std::string back = std::string("\xC2\xB2 ") + T("BACK TO THE GAME", "VOLTAR AO JOGO");
            float contentW = std::max(Width(FontId::Large, "MULTIPLAYER", TITLE), FooterWidth(back, ""));
            for (const char* row : rows)
                contentW = std::max(contentW, Width(FontId::Large, row, ROW_CELL));
            contentW = std::min(contentW, 640.0f);
            Box box = FitBox(contentW, TITLE + 20.0f + int(LEVEL_MENU_ROWS) * ROW_PITCH + 8.0f + HELP_CELL, VIRTUAL_W * 0.5f, 0.5f, -1.0f);
            DrawBox(v, box);
            float y = box.Top();
            Draw(v, FontId::Large, "MULTIPLAYER", VIRTUAL_W * 0.5f, y, TITLE, pal.title, 0.5f, box.Room(y, TITLE));
            y += TITLE + 20.0f;
            for (int row = 0; row < LEVEL_MENU_ROWS; row++)
            {
                float right = Draw(v, FontId::Large, rows[row], VIRTUAL_W * 0.5f, y, ROW_CELL, row == g_row ? pal.selected : pal.normal, 0.5f,
                                   box.Room(y, ROW_CELL));
                AddZone(VIRTUAL_W - right - 20.0f, y - 6.0f, right + 20.0f, y + ROW_CELL + 6.0f, Hit::Row, row);
                y += ROW_PITCH;
            }
            DrawBoxFooter(v, box, y + 8.0f, back, "", pal.help, pal.info);
        }

        // The HUB panel, at the top right: as wide and as high as its lines (the code, the internet's
        // state, the players and their ping), kept inside its frame.
        constexpr float PANEL_R = 1268.0f, PANEL_T = 8.0f;
        constexpr float PANEL_TITLE = 24.0f, PANEL_CODE = 26.0f, PANEL_NET = 18.0f, PANEL_NAME = 22.0f, PANEL_PING = 20.0f, PANEL_PITCH = 26.0f;

        void RasterizePanel(const View& v)
        {
            const Palette& pal = DIMMED;
            std::vector<lobby::Player> players = lobby::Players();
            std::string title = "MULTIPLAYER " + std::to_string(players.size()) + "/" + std::to_string(lobby::GetOptions().maxPlayers);
            std::string code = lobby::InviteCode(), hint = "\xC2\xB5 LOBBY";
            std::vector<std::string> net = Wrap(lobby::InternetText(g_portuguese), FontId::Small, PANEL_NET, 300.0f);
            auto name = [](const lobby::Player& p) { return std::to_string(p.slot + 1) + " " + p.name; };
            auto ping = [](const lobby::Player& p) { return p.slot ? std::to_string(p.pingMs) + " ms" : std::string(); };
            float contentW = std::max({ Width(FontId::Small, title, PANEL_TITLE), Width(FontId::Small, code, PANEL_CODE), Width(FontId::Small, hint, PANEL_NAME) });
            for (const std::string& line : net)
                contentW = std::max(contentW, Width(FontId::Small, line, PANEL_NET));
            for (const lobby::Player& p : players)
                contentW = std::max(contentW, Width(FontId::Small, name(p), PANEL_NAME) + 16.0f + Width(FontId::Small, ping(p), PANEL_PING));
            contentW = std::clamp(contentW, 220.0f, 340.0f);
            float contentH = PANEL_TITLE + 8.0f + PANEL_CODE + 6.0f + net.size() * (PANEL_NET + 4.0f) + 8.0f + players.size() * PANEL_PITCH + 6.0f + PANEL_NAME;
            Box box = FitBox(contentW, contentH, PANEL_R, 1.0f, PANEL_T);
            DrawBox(v, box);
            AddZone(box.x0, box.y0, box.x1, box.y1, Hit::Panel);
            float y = box.Top();
            Draw(v, FontId::Small, title, box.Left(y, PANEL_TITLE), y, PANEL_TITLE, pal.title, 0.0f, box.Room(y, PANEL_TITLE));
            y += PANEL_TITLE + 8.0f;
            Draw(v, FontId::Small, code, box.Left(y, PANEL_CODE), y, PANEL_CODE, pal.selected, 0.0f, box.Room(y, PANEL_CODE));
            y += PANEL_CODE + 6.0f;
            for (const std::string& line : net)
            {
                Draw(v, FontId::Small, line, box.Left(y, PANEL_NET), y, PANEL_NET, pal.help, 0.0f, box.Room(y, PANEL_NET));
                y += PANEL_NET + 4.0f;
            }
            y += 8.0f;
            for (const lobby::Player& p : players)
            {
                float left = box.Left(y, PANEL_NAME), right = box.Right(y, PANEL_NAME);
                std::string ms = ping(p);
                float msW = ms.empty() ? 0.0f : std::min(Width(FontId::Small, ms, PANEL_PING), (right - left) * 0.4f);
                Draw(v, FontId::Small, name(p), left, y, PANEL_NAME, pal.normal, 0.0f, right - left - (ms.empty() ? 0.0f : msW + 16.0f));
                if (!ms.empty())
                    Draw(v, FontId::Small, ms, right, y + (PANEL_NAME - PANEL_PING) * 0.5f, PANEL_PING, pal.info, 1.0f, msW);
                y += PANEL_PITCH;
            }
            y += 6.0f;
            Draw(v, FontId::Small, hint, box.Left(y, PANEL_NAME), y, PANEL_NAME, pal.help, 0.0f, box.Room(y, PANEL_NAME));
        }

        View MakeView(int width, int height)
        {
            View v;
            v.s = std::min(width / VIRTUAL_W, height / VIRTUAL_H);
            v.ox = (width - VIRTUAL_W * v.s) * 0.5f, v.oy = (height - VIRTUAL_H * v.s) * 0.5f;
            return v;
        }

        // ---- input ---------------------------------------------------------------------------------

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
                g_heldButton = 0;
            g_previousButtons = buttons;
            return pressed;
        }

        // The mouse / touch since the last call as buttons (and letters in g_tapped). Hover selects a
        // row; a click is taken from the game's menus.
        uint16_t MouseButtons()
        {
            uint32_t moves = input::pc::CursorMoves(), clicks = input::pc::LeftClicks();
            bool moved = moves != g_mouseMoves, clicked = clicks != g_mouseClicks;
            g_mouseMoves = moves, g_mouseClicks = clicks;
            float u, v;
            if ((!moved && !clicked) || !g_drawnW || !g_drawnH || !input::pc::CursorInClient(u, v))
                return 0;
            if (clicked)
                input::pc::TakeMenuClick();
            View view = MakeView(g_drawnW, g_drawnH);
            float x = (u * g_drawnW - view.ox) / view.s, y = (v * g_drawnH - view.oy) / view.s;
            const HitZone* zone = nullptr;
            for (const HitZone& z : g_zones)
                if (x >= z.x0 && x < z.x1 && y >= z.y0 && y < z.y1)
                    zone = &z;  // the last drawn is on top
            if (!zone)
                return 0;
            int before = g_row;
            if ((zone->hit == Hit::Row || zone->hit == Hit::RowLeft || zone->hit == Hit::RowRight) && g_row != zone->index)
            {
                g_row = zone->index;
                g_dirty = true;
            }
            if (!clicked)
                return 0;
            switch (zone->hit)
            {
            case Hit::Row: return zone->askFirst && before != zone->index ? 0 : XINPUT_GAMEPAD_A;
            case Hit::RowLeft: return XINPUT_GAMEPAD_DPAD_LEFT;
            case Hit::RowRight: return XINPUT_GAMEPAD_DPAD_RIGHT;
            case Hit::Back: return XINPUT_GAMEPAD_B;
            case Hit::Accept:
            case Hit::Anywhere: return XINPUT_GAMEPAD_A;
            case Hit::Key: g_tapped += char32_t(zone->index); return 0;
            case Hit::Erase: return XINPUT_GAMEPAD_X;
            case Hit::Confirm: g_tapped += U'\r'; return 0;
            case Hit::Panel: return XINPUT_GAMEPAD_BACK;
            case Hit::Look: return XINPUT_GAMEPAD_Y;
            case Hit::Up: return XINPUT_GAMEPAD_DPAD_UP;
            case Hit::Down: return XINPUT_GAMEPAD_DPAD_DOWN;
            }
            return 0;
        }

        bool Typing()
        {
            return g_screen == Screen::Code || g_screen == Screen::Password || g_screen == Screen::HostPassword || g_screen == Screen::Name;
        }

        bool g_textEntry = false;  // the keyboard types text (a field's screen)

        void Go(Screen screen, int row = 0)
        {
            g_screen = screen;
            g_row = row;
            g_dirty = true;
            if (Typing() != g_textEntry)
            {
                g_textEntry = Typing();
                input::pc::SetTextEntry(g_textEntry);
                if (g_textEntry)
                    input::pc::TakeTypedText();
            }
        }

        void ShowMessage(const std::string& text, Screen after)
        {
            g_message = text;
            g_afterMessage = after;
            Go(Screen::Message);
        }

        void Open()
        {
            g_portuguese = settings::Get().language.rfind("pt", 0) == 0;
            g_open = true;
            g_closing = false;
            g_openedAt = plat::TickCountMs();
            g_previousButtons = input::MenuButtons();  // the A that chose the item is not a press here
            g_heldButton = 0;
            g_mouseMoves = input::pc::CursorMoves(), g_mouseClicks = input::pc::LeftClicks();
            g_tapped.clear();
            g_dirty = true;
            art::Ready(2000);
        }

        // `back`: B for the game, which leaves the options screen to the main menu.
        void Close(bool back)
        {
            if (g_textEntry)
                input::pc::SetTextEntry(false);
            g_textEntry = false;
            g_closing = !g_lobbyModal && !g_levelModal;  // the lobby at the HUB just goes (the panel is back)
            g_open = false;
            g_lobbyModal = false;
            g_levelModal = false;
            g_releasing = true;
            g_closedAt = plat::TickCountMs();
            if (back)
                g_backUntil = g_closedAt + BACK_PRESS_MS;
            g_dirty = true;
            Log("[mp] menu closed%s", back ? " (back to the main menu)" : "");
        }

        void Join(const std::string& target, const std::string& password)
        {
            g_joinTarget = target;
            if (!lobby::Join(target, password))
            {
                ShowMessage(g_screen == Screen::Code ? T("THAT IS NOT A VALID CODE", "ESSE CÓDIGO NÃO É VÁLIDO") : lobby::ReasonText(lobby::LastReason(), g_portuguese),
                            g_screen);
                return;
            }
            Go(Screen::Waiting);
        }

        // The field, as NEW GAME's name entry: typed letters (keyboard, taps), the D-pad from key to key, A the
        // highlighted key, X erases the last letter, Y / START (or DONE) confirm. True when confirmed.
        bool EditField(uint16_t pressed, const char* letters, size_t maximum)
        {
            (void)letters;
            bool confirm = false;
            auto type = [&](char32_t c) {
                if (char ch = FieldChar(c); ch && g_field.size() < maximum)
                {
                    g_field += ch;
                    g_dirty = true;
                }
            };
            auto erase = [&] {
                if (!g_field.empty())
                {
                    g_field.pop_back();
                    g_dirty = true;
                }
            };
            std::u32string typed = input::pc::TakeTypedText() + g_tapped;
            g_tapped.clear();
            for (char32_t c : typed)
            {
                if (c == U'\r')
                    confirm = true;
                else if (c == U'\b')
                    erase();
                else
                {
                    if (int key = keyboard::KeyOf(char32_t(toupper(int(c)))); key >= 0)
                        g_key = key;
                    type(c);
                }
            }
            if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) g_key = keyboard::Step(g_key, -1, 0);
            if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) g_key = keyboard::Step(g_key, 1, 0);
            if (pressed & XINPUT_GAMEPAD_DPAD_UP) g_key = keyboard::Step(g_key, 0, -1);
            if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) g_key = keyboard::Step(g_key, 0, 1);
            if (pressed & XINPUT_GAMEPAD_A)
            {
                char32_t c = keyboard::Keys()[size_t(g_key)].c;
                if (c == keyboard::ERASE)
                    erase();
                else if (c == keyboard::DONE)
                    confirm = true;
                else
                    type(c);
            }
            if (pressed & XINPUT_GAMEPAD_X)
                erase();
            if (pressed & (XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_START))
                confirm = true;
            g_cursor = g_field.size();
            return confirm;
        }

        // Tests: CRASH_MP_CODE fills the code, CRASH_MP_PASSWORD the password (mp_lobby.h).
        std::string Prefill(const char* name)
        {
            const char* text = getenv(name);
            return text ? Upper(text) : std::string();
        }

        void BeginField(Screen screen, const std::string& text)
        {
            g_field.clear();
            for (char c : text)
                if (c != '-' && (screen != Screen::Name || isalnum(uint8_t(c))))
                    g_field += c;
            g_cursor = g_field.size();
            g_key = keyboard::KeyOf('A');
            Go(screen);
        }

        // The player's name online ([multiplayer] name in settings.toml, lobby::SetLocalName): asked
        // the first time MULTIPLAYER opens, unless CRASH_MP_NAME gives one (tests).
        bool NameAsked() { return !settings::Get().mpName.empty() || getenv("CRASH_MP_NAME"); }

        void SaveName(const std::string& name)
        {
            settings::Values values = settings::Get();
            values.mpName = name;
            settings::Set(values);
            settings::Save();
            lobby::SetLocalName(name);
            Log("[mp] menu: player name %s (saved)", name.c_str());
        }

        // A screen opened from the main menu.
        void UpdateFull(uint16_t pressed)
        {
            bool back = pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK);
            int rows = int(Rows().size());
            if (rows)
            {
                if (pressed & XINPUT_GAMEPAD_DPAD_UP)
                    g_row = (g_row + rows - 1) % rows;
                if (pressed & XINPUT_GAMEPAD_DPAD_DOWN)
                    g_row = (g_row + 1) % rows;
            }
            bool accept = pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START);
            switch (g_screen)
            {
            case Screen::Root:
                if (back)
                    Close(true);
                else if (accept && g_row == ROOT_NAME)
                    BeginField(Screen::Name, lobby::LocalName());
                else if (g_row == ROOT_COLOUR_ROW)
                    StepColour(pressed, true);
                else if (accept)
                    Go(g_row == ROOT_HOST ? Screen::HostMode : Screen::FindMode);
                break;
            case Screen::Name:
                if (back)
                {
                    Go(Screen::Root, ROOT_NAME);
                    break;
                }
                if (EditField(pressed, PASSWORD_LETTERS, NAME_LENGTH))
                {
                    if (g_field.empty())
                        ShowMessage(T("TYPE YOUR NAME", "DIGITE O SEU NOME"), Screen::Name);
                    else
                    {
                        SaveName(g_field);
                        Go(Screen::Root, ROOT_HOST);
                    }
                }
                break;
            case Screen::HostMode:
                if (back)
                    Go(Screen::Root, ROOT_HOST);
                else if (accept)
                {
                    if (!lobby::Host())
                    {
                        ShowMessage(lobby::ReasonText(lobby::LastReason(), g_portuguese), Screen::HostMode);
                        break;
                    }
                    bool newGame = g_row == 0;
                    g_hostNewGame = newGame;
                    g_forceExit = newGame ? EXIT_NEW_GAME : EXIT_LOAD_GAME;
                    Log("[mp] menu: hosting, %s", newGame ? "new game" : "load game");
                    Close(false);
                }
                break;
            case Screen::FindMode:
                if (back)
                    Go(Screen::Root, ROOT_FIND);
                else if (accept && g_row == 0)
                {
                    lobby::Search();
                    Go(Screen::Servers);
                }
                else if (accept)
                    BeginField(Screen::Code, Prefill("CRASH_MP_CODE"));
                break;
            case Screen::Servers:
            {
                std::vector<lobby::Server> servers = lobby::Servers();
                int count = int(std::min<size_t>(servers.size(), 7));
                if (count && (pressed & XINPUT_GAMEPAD_DPAD_UP))
                    g_row = (g_row + count - 1) % count;
                if (count && (pressed & XINPUT_GAMEPAD_DPAD_DOWN))
                    g_row = (g_row + 1) % count;
                if (back)
                {
                    lobby::Leave();
                    Go(Screen::FindMode, 0);
                }
                else if (accept && g_row < count)
                {
                    const lobby::Server& s = servers[size_t(g_row)];
                    std::string target = lobby::FormatAddress(s.ip, s.port);
                    if (s.locked)
                    {
                        g_joinTarget = target;
                        BeginField(Screen::Password, Prefill("CRASH_MP_PASSWORD"));
                    }
                    else
                        Join(target, "");
                }
                break;
            }
            case Screen::Code:
                if (back)
                {
                    Go(Screen::FindMode, 1);
                    break;
                }
                if (EditField(pressed, CODE_LETTERS, CODE_LENGTH))
                {
                    if (g_field.size() != SHORT_CODE_LENGTH && g_field.size() != CODE_LENGTH)
                        ShowMessage(T("THE CODE HAS 7 LETTERS", "O CÓDIGO TEM 7 LETRAS"), Screen::Code);
                    else
                        Join(g_field, "");
                }
                break;
            case Screen::Password:
                if (back)
                {
                    lobby::Leave();
                    Go(Screen::FindMode, 0);
                    break;
                }
                if (EditField(pressed, PASSWORD_LETTERS, PASSWORD_LENGTH))
                    Join(g_joinTarget, g_field);
                break;
            case Screen::Waiting:
            {
                lobby::State state = lobby::GetState();
                if (back)
                {
                    lobby::Leave();
                    Go(Screen::Root, ROOT_FIND);
                }
                else if (state == lobby::State::Joined && (pressed & XINPUT_GAMEPAD_Y))
                    OpenPicker(false);
                else if (state == lobby::State::Joined && StepColour(pressed, false))
                    break;
                else if (state == lobby::State::Failed)
                {
                    lobby::Reason reason = lobby::LastReason();
                    if (reason == lobby::Reason::Password)
                    {
                        BeginField(Screen::Password, Prefill("CRASH_MP_PASSWORD"));  // g_joinTarget stays
                        Log("[mp] menu: the host asks for a password");
                    }
                    else
                        ShowMessage(lobby::ReasonText(reason, g_portuguese), Screen::FindMode);
                }
                break;
            }
            case Screen::Message:
                if (accept || back)
                    Go(g_afterMessage);
                break;
            default: break;
            }
        }

        // The host's lobby at the HUB.
        void UpdateLobby(uint16_t pressed)
        {
            int players = LobbyPlayerRows(), rows = players + LOBBY_ROWS;
            if (g_screen == Screen::HostPassword)
            {
                if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK))
                    Go(Screen::Lobby, players + ROW_PASSWORD);
                else if (EditField(pressed, PASSWORD_LETTERS, PASSWORD_LENGTH))
                {
                    lobby::SetPassword(g_field);
                    Go(Screen::Lobby, players + ROW_PASSWORD);
                }
                return;
            }
            if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK))
            {
                Close(false);
                return;
            }
            if (pressed & XINPUT_GAMEPAD_DPAD_UP)
                g_row = (g_row + rows - 1) % rows;
            if (pressed & XINPUT_GAMEPAD_DPAD_DOWN)
                g_row = (g_row + 1) % rows;
            g_row = std::clamp(g_row, 0, rows - 1);
            bool accept = pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START);
            int change = (pressed & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) || accept ? 1 : 0;
            if (g_row < players)
            {
                std::vector<lobby::Player> list = lobby::Players();
                if (list[size_t(g_row)].me)
                    StepColour(pressed, true);  // the host's own row: its colour
                else if (accept)
                    lobby::Kick(list[size_t(g_row)].slot);
                return;
            }
            int row = g_row - players;
            lobby::Options o = lobby::GetOptions();
            if (row == ROW_LOOK && accept)
                OpenPicker(true);
            else if (row == ROW_PASSWORD && accept)
                BeginField(Screen::HostPassword, lobby::Password());
            else if (row == ROW_MAX_PLAYERS && change)
            {
                o.maxPlayers = uint32_t(std::clamp(int(o.maxPlayers) + change, 2, int(lobby::MAX_PLAYERS)));
                lobby::SetOptions(o);
            }
            else if (row == ROW_KNOCK_OUT && change)
            {
                static const uint32_t TIMES[] = { 3, 5, 10 };
                int at = int(std::find(std::begin(TIMES), std::end(TIMES), o.knockOutSeconds) - std::begin(TIMES));
                o.knockOutSeconds = TIMES[std::clamp(at + change, 0, 2)];
                lobby::SetOptions(o);
            }
            else if (row == ROW_CLOSE && accept)
            {
                lobby::Leave();
                Close(false);
            }
        }

        // A client's START in a level: RESUME, SETTINGS or QUIT TO THE MAIN MENU (A); B / START resume too.
        void UpdateLevelMenu(uint16_t pressed)
        {
            if (pressed & XINPUT_GAMEPAD_DPAD_UP)
                g_row = (g_row + LEVEL_MENU_ROWS - 1) % LEVEL_MENU_ROWS;
            if (pressed & XINPUT_GAMEPAD_DPAD_DOWN)
                g_row = (g_row + 1) % LEVEL_MENU_ROWS;
            if ((pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)) ||
                ((pressed & XINPUT_GAMEPAD_A) && g_row == ROW_RESUME))
                Close(false);
            else if ((pressed & XINPUT_GAMEPAD_A) && g_row == ROW_SETTINGS)
            {
                Close(false);
                settings::menu::RequestOpenOverGame();  // the settings screen, over the game too
                Log("[mp] menu: client: settings");
            }
            else if (pressed & XINPUT_GAMEPAD_A)
            {
                g_quitLevel = true;
                Log("[mp] menu: client: quit to the main menu");
                Close(false);
            }
        }

        // SKIN AND COLOR: the screens close and the next frontend condition (OverrideExit) takes the skin screen.
        void OpenPicker(bool host)
        {
            g_pickerHost = host;
            g_pickerRequest = true;
            Close(false);
            Log("[mp] menu: SKIN AND COLOR (%s)", host ? "host, from the HUB" : "client, from the wait");
        }

        bool PickerWanted() { return g_picker && g_node == NODE_SKINS && lobby::GetState() != lobby::State::Off; }

        // A disc of `colour`'s fur with the game's dark edge (the colour's sample next to its name).
        void DrawDisc(const View& v, float x, float y, float size, uint32_t colour)
        {
            uint8_t rgb[3];
            colors::Swatch(colour, rgb);
            int d = std::max(4, int(std::lround(size * v.s)));
            art::Image disc;
            disc.w = disc.h = d;
            disc.px.assign(size_t(d) * d, 0);
            float r = d * 0.5f, edge = std::max(1.5f, d * 0.09f);
            for (int py = 0; py < d; py++)
                for (int px = 0; px < d; px++)
                {
                    float dx = px + 0.5f - r, dy = py + 0.5f - r, dist = std::sqrt(dx * dx + dy * dy);
                    float cover = std::clamp(r - dist, 0.0f, 1.0f);
                    if (cover <= 0.0f)
                        continue;
                    float inside = std::clamp(r - edge - dist, 0.0f, 1.0f);  // 1 in the colour, 0 on the edge
                    float c[3] = { rgb[0] * inside + 40.0f * (1.0f - inside), rgb[1] * inside + 16.0f * (1.0f - inside),
                                   rgb[2] * inside };
                    auto p = [&](float value) { return uint32_t(std::lround(value * cover)); };
                    disc.px[size_t(py) * d + px] = p(c[0]) | p(c[1]) << 8 | p(c[2]) << 16 | uint32_t(std::lround(cover * 255.0f)) << 24;
                }
            g_canvas.Blit(disc, v.IX(x), v.IY(y));
        }

        // SKIN AND COLOR over the game's skin screen, in its own look: on the left the colour (its title as the
        // skin's name, the up / down arrows as the menus' left / right ones around the colour's name and disc),
        // at the bottom between BACK and WEAR SKIN the hint as the game's buttons. Up / down (or a tap on an
        // arrow) change the colour, the game's left / right the skin.
        void RasterizePicker(const View& v)
        {
            constexpr TextStyle TITLE{ { 64, 26, 2, 255 }, { 200, 124, 30, 255 } };  // the skin's name on the cloth
            constexpr TextStyle HINT{ { 236, 150, 20, 255 }, { 70, 28, 0, 255 } };   // BACK / WEAR SKIN
            constexpr float CX = 222.0f, TITLE_CELL = 38.0f, NAME_CELL = 34.0f, ARROW = 34.0f, DISC = 30.0f;
            constexpr float HINT_Y = 624.0f, HINT_CELL = 36.0f, HINT_ARROW = 22.0f;
            const colors::Colour& c = colors::Get(colors::Mine());
            float y = 236.0f;
            Draw(v, FontId::Large, T("COLOR", "COR"), CX, y, TITLE_CELL, TITLE, 0.5f, 380.0f);
            y += TITLE_CELL + 14.0f;
            float arrowW = art::IconWidth(art::ICON_LEFT, ARROW);  // turned, the arrow is ARROW wide and arrowW high
            art::DrawIconTurned(g_canvas, art::ICON_LEFT, v.X(CX - ARROW * 0.5f), v.Y(y), ARROW * v.s, true);
            AddZone(CX - 80.0f, y - 8.0f, CX + 80.0f, y + arrowW + 6.0f, Hit::Up);
            y += arrowW + 10.0f;
            std::string name = T(c.en, c.pt);
            float nameW = std::min(Width(FontId::Large, name, NAME_CELL), 300.0f), total = DISC + 12.0f + nameW;
            DrawDisc(v, CX - total * 0.5f, y + (NAME_CELL - DISC) * 0.5f, DISC, colors::Mine());
            Draw(v, FontId::Large, name, CX - total * 0.5f + DISC + 12.0f, y, NAME_CELL, HINT, 0.0f, nameW);
            y += NAME_CELL + 10.0f;
            art::DrawIconTurned(g_canvas, art::ICON_LEFT, v.X(CX - ARROW * 0.5f), v.Y(y), ARROW * v.s, false);
            AddZone(CX - 80.0f, y - 6.0f, CX + 80.0f, y + arrowW + 8.0f, Hit::Down);
            // The hint: the two arrows one over the other, then COLOR, as the game's B BACK.
            std::string hint = T("COLOR", "COR");
            float hintArrowH = art::IconWidth(art::ICON_LEFT, HINT_ARROW);
            float hintW = HINT_ARROW + 8.0f + Width(FontId::Large, hint, HINT_CELL), hx = VIRTUAL_W * 0.5f - hintW * 0.5f;
            float top = HINT_Y + HINT_CELL * 0.5f - hintArrowH - 1.0f;
            art::DrawIconTurned(g_canvas, art::ICON_LEFT, v.X(hx), v.Y(top), HINT_ARROW * v.s, true);
            art::DrawIconTurned(g_canvas, art::ICON_LEFT, v.X(hx), v.Y(top + hintArrowH + 2.0f), HINT_ARROW * v.s, false);
            Draw(v, FontId::Large, hint, hx + HINT_ARROW + 8.0f, HINT_Y, HINT_CELL, HINT);
            AddZone(hx - 10.0f, HINT_Y - 8.0f, hx + hintW + 10.0f, HINT_Y + HINT_CELL + 8.0f, Hit::Down);
        }

        bool PanelWanted()
        {
            return g_node == NODE_HUB && lobby::GetState() == lobby::State::Hosting && !lobby::HostInGame();
        }

        // True while something is drawn.
        bool Update()
        {
            uint64_t now = plat::TickCountMs();
            if (g_requested.exchange(false) && !g_open)
            {
                bool resume = g_resumeOpen.exchange(false) && lobby::GetState() == lobby::State::Joined;
                if (!resume && lobby::GetState() != lobby::State::Off)
                    lobby::Leave();  // a session left behind (the host went back to the main menu)
                g_palette = &CLOTH;
                g_lobbyModal = false;
                // The saved name, for the session; the first time (none saved, no CRASH_MP_NAME) the
                // name screen comes first.
                lobby::SetLocalName(settings::Get().mpName);
                if (resume)
                    Go(Screen::Waiting);
                else if (NameAsked())
                    Go(Screen::Root, ROOT_HOST);
                else
                    BeginField(Screen::Name, lobby::LocalName());
                Open();
                Log("[mp] menu opened%s", resume ? " at the lobby (back from the host's level)" : "");
            }
            if (!g_open && g_levelMenuRequested.exchange(false))
            {
                g_portuguese = settings::Get().language.rfind("pt", 0) == 0;
                g_palette = &DIMMED;
                g_lobbyModal = false;
                g_levelModal = true;
                Go(Screen::LevelMenu);
                Open();
                g_openedAt = now - OPEN_DELAY_MS;
                Log("[mp] menu: the client's menu in the level (the game goes on)");
            }
            bool panel = PanelWanted(), picker = PickerWanted();
            // The host back at the HUB from SKIN AND COLOR: its lobby again.
            if (!g_open && panel && g_reopenLobby.exchange(false))
            {
                g_palette = &DIMMED;
                g_lobbyModal = true;
                Go(Screen::Lobby, 0);
                Open();
                g_openedAt = now - OPEN_DELAY_MS;
                Log("[mp] lobby opened again at the HUB (after SKIN AND COLOR)");
            }
            if (!g_open && !g_releasing && !g_closing && !panel && !picker)
            {
                g_panelShown = false;
                g_pickerShown = false;
                return false;
            }
            uint16_t buttons = input::MenuButtons();
            if (!g_open)
            {
                uint16_t pressed = Pressed(buttons) | ((panel && g_panelShown) || (picker && g_pickerShown) ? MouseButtons() : 0);
                if (picker && (pressed & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN)))
                {
                    colors::Step(pressed & XINPUT_GAMEPAD_DPAD_UP ? -1 : 1);
                    g_dirty = true;
                }
                g_pickerShown = picker;
                if (g_releasing && !buttons && now >= g_backUntil)
                    g_releasing = false;
                if (g_closing && now - g_closedAt >= FADE_OUT_MS)
                    g_closing = false;
                // The HUB panel: BACK opens the lobby.
                if (panel && !g_releasing && (pressed & XINPUT_GAMEPAD_BACK))
                {
                    g_palette = &DIMMED;
                    g_lobbyModal = true;
                    Go(Screen::Lobby);
                    Open();
                    g_openedAt = now - OPEN_DELAY_MS;
                    Log("[mp] lobby opened at the HUB");
                }
                g_panelShown = panel;
                if (now - g_lastDraw >= REDRAW_MS)
                    g_dirty = true;
                return panel || picker || g_closing;
            }
            g_panelShown = false;
            if (g_started.exchange(false))
            {
                Close(false);
                return true;
            }
            if (g_lobbyModal && (!panel || g_node != NODE_HUB))
            {
                Close(false);  // the level started, or the session closed
                return true;
            }
            if (g_levelModal && (!g_clientInLevel || g_leaving != Leaving::None))
            {
                Close(false);  // leaving the level anyway
                return true;
            }
            if (now - g_openedAt < OPEN_DELAY_MS)
            {
                g_previousButtons = buttons;
                g_mouseMoves = input::pc::CursorMoves(), g_mouseClicks = input::pc::LeftClicks();
                return true;
            }
            uint16_t pressed = Pressed(buttons) | MouseButtons();
            if (pressed || !g_tapped.empty())
                g_dirty = true;
            if (g_levelModal)
                UpdateLevelMenu(pressed);
            else if (g_lobbyModal)
                UpdateLobby(pressed);
            else
                UpdateFull(pressed);
            if (now - g_lastDraw >= REDRAW_MS)
                g_dirty = true;
            return true;
        }

        void Render(int width, int height)
        {
            if (width != g_drawnW || height != g_drawnH)
            {
                art::ClearCache();
                g_drawnW = width, g_drawnH = height;
                g_dirty = true;
            }
            if (!g_dirty)
                return;
            g_canvas.Reset(width, height);
            g_zones.clear();
            View v = MakeView(width, height);
            if (g_open && g_lobbyModal)
                RasterizeLobby(v, width, height);
            else if (g_open && g_levelModal)
                RasterizeLevelMenu(v);
            else if (g_open || g_closing)
            {
                if (!g_lobbyModal)
                    RasterizeFull(v, width, height);
            }
            if (!g_open && g_panelShown)
                RasterizePanel(v);
            if (!g_open && g_pickerShown)
                RasterizePicker(v);
            g_canvas.Export(g_pixels);
            g_version++;
            g_dirty = false;
            g_lastDraw = plat::TickCountMs();
        }
    }

    void RequestOpen()
    {
        if (!g_drawing)
        {
            Log("[mp] the MULTIPLAYER screens need the renderer; ignored");
            return;
        }
        g_requested = true;
    }

    bool ReopensLobby() { return g_resumeOpen; }

    bool BlocksGameInput()
    {
        return g_open || g_releasing || g_requested;
    }

    uint16_t GameButtons()
    {
        return plat::TickCountMs() < g_backUntil ? XINPUT_GAMEPAD_B : 0;
    }

    uint16_t HiddenButtons()
    {
        return (g_panelShown ? XINPUT_GAMEPAD_BACK : 0) | (g_pickerShown ? XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN : 0);
    }

    bool PickerOpen() { return g_picker && g_node == NODE_SKINS; }

    int PreviewColour() { return PickerOpen() ? int(colors::Mine()) : -1; }

    int32_t OverrideExit(PPCContext& ctx, uint8_t* base, uint32_t tree, uint32_t node, int32_t exit)
    {
        uint32_t previous = g_node.exchange(node);
        lobby::State state = lobby::GetState();
        // Host: NEW / LOAD GAME leave the options screen through the main menu's own exits.
        if (node == NODE_OPTIONS && !g_open && g_forceExit >= 0)
        {
            int32_t forced = g_forceExit.exchange(-1);
            Log("[mp] menu: exit %d from the options screen (%s)", forced, forced == EXIT_NEW_GAME ? "new game" : "load game");
            return forced;
        }
        if (node == NODE_MAIN_MENU)
            g_forceExit = -1;
        // A hosted new game: from its last screen to the HUB instead of the first level.
        if (node == NODE_NEW_GAME_FADE && exit == EXIT_FIRST_LEVEL && g_hostNewGame.exchange(false) && state == lobby::State::Hosting)
        {
            Log("[mp] menu: hosted new game: to the HUB (exit %d instead of %d)", EXIT_TO_HUB, exit);
            return EXIT_TO_HUB;
        }
        // Client in a level: START opens its own menu over the game instead of the game's pause.
        if (g_clientInLevel && state == lobby::State::Joined && node == NODE_GAME_RUNNING && exit == EXIT_PAUSE)
        {
            if (!g_open && !g_releasing && g_leaving == Leaving::None)
                g_levelMenuRequested = true;
            return -1;
        }
        // The host back at the main menu after its HUB: the session closes (the clients go to theirs).
        if (state == lobby::State::Hosting)
        {
            if (node == NODE_HUB)
                g_hostReachedHub = true;
            else if (node == NODE_MAIN_MENU && g_hostReachedHub.exchange(false))
            {
                Log("[mp] menu: the host is back at the main menu: the session closes");
                lobby::Leave();
                state = lobby::GetState();
            }
        }
        // Pause in a session: the game's DROP OUT leaves the player out of a co-op level (a black screen
        // waiting for B); online it is EXIT LEVEL (with the game's confirmation).
        if ((exit == EXIT_DROP_OUT || exit == EXIT_DROP_OUT_NO_SAVE) && (state == lobby::State::Hosting || state == lobby::State::Joined))
        {
            Log("[mp] menu: DROP OUT is EXIT LEVEL online");
            exit = exit == EXIT_DROP_OUT ? EXIT_LEAVE_LEVEL : EXIT_LEAVE_LEVEL_NO_SAVE;
        }
        // Host: EXIT LEVEL takes the clients to the HUB at once (the host saves and goes to its HUB).
        if (state == lobby::State::Hosting && node == NODE_PAUSE && exit == EXIT_SAVE_AND_EXIT)
            lobby::HostBackInMenus();
        // Client: only the host keeps a save. Out of a level by the end screen's way to the HUB (no save
        // screen), then from the HUB to the main menu: after its own QUIT (or a closed session) the main menu
        // itself, after the host's return to its HUB the lobby's wait (the host's next level is followed from there).
        if (g_clientInLevel)
        {
            for (int32_t save : SAVE_EXITS)
                if (exit == save)
                {
                    Log("[mp] menu: client: no save (exit %d -> %d)", exit, EXIT_HUB_WITHOUT_SAVING);
                    exit = EXIT_HUB_WITHOUT_SAVING;
                }
            Leaving leaving = g_leaving;
            if (leaving == Leaving::None)
            {
                if (exit == EXIT_SAVE_AND_EXIT || exit == EXIT_EXIT_NO_SAVE || g_quitLevel.exchange(false))
                    leaving = Leaving::Quit;
                else if (state != lobby::State::Joined)
                    leaving = Leaving::Quit;  // the session closed (the host quit, a time-out)
                else if (lobby::TakeHostLeftLevel() || (node == NODE_HUB && !lobby::HostInGame()))
                    leaving = Leaving::ToLobby;
                if (leaving != Leaving::None)
                {
                    g_leaving = leaving;
                    // No more host records on the level being unloaded (its actors go away).
                    net::SetRole(net::Role::None, 0, 1, 0, 0);
                    Log("[mp] menu: client leaving the level (%s)", leaving == Leaving::Quit ? "to the main menu" : "back to the lobby");
                }
            }
            if (leaving != Leaving::None)
            {
                if ((node == NODE_GAME_RUNNING || node == NODE_PAUSE || node == NODE_PAUSE_NO_SAVE) && exit < 0)
                    return EXIT_EXIT_NO_SAVE;  // its music / pause / stats clean-up, then the end screen
                if (node == NODE_LEAVING_LEVEL)
                    return EXIT_HUB_WITHOUT_SAVING;
                if (node == NODE_LEAVING_LEVEL_NO_SAVE)
                    return EXIT_CONTINUE_NO_SAVE;
                if (node == NODE_HUB)
                {
                    g_clientInLevel = false;
                    g_leaving = Leaving::None;
                    if (leaving == Leaving::ToLobby && lobby::GetState() == lobby::State::Joined)
                        g_resumeLobby = true;
                    else
                        lobby::Leave();
                    Log("[mp] menu: client at the HUB: to the main menu%s (exit %d)", g_resumeLobby ? " and the lobby" : "",
                        EXIT_HUB_TO_MAIN_MENU);
                    return EXIT_HUB_TO_MAIN_MENU;
                }
            }
        }
        // The lobby over the options screen, as from the MULTIPLAYER item (a level the host starts
        // meanwhile is followed from there).
        if (node == NODE_MAIN_MENU && g_resumeLobby.exchange(false) && lobby::GetState() == lobby::State::Joined)
        {
            g_resumeOpen = true;
            Log("[mp] menu: back in the lobby (exit %d)", EXIT_SETTINGS);
            return EXIT_SETTINGS;
        }
        // SKIN AND COLOR: the game's skin screen from the host's HUB or a client's wait, showing the skin worn.
        if (g_pickerRequest && (node == NODE_HUB || node == NODE_OPTIONS) && (state == lobby::State::Hosting || state == lobby::State::Joined))
        {
            g_pickerRequest = false;
            // As the HUB's doll does (Frontend:SetSkinToDisplay): the skin the screen starts on, in the
            // frontend manager (managers subsystem 6) +16640.
            int skin = dev::skins::Current(ctx, base);
            PPCContext call = ctx;
            sub_821FB4F0(call, base);
            call.r4.u64 = 6;
            sub_821FA440(call, base);
            if (call.r3.u32 >= 0x40000000)
                StoreBE32(call.r3.u32 + FRONTEND_SKIN_TO_DISPLAY, uint32_t(skin >= 0 && skin != dev::skins::HULK ? skin : dev::skins::CRASH));
            g_picker = true;
            Log("[mp] menu: SKIN AND COLOR: the skin screen (exit %d from node %u, skin %d)", EXIT_TO_SKINS, node, skin);
            return EXIT_TO_SKINS;
        }
        if (g_picker && node == NODE_SKINS && (exit == EXIT_SKIN_CONFIRMED || exit == EXIT_SKIN_BACK))
        {
            g_picker = false;
            if (g_pickerHost || state != lobby::State::Joined)
            {
                g_reopenLobby = state == lobby::State::Hosting;
                return exit;  // to the HUB (ExitConfirmed wears the skin)
            }
            // A client: the skin worn here (the exit's own SelectSkin would go to the HUB), then the wait.
            if (exit == EXIT_SKIN_CONFIRMED)
            {
                int skin = int32_t(LoadBE32(SHOWN_SKIN));
                Log("[mp] menu: SKIN AND COLOR: %s", dev::skins::Select(ctx, base, skin, false, true).c_str());
            }
            g_resumeOpen = true;
            Log("[mp] menu: SKIN AND COLOR: back to the wait (exit %d)", EXIT_SETTINGS);
            return EXIT_SETTINGS;
        }
        // Host: the level start the clients follow, and the way back.
        if (state == lobby::State::Hosting)
        {
            if (node == NODE_IN_GAME && previous != NODE_IN_GAME && !lobby::HostInGame())
                lobby::HostLevelStarted(int32_t(LoadBE32(MANAGERS + LEVEL_INDEX)));
            else if (node == NODE_HUB && lobby::HostInGame())
                lobby::HostBackInMenus();
        }
        // Client: the host started a level.
        if (state == lobby::State::Joined && exit < 0 && (node == NODE_OPTIONS || node == NODE_MAIN_MENU || node == NODE_HUB || node == NODE_SKINS))
        {
            int32_t level;
            if (lobby::TakeStart(level))
            {
                PPCContext call = ctx;
                call.r3.u64 = tree;
                call.r4.s64 = level;
                sub_820F5D58(call, base);
                g_started = true;
                g_picker = false;
                g_clientInLevel = true;
                g_leaving = Leaving::None;
                g_quitLevel = false;
                lobby::TakeHostLeftLevel();  // an older one
                Log("[mp] menu: following the host into level %d (StartLevel + SkipToLevel from node %u)", level, node);
                return EXIT_SKIP_TO_LEVEL;
            }
        }
        return exit;
    }

    void Draw(uint32_t width, uint32_t height)
    {
        g_drawing = true;
        if (!Update() || !width || !height)
            return;
        Render(int(width), int(height));
        uint64_t now = plat::TickCountMs();
        float t = 1.0f;
        if (g_closing && !g_open)
            t = 1.0f - float(now - g_closedAt) / FADE_OUT_MS;
        else if (g_open && !g_lobbyModal && !g_levelModal)
            t = (float(now - g_openedAt) - OPEN_DELAY_MS) / FADE_IN_MS;
        t = std::clamp(t, 0.0f, 1.0f);
        if (t <= 0.0f)
            return;
        float eased = 1.0f - (1.0f - t) * (1.0f - t);
        const uint32_t* pixels = g_pixels.data();
        uint64_t version = g_version;
        if (t < 1.0f)
        {
            uint32_t k = uint32_t(std::lround(eased * 256.0f));
            g_faded.resize(g_pixels.size());
            for (size_t i = 0; i < g_pixels.size(); i++)
                g_faded[i] = (g_pixels[i] & 0x00FFFFFFu) | (((g_pixels[i] >> 24) * k >> 8) << 24);
            pixels = g_faded.data();
            version = (1ull << 63) | ++g_fadedVersion;
        }
        gpu::overlay::DrawBitmap(width, height, pixels, int(width), int(height), 0, 0, version, gpu::overlay::BITMAP_MULTIPLAYER);
    }
}

// Frontend:PauseGame(paused) (r4): in a session the game never stops. Its pause (the host's), the
// tutorials and the other screens GameRunning opens show over the running level: the other machines go
// on with it.
PPC_FUNC(sub_820F8918)
{
    mp::lobby::State state = mp::lobby::GetState();
    bool session = state == mp::lobby::State::Hosting || state == mp::lobby::State::Joined;
    if (ctx.r4.u8 && session && mp::menu::g_node == mp::menu::NODE_GAME_RUNNING)
    {
        static int logged;
        if (logged++ < 3)
            Log("[mp] menu: PauseGame(true) skipped: the session goes on");
        return;
    }
    __imp__sub_820F8918(ctx, base);
}

// CFrontendManager::HaveBothPlayersJoinedGame: the frontend's two-player variants ask it, the pause's page
// (COND TwoPlayer / OnePlayer, sub_820F6420, chosen once when the pause opens) and its exits (node 1081 in
// sub_820F9BA8). In a session the host gets the one-player pause (RESUME, OPTIONS, EXIT LEVEL: save and exit,
// the clients back to the lobby) instead of the two-player one with DROP OUT. Not for UpdateInputAndCheats
// (sub_821DB680): there "not both joined" lets another controller drop in as player 2.
static thread_local bool t_inputUpdate = false;

PPC_FUNC(sub_821DB680)
{
    t_inputUpdate = true;
    __imp__sub_821DB680(ctx, base);
    t_inputUpdate = false;
}

PPC_FUNC(sub_821DAAB0)
{
    mp::lobby::State state = mp::lobby::GetState();
    if (!t_inputUpdate && (state == mp::lobby::State::Hosting || state == mp::lobby::State::Joined))
    {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_821DAAB0(ctx, base);
}
