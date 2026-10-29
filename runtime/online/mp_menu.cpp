// The MULTIPLAYER screens (mp_menu.h), drawn like the settings screen (settings/settings_menu.cpp):
// the game's fonts and frames (settings/menu_art.h) in a virtual 1280x720 screen scaled to the window.
// Controller, keyboard, and the mouse / touch (Android's menus are touch as a mouse): a click on a
// row chooses it, on the game's B BACK / SELECT A hints is that button, on an option's left / right
// half changes it, on the HUB panel opens the lobby; code and password have keys on the screen.

#include "mp_menu.h"
#include "mp.h"
#include "mp_lobby.h"
#include "../diagnostics.h"
#include "../gpu/overlay.h"
#include "../kernel/input.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../kernel/pc_input.h"
#include "../kernel/xinput_types.h"
#include "../platform/platform.h"
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

        enum class Screen { Root, HostMode, FindMode, Servers, Code, Password, Waiting, Message, Lobby, HostPassword, LevelMenu };
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
        std::atomic<uint32_t> g_node{ UINT32_MAX };
        std::atomic<bool> g_panelShown{ false };  // the HUB panel is on screen

        // Render thread.
        Screen g_screen = Screen::Root;
        int g_row = 0;
        bool g_portuguese = false;
        std::string g_field;      // the code or password being typed
        size_t g_cursor = 0;
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
        enum class Hit : uint8_t { Row, RowLeft, RowRight, Back, Accept, Key, Erase, Confirm, Panel, Anywhere };
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
        constexpr size_t SHORT_CODE_LENGTH = 7, CODE_LENGTH = 10, PASSWORD_LENGTH = 12;  // codes: mp_lobby.h

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

        float Draw(const View& v, FontId font, const std::string& text, float x, float y, float cell, const TextStyle& style,
                   float align = 0.0f, float maxWidth = 1e9f)
        {
            std::u32string u = art::Utf32(text);
            float width = art::TextWidth(font, u, cell);
            if (width > maxWidth && width > 0.0f)
            {
                float fitted = cell * maxWidth / width;
                y += (cell - fitted) * 0.5f;
                cell = fitted, width = maxWidth;
            }
            float left = x - width * align;
            art::DrawGameText(g_canvas, font, u, v.X(left), v.Y(y), cell * v.s, style);
            return left + width;
        }

        // MSG_Background mirrored into the four corners (as the settings' dialogs).
        void DrawFrame(const View& v, float x0, float y0, float x1, float y1)
        {
            const art::Image* corner = art::GetArt(art::Art::MessageFrame);
            int left = v.IX(x0), top = v.IY(y0), right = v.IX(x1), bottom = v.IY(y1);
            if (!corner)
            {
                g_canvas.Fill(left, top, right, bottom, { 0, 0, 0, 170 });
                return;
            }
            int halfW = (right - left) / 2, halfH = (bottom - top) / 2;
            float k = float(halfH) / float(corner->h);
            int curl = corner->w / 3;
            int curlW = std::min(int(std::lround(curl * k)), halfW);
            for (int q = 0; q < 4; q++)
            {
                bool flipX = q & 1, flipY = q & 2;
                int qx = flipX ? right - halfW : left, qy = flipY ? bottom - halfH : top;
                const art::Image& c = art::Scaled(*corner, 0, 0, curl, corner->h, curlW, halfH, flipX, flipY);
                const art::Image& edge = art::Scaled(*corner, curl, 0, corner->w - curl, corner->h, halfW - curlW, halfH, flipX, flipY);
                g_canvas.Blit(c, flipX ? qx + halfW - curlW : qx, qy, FRAME);
                g_canvas.Blit(edge, flipX ? qx : qx + curlW, qy, FRAME);
            }
        }

        std::string Upper(std::string text)
        {
            for (char& c : text)
                c = char(toupper(uint8_t(c)));
            return text;
        }

        std::vector<std::string> Rows()
        {
            switch (g_screen)
            {
            case Screen::Root: return { T("HOST GAME", "CRIAR PARTIDA"), T("FIND GAME", "ENCONTRAR PARTIDA") };
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
            case Screen::Waiting:
            case Screen::Lobby: return "LOBBY";
            default: return "MULTIPLAYER";
            }
        }

        // The field being typed: the code as XXXX-XXX (XXXXX-XXXXX once it has more than 7 letters), the
        // password as is; '_' where letters are missing, the one under the cursor in the selected colour.
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

        // Keys for the mouse / touch: the field's letters, ERASE and OK, 10 a row from `top`.
        constexpr float KEY_W = 64.0f, KEY_H = 46.0f, KEY_PITCH_X = 70.0f, KEY_PITCH_Y = 52.0f;

        void DrawKeys(const View& v, const Palette& pal, float top, bool code)
        {
            std::string letters = code ? CODE_LETTERS : PASSWORD_LETTERS;
            int count = int(letters.size()) + 2;
            float left = VIRTUAL_W * 0.5f - (10 * KEY_PITCH_X - (KEY_PITCH_X - KEY_W)) * 0.5f;
            for (int i = 0; i < count; i++)
            {
                float x = left + (i % 10) * KEY_PITCH_X, y = top + (i / 10) * KEY_PITCH_Y;
                bool erase = i == count - 2, ok = i == count - 1;
                float w = KEY_W;
                g_canvas.Fill(v.IX(x), v.IY(y), v.IX(x + w), v.IY(y + KEY_H), { 0, 0, 0, 70 });
                std::string label = erase ? std::string("\xC2\xB3") : ok ? std::string("OK") : std::string(1, letters[size_t(i)]);
                Draw(v, FontId::Small, label, x + w * 0.5f, y + 7.0f, 32.0f, ok ? pal.selected : pal.normal, 0.5f, w - 6.0f);
                AddZone(x, y, x + w, y + KEY_H, erase ? Hit::Erase : ok ? Hit::Confirm : Hit::Key, erase || ok ? 0 : letters[size_t(i)]);
            }
        }

        void DrawHelp(const View& v, const Palette& pal, const std::vector<std::string>& lines, float y = HELP_Y)
        {
            for (const std::string& line : lines)
            {
                Draw(v, FontId::Small, line, VIRTUAL_W * 0.5f, y, HELP_CELL, pal.help, 0.5f, 980.0f);
                y += HELP_CELL + 8.0f;
            }
        }

        void DrawRoster(const View& v, const Palette& pal, float y, bool host)
        {
            std::vector<lobby::Player> players = lobby::Players();
            for (size_t i = 0; i < players.size(); i++)
            {
                const lobby::Player& p = players[i];
                char line[96];
                snprintf(line, sizeof(line), "%u  %s", p.slot + 1, p.name.c_str());
                bool selected = host && g_lobbyModal && g_row == int(i) && p.slot != 0;
                const TextStyle& style = selected || (p.me && !host) ? pal.selected : pal.normal;
                Draw(v, FontId::Small, line, 400.0f, y, LIST_CELL, style, 0.0f, 420.0f);
                std::string right = p.slot == 0 ? std::string("HOST") : std::to_string(p.pingMs) + " ms";
                if (p.me && p.slot != 0)
                    right = std::string(T("YOU", "VOCÊ")) + "  " + right;
                Draw(v, FontId::Small, right, 880.0f, y, LIST_CELL, style, 1.0f);
                if (host && p.slot != 0)
                    AddZone(380.0f, y - 6.0f, 900.0f, y + LIST_CELL + 6.0f, Hit::Row, int(i), true);
                y += LIST_PITCH;
            }
        }

        // Lobby (host, at the HUB): its rows after the players.
        enum LobbyRow { ROW_PASSWORD, ROW_MAX_PLAYERS, ROW_KNOCK_OUT, ROW_CLOSE, LOBBY_ROWS };

        int LobbyPlayerRows() { return int(lobby::Players().size()); }

        std::string LobbyRowText(int row)
        {
            lobby::Options o = lobby::GetOptions();
            switch (row)
            {
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

        void RasterizeFull(const View& v, int width, int height)
        {
            const Palette& pal = *g_palette;
            if (g_palette == &DIMMED)
                g_canvas.FillGradient(0, 0, width, height, DIM_TOP, DIM_BOTTOM);
            Draw(v, FontId::Large, Title(), VIRTUAL_W * 0.5f, TITLE_Y, TITLE_CELL, pal.title, 0.5f);
            if (g_screen != Screen::Message)
                AddFooterZones();
            std::vector<std::string> rows = Rows();
            if (!rows.empty())
            {
                for (size_t i = 0; i < rows.size(); i++)
                {
                    float y = ROWS_Y + 40.0f + i * ROW_PITCH;
                    float right = Draw(v, FontId::Large, rows[i], VIRTUAL_W * 0.5f, y, ROW_CELL, int(i) == g_row ? pal.selected : pal.normal, 0.5f);
                    float left = VIRTUAL_W - right;
                    AddZone(left - 20.0f, y - 6.0f, right + 20.0f, y + ROW_CELL + 6.0f, Hit::Row, int(i));
                }
                std::string help = g_screen == Screen::Root
                                       ? T("Up to 4 players: one hosts, the others find the game in the list (same network) or by its code.",
                                           "Até 4 jogadores: um cria a partida, os outros a encontram na lista (mesma rede) ou pelo código.")
                                   : g_screen == Screen::HostMode
                                       ? T("Start or load your game: the players wait for you at the HUB.",
                                           "Comece ou carregue o seu jogo: os jogadores esperam você no HUB.")
                                       : T("The list shows the games on this network; a code also works over the internet.",
                                           "A lista mostra as partidas desta rede; o código serve também pela internet.");
                DrawHelp(v, pal, { help });
                if (g_screen == Screen::HostMode || g_screen == Screen::Root)
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
                         ROWS_Y + 40.0f, LIST_CELL, pal.info, 0.5f);
                g_row = std::min(g_row, std::max(0, int(servers.size()) - 1));
                float y = ROWS_Y;
                for (size_t i = 0; i < servers.size() && i < 7; i++)
                {
                    const lobby::Server& s = servers[i];
                    const TextStyle& style = int(i) == g_row ? pal.selected : pal.normal;
                    Draw(v, FontId::Small, s.name, 300.0f, y, LIST_CELL, style, 0.0f, 360.0f);
                    std::string state = std::to_string(s.players) + "/" + std::to_string(s.maxPlayers);
                    if (s.locked)
                        state += std::string("  ") + T("PASSWORD", "SENHA");
                    if (s.inGame)
                        state += std::string("  ") + T("IN A LEVEL", "EM FASE");
                    Draw(v, FontId::Small, state, 980.0f, y, LIST_CELL, style, 1.0f);
                    AddZone(280.0f, y - 6.0f, 1000.0f, y + LIST_CELL + 6.0f, Hit::Row, int(i));
                    y += LIST_PITCH;
                }
                DrawHelp(v, pal, { T("A: join    B: back", "A: entrar    B: voltar") });
                break;
            }
            case Screen::Code:
            case Screen::Password:
            {
                bool code = g_screen == Screen::Code;
                Draw(v, FontId::Small, code ? T("The host's code (in its HUB panel)", "O código do host (no painel do HUB dele)")
                                            : T("This game has a password", "Esta partida tem senha"),
                     VIRTUAL_W * 0.5f, SUB_Y + 4.0f, SUB_CELL, pal.help, 0.5f);
                DrawField(v, pal, ROWS_Y - 40.0f, code);
                DrawKeys(v, pal, ROWS_Y + 30.0f, code);
                DrawHelp(v, pal, { T("Type it, touch the keys, or D-pad up / down: letter, left / right: move, X: erase",
                                     "Digite, toque nas teclas, ou direcional cima / baixo: letra, esquerda / direita: mover, X: apagar"),
                                   T("A / Enter / OK: join    B: back", "A / Enter / OK: entrar    B: voltar") }, HELP_Y + 4.0f);
                break;
            }
            case Screen::Waiting:
            {
                lobby::State state = lobby::GetState();
                if (state == lobby::State::Joining)
                {
                    Draw(v, FontId::Small, std::string(T("Connecting to ", "Conectando a ")) + lobby::Address() + "...", VIRTUAL_W * 0.5f, ROWS_Y,
                         SUB_CELL, pal.help, 0.5f);
                }
                else
                {
                    Draw(v, FontId::Small, std::string(T("Code ", "Código ")) + lobby::InviteCode(), VIRTUAL_W * 0.5f, SUB_Y, SUB_CELL, pal.help, 0.5f);
                    DrawRoster(v, pal, ROWS_Y, false);
                    DrawHelp(v, pal, { lobby::HostInGame() ? T("The host is in a level: you join the next one it starts.",
                                                               "O host está numa fase: você entra na próxima que ele começar.")
                                                           : T("Waiting for the host to start a level...", "Esperando o host começar uma fase...") });
                }
                DrawHelp(v, pal, { T("B: leave", "B: sair") }, HELP_Y + 40.0f);
                break;
            }
            case Screen::Message:
                AddZone(0.0f, 0.0f, VIRTUAL_W, VIRTUAL_H, Hit::Anywhere);
                DrawFrame(v, 260.0f, 220.0f, 1020.0f, 420.0f);
                Draw(v, FontId::Small, g_message, VIRTUAL_W * 0.5f, 270.0f, SUB_CELL, DIMMED.warning, 0.5f, 680.0f);
                Draw(v, FontId::Small, std::string("\xC2\xB1 OK"), VIRTUAL_W * 0.5f, 340.0f, SUB_CELL, DIMMED.selected, 0.5f);
                break;
            default: break;
            }
        }

        // Host, at the HUB: the lobby (modal) or the panel.
        void RasterizeLobby(const View& v, int width, int height)
        {
            const Palette& pal = DIMMED;
            int players = LobbyPlayerRows();
            g_canvas.FillGradient(0, 0, width, height, DIM_TOP, DIM_BOTTOM);  // the HUB's own texts behind
            if (g_screen == Screen::HostPassword)
            {
                DrawFrame(v, 240.0f, 90.0f, 1040.0f, 560.0f);
                Draw(v, FontId::Large, T("PASSWORD", "SENHA"), VIRTUAL_W * 0.5f, 116.0f, 38.0f, pal.title, 0.5f);
                DrawField(v, pal, 170.0f, false);
                DrawKeys(v, pal, 236.0f, false);
                std::string back = std::string("\xC2\xB2 ") + T("BACK", "VOLTAR");
                float end = Draw(v, FontId::Small, back, 330.0f, 466.0f, HELP_CELL, pal.help);
                AddZone(320.0f, 458.0f, end + 10.0f, 466.0f + HELP_CELL + 8.0f, Hit::Back);
                Draw(v, FontId::Small, T("Empty = no password", "Vazia = sem senha"), 950.0f, 466.0f, HELP_CELL, pal.info, 1.0f);
                return;
            }
            float top = 80.0f, bottom = top + 230.0f + (players + int(LOBBY_ROWS)) * LIST_PITCH;
            DrawFrame(v, 300.0f, top, 980.0f, bottom);
            Draw(v, FontId::Large, "MULTIPLAYER", VIRTUAL_W * 0.5f, top + 26.0f, 38.0f, pal.title, 0.5f);
            std::string code = lobby::InviteCode(), local = lobby::LocalCode();
            Draw(v, FontId::Small, std::string(T("Code ", "Código ")) + code + (code != local ? std::string(T("   LAN ", "   rede local ")) + local : "   " + lobby::Address()),
                 VIRTUAL_W * 0.5f, top + 74.0f, 26.0f, pal.info, 0.5f, 620.0f);
            Draw(v, FontId::Small, lobby::InternetText(g_portuguese), VIRTUAL_W * 0.5f, top + 104.0f, 22.0f, pal.help, 0.5f, 620.0f);
            float y = top + 146.0f;
            DrawRoster(v, pal, y, true);
            y += players * LIST_PITCH;
            for (int row = 0; row < LOBBY_ROWS; row++)
            {
                Draw(v, FontId::Small, LobbyRowText(row), VIRTUAL_W * 0.5f, y, LIST_CELL, g_row == players + row ? pal.selected : pal.normal, 0.5f, 600.0f);
                if (row == ROW_MAX_PLAYERS || row == ROW_KNOCK_OUT)
                {
                    AddZone(380.0f, y - 6.0f, VIRTUAL_W * 0.5f, y + LIST_CELL + 6.0f, Hit::RowLeft, players + row);
                    AddZone(VIRTUAL_W * 0.5f, y - 6.0f, 900.0f, y + LIST_CELL + 6.0f, Hit::RowRight, players + row);
                }
                else
                    AddZone(380.0f, y - 6.0f, 900.0f, y + LIST_CELL + 6.0f, Hit::Row, players + row, row == ROW_CLOSE);
                y += LIST_PITCH;
            }
            std::string help = g_row < players ? T("A: kick", "A: expulsar") : T("A / left / right: change", "A / esquerda / direita: mudar");
            std::string back = std::string("\xC2\xB2 ") + T("BACK", "VOLTAR");
            float end = Draw(v, FontId::Small, back, 390.0f, bottom - 62.0f, HELP_CELL, pal.help);
            AddZone(380.0f, bottom - 70.0f, end + 10.0f, bottom - 62.0f + HELP_CELL + 8.0f, Hit::Back);
            Draw(v, FontId::Small, help, 890.0f, bottom - 62.0f, HELP_CELL, pal.info, 1.0f);
        }

        // A client's menu in a level, over the game (which goes on).
        void RasterizeLevelMenu(const View& v)
        {
            const Palette& pal = DIMMED;
            DrawFrame(v, 340.0f, 180.0f, 940.0f, 540.0f);
            Draw(v, FontId::Large, "MULTIPLAYER", VIRTUAL_W * 0.5f, 206.0f, 38.0f, pal.title, 0.5f);
            const char* rows[LEVEL_MENU_ROWS] = { T("RESUME", "CONTINUAR"), T("SETTINGS", "CONFIGURAÇÕES"), T("QUIT TO THE MAIN MENU", "SAIR PARA O MENU PRINCIPAL") };
            for (int row = 0; row < LEVEL_MENU_ROWS; row++)
            {
                float y = 270.0f + row * ROW_PITCH;
                float right = Draw(v, FontId::Large, rows[row], VIRTUAL_W * 0.5f, y, ROW_CELL, row == g_row ? pal.selected : pal.normal, 0.5f, 540.0f);
                AddZone(VIRTUAL_W - right - 20.0f, y - 6.0f, right + 20.0f, y + ROW_CELL + 6.0f, Hit::Row, row);
            }
            std::string back = std::string("\xC2\xB2 ") + T("BACK TO THE GAME", "VOLTAR AO JOGO");
            float end = Draw(v, FontId::Small, back, 400.0f, 470.0f, HELP_CELL, pal.help);
            AddZone(390.0f, 462.0f, end + 10.0f, 470.0f + HELP_CELL + 8.0f, Hit::Back);
        }

        // The HUB panel's size in virtual units.
        constexpr float PANEL_L = 900.0f, PANEL_T = 14.0f, PANEL_R = 1268.0f, PANEL_PAD = 50.0f;
        float PanelBottom(size_t players) { return PANEL_T + 126.0f + players * 30.0f + 64.0f; }

        void RasterizePanel(const View& v)
        {
            const Palette& pal = DIMMED;
            std::vector<lobby::Player> players = lobby::Players();
            DrawFrame(v, PANEL_L, PANEL_T, PANEL_R, PanelBottom(players.size()));
            AddZone(PANEL_L, PANEL_T, PANEL_R, PanelBottom(players.size()), Hit::Panel);
            float x = PANEL_L + PANEL_PAD, y = PANEL_T + 24.0f;
            Draw(v, FontId::Small, "MULTIPLAYER " + std::to_string(players.size()) + "/" + std::to_string(lobby::GetOptions().maxPlayers), x, y, 26.0f,
                 pal.title, 0.0f, 280.0f);
            y += 34.0f;
            Draw(v, FontId::Small, lobby::InviteCode(), x, y, 28.0f, pal.selected);
            y += 34.0f;
            Draw(v, FontId::Small, lobby::InternetText(g_portuguese), x, y, 20.0f, pal.help, 0.0f, PANEL_R - PANEL_PAD - x);
            y += 32.0f;
            for (const lobby::Player& p : players)
            {
                Draw(v, FontId::Small, std::to_string(p.slot + 1) + " " + p.name, x, y, 24.0f, pal.normal, 0.0f, 200.0f);
                if (p.slot)
                    Draw(v, FontId::Small, std::to_string(p.pingMs) + " ms", PANEL_R - PANEL_PAD, y, 22.0f, pal.info, 1.0f);
                y += 30.0f;
            }
            Draw(v, FontId::Small, std::string("\xC2\xB5 LOBBY"), x, y + 2.0f, 24.0f, pal.help);
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
            }
            return 0;
        }

        bool Typing() { return g_screen == Screen::Code || g_screen == Screen::Password || g_screen == Screen::HostPassword; }

        void Go(Screen screen, int row = 0)
        {
            bool typing = Typing();
            g_screen = screen;
            g_row = row;
            g_dirty = true;
            if (Typing())
            {
                if (!typing)
                {
                    input::pc::SetTextEntry(true);
                    input::pc::TakeTypedText();
                }
            }
            else if (typing)
                input::pc::SetTextEntry(false);
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
            if (Typing())
                input::pc::SetTextEntry(false);
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

        // The field: typed letters and the pad's letter cycling.
        // Returns true when it was confirmed (A / Enter).
        bool EditField(uint16_t pressed, const char* letters, size_t maximum)
        {
            bool confirm = false;
            std::u32string typed = input::pc::TakeTypedText() + g_tapped;
            g_tapped.clear();
            for (char32_t c : typed)
            {
                if (c == U'\r')
                {
                    confirm = true;
                    continue;
                }
                char ch = char(toupper(int(c)));
                if (ch == 'O' && letters == CODE_LETTERS)
                    ch = '0';
                if ((ch == 'I' || ch == 'L') && letters == CODE_LETTERS)
                    ch = '1';
                if (!strchr(letters, ch) || g_field.size() >= maximum)
                    continue;
                g_cursor = std::min(g_cursor, g_field.size());
                if (g_cursor < g_field.size() && g_cursor + 1 == maximum)
                    g_field.erase(g_field.begin() + g_cursor);  // a full field: the last letter is replaced
                g_field.insert(g_field.begin() + g_cursor, ch);
                g_cursor = std::min(g_cursor + 1, maximum - 1);
                g_dirty = true;
            }
            size_t count = strlen(letters);
            auto cycle = [&](int direction) {
                if (g_cursor >= g_field.size())
                {
                    if (g_field.size() >= maximum)
                        return;
                    g_field.push_back(direction > 0 ? letters[0] : letters[count - 1]);
                    return;
                }
                const char* at = strchr(letters, g_field[g_cursor]);
                size_t index = at ? size_t(at - letters) : 0;
                g_field[g_cursor] = letters[(index + count + direction) % count];
            };
            if (pressed & XINPUT_GAMEPAD_DPAD_UP)
                cycle(1);
            if (pressed & XINPUT_GAMEPAD_DPAD_DOWN)
                cycle(-1);
            if ((pressed & XINPUT_GAMEPAD_DPAD_RIGHT) && g_cursor < g_field.size() && g_cursor + 1 < maximum)
                g_cursor++;
            if ((pressed & XINPUT_GAMEPAD_DPAD_LEFT) && g_cursor > 0)
                g_cursor--;
            // X (Backspace): the letter under the cursor, or the last one.
            if ((pressed & XINPUT_GAMEPAD_X) && !g_field.empty())
            {
                g_dirty = true;
                if (g_cursor >= g_field.size())
                {
                    g_field.pop_back();
                    g_cursor = g_field.size();
                }
                else
                    g_field.erase(g_field.begin() + g_cursor);
            }
            if (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START))
                confirm = true;
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
                if (c != '-')
                    g_field += c;
            g_cursor = text.size();
            Go(screen);
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
                else if (accept)
                    Go(g_row == 0 ? Screen::HostMode : Screen::FindMode);
                break;
            case Screen::HostMode:
                if (back)
                    Go(Screen::Root, 0);
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
                    Go(Screen::Root, 1);
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
                    Go(Screen::Root, 1);
                }
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
                if (accept && list[size_t(g_row)].slot != 0)
                    lobby::Kick(list[size_t(g_row)].slot);
                return;
            }
            int row = g_row - players;
            lobby::Options o = lobby::GetOptions();
            if (row == ROW_PASSWORD && accept)
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
                Go(resume ? Screen::Waiting : Screen::Root);
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
            bool panel = PanelWanted();
            if (!g_open && !g_releasing && !g_closing && !panel)
            {
                g_panelShown = false;
                return false;
            }
            uint16_t buttons = input::MenuButtons();
            if (!g_open)
            {
                uint16_t pressed = Pressed(buttons) | (panel && g_panelShown ? MouseButtons() : 0);
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
                return panel || g_closing;
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
        return g_panelShown ? XINPUT_GAMEPAD_BACK : 0;
    }

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
        // Host: the level start the clients follow, and the way back.
        if (state == lobby::State::Hosting)
        {
            if (node == NODE_IN_GAME && previous != NODE_IN_GAME && !lobby::HostInGame())
                lobby::HostLevelStarted(int32_t(LoadBE32(MANAGERS + LEVEL_INDEX)));
            else if (node == NODE_HUB && lobby::HostInGame())
                lobby::HostBackInMenus();
        }
        // Client: the host started a level.
        if (state == lobby::State::Joined && exit < 0 && (node == NODE_OPTIONS || node == NODE_MAIN_MENU || node == NODE_HUB))
        {
            int32_t level;
            if (lobby::TakeStart(level))
            {
                PPCContext call = ctx;
                call.r3.u64 = tree;
                call.r4.s64 = level;
                sub_820F5D58(call, base);
                g_started = true;
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
