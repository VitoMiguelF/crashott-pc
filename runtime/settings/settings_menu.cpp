#include "direct_boot.h"
#include "settings_menu.h"
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
#include "../kernel/pc_input.h"
#include "../subtitles/subtitles.h"

#include <windows.h>
#include <xinput.h>

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
        constexpr ULONGLONG OPEN_DELAY_MS = 120, FADE_IN_MS = 220, FADE_OUT_MS = 110, BACK_PRESS_MS = 200;
        constexpr float RISE = 18.0f;  // virtual pixels
        constexpr ULONGLONG RESTART_DELAY_MS = 250;  // "restarting..." on screen before the restart

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

        constexpr ULONGLONG REPEAT_DELAY_MS = 380;
        constexpr ULONGLONG REPEAT_MS = 90;

        enum class Item
        {
            Master, Music, Effects, Voice,
            Window, Resolution, FpsLimit, VSync, AntiAliasing, Overlay,
            Upscaler, UpscalerQuality, Sharpness, MotionBlur, DepthOfField, Anisotropy, ShadowSoftness, ShadowResolution, HdRemaster,
            Language, MojoColors, CoopMode, InvertAxis, Autosave, SkipCutscenes, Storage,
            Subtitles, SubtitleSize, SubtitleBackground,
            FreeCamera, CameraSensitivity, CameraInvertY, CameraDistance,
            LightSsao, LightGi, LightGiIntensity, LightContactShadows, LightSoftShadows,
            Prompts, MouseCamera, MouseSensitivity, MouseInvertY, WalkSpeed, MouseMenus,
            DevConsole,
            HeaderSubtitles, HeaderCamera, HeaderLighting, HeaderDev,  // sub-group headings inside a tab (not selectable)
            Bind0,  // + settings::Action: the key bindings (controls.h), one row per action
        };

        bool IsHeader(Item item)
        {
            return item == Item::HeaderSubtitles || item == Item::HeaderCamera || item == Item::HeaderLighting || item == Item::HeaderDev;
        }

        // The CONTROLS tab's binding rows.
        bool IsBinding(Item item)
        {
            return int(item) >= int(Item::Bind0) && int(item) < int(Item::Bind0) + int(ACTION_COUNT);
        }

        Action ActionOf(Item item)
        {
            return Action(int(item) - int(Item::Bind0));
        }

        std::vector<Item> ControlItems()
        {
            std::vector<Item> items = { Item::Prompts, Item::MouseCamera, Item::MouseSensitivity, Item::MouseInvertY, Item::WalkSpeed,
                                        Item::MouseMenus };
            for (int i = 0; i < int(ACTION_COUNT); i++)
                items.push_back(Item(int(Item::Bind0) + i));
            return items;
        }

        // A tab shows at most this many rows; longer ones (CONTROLS) scroll with the selection.
        constexpr size_t MAX_ROWS = 9;

        // English and Brazilian Portuguese (the screen follows the language chosen in it).
        struct Text
        {
            const char* en;
            const char* pt;
        };

        // The tabs, switched with LB / RB (Q / E on the keyboard); the screen opens on the first.
        // The `configuracoes*` and `controles_tela` profiles of config/run.toml rely on the order:
        // GAME (LANGUAGE, CO-OP, ...), AUDIO, VIDEO (RESOLUTION 2nd row, ANTI-ALIASING 5th),
        // GRAPHICS (DLSS / FSR first), CONTROLS, DEFINITIVE EDITION (the port's own extras). DOWN
        // skips the sub-group headings.
        struct Tab
        {
            const char* id;  // in the log
            Text name;
            std::vector<Item> items;
        };
        const Tab TABS[] = {
            { "game", { "GAME", "JOGO" }, { Item::Language, Item::CoopMode, Item::InvertAxis, Item::Autosave, Item::SkipCutscenes, Item::Storage } },
            { "audio", { "AUDIO", "ÁUDIO" },
              { Item::Master, Item::Music, Item::Effects, Item::Voice, Item::HeaderSubtitles, Item::Subtitles, Item::SubtitleSize,
                Item::SubtitleBackground } },
            { "video", { "VIDEO", "VÍDEO" }, { Item::Window, Item::Resolution, Item::FpsLimit, Item::VSync, Item::AntiAliasing, Item::Overlay } },
            { "graphics", { "GRAPHICS", "GRÁFICOS" },
              { Item::Upscaler, Item::UpscalerQuality, Item::Sharpness, Item::MotionBlur, Item::DepthOfField, Item::Anisotropy,
                Item::ShadowSoftness, Item::ShadowResolution } },
            { "controls", { "CONTROLS", "CONTROLES" }, ControlItems() },
            { "definitive", { "DEFINITIVE EDITION", "DEFINITIVE EDITION" },
              { Item::MojoColors, Item::HdRemaster, Item::HeaderLighting, Item::LightSsao, Item::LightGi, Item::LightGiIntensity,
                Item::LightContactShadows, Item::LightSoftShadows, Item::HeaderCamera, Item::FreeCamera, Item::CameraSensitivity,
                Item::CameraInvertY, Item::CameraDistance, Item::HeaderDev, Item::DevConsole } },
        };
        constexpr int TAB_COUNT = int(std::size(TABS));
        constexpr int GAME_TAB = 0, DEFINITIVE_TAB = 5;

        bool Selectable(Item item)
        {
            return item != Item::Storage && item != Item::ShadowResolution && !IsHeader(item);
        }

        std::atomic<bool> g_available{ false };
        std::atomic<bool> g_requested{ false };
        std::atomic<bool> g_open{ false };
        std::atomic<bool> g_releasing{ false };
        std::atomic<ULONGLONG> g_backUntil{ 0 };  // B for the game until then (closing)
        std::atomic<bool> g_fromMainMenu{ true };  // where RequestOpen came from (the restart warning)

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
        ULONGLONG g_heldSince = 0, g_lastRepeat = 0;
        bool g_dirty = true;
        menu_art::Canvas g_canvas;
        std::vector<uint32_t> g_pixels;
        uint64_t g_version = 0;
        ULONGLONG g_openedAt = 0, g_closedAt = 0;
        bool g_closing = false;  // fading out
        // Leaving with options that need a restart: "restart now?" (A restart, B later), then
        // "restarting..." for a moment before Restart.
        bool g_confirm = false;
        ULONGLONG g_restartAt = 0;
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

        std::string OnOff(bool value)
        {
            return value ? T({ "ON", "LIGADO" }) : T({ "OFF", "DESLIGADO" });
        }

        // The setting behind each item, as in settings.toml (also the name in the log).
        const char* Key(Item item)
        {
            if (IsBinding(item))
            {
                static std::string keys[ACTION_COUNT];
                std::string& key = keys[size_t(ActionOf(item))];
                key = std::string("controls.") + ActionId(ActionOf(item));
                return key.c_str();
            }
            switch (item)
            {
            case Item::Master: return "audio.master";
            case Item::Music: return "audio.music";
            case Item::Effects: return "audio.effects";
            case Item::Voice: return "audio.voice";
            case Item::Window: return "video.window";
            case Item::Resolution: return "video.resolution";
            case Item::FpsLimit: return "video.fps_limit";
            case Item::VSync: return "video.vsync";
            case Item::AntiAliasing: return "video.antialiasing";
            case Item::Overlay: return "video.overlay";
            case Item::Upscaler: return "graphics.upscaler";
            case Item::UpscalerQuality: return "graphics.upscaler_quality";
            case Item::Sharpness: return "graphics.sharpness";
            case Item::MotionBlur: return "graphics.motion_blur";
            case Item::DepthOfField: return "graphics.depth_of_field";
            case Item::Anisotropy: return "graphics.anisotropic_filtering";
            case Item::ShadowSoftness: return "graphics.shadow_softness";
            case Item::ShadowResolution: return "graphics.shadow_resolution";
            case Item::HdRemaster: return "graphics.hd_remaster";
            case Item::Language: return "game.language";
            case Item::MojoColors: return "game.mojo_colors";
            case Item::CoopMode: return "game.coop_mode";
            case Item::InvertAxis: return "game.invert_axis";
            case Item::Autosave: return "game.autosave";
            case Item::SkipCutscenes: return "game.skip_cutscenes";
            case Item::Storage: return "game.storage";
            case Item::Subtitles: return "game.subtitles";
            case Item::SubtitleSize: return "game.subtitle_size";
            case Item::SubtitleBackground: return "game.subtitle_background";
            case Item::FreeCamera: return "camera.free_camera";
            case Item::CameraSensitivity: return "camera.sensitivity";
            case Item::CameraInvertY: return "camera.invert_y";
            case Item::CameraDistance: return "camera.distance";
            case Item::DevConsole: return "dev.console";
            case Item::LightSsao: return "lighting.ssao";
            case Item::LightGi: return "lighting.gi";
            case Item::LightGiIntensity: return "lighting.gi_intensity";
            case Item::LightContactShadows: return "lighting.contact_shadows";
            case Item::LightSoftShadows: return "lighting.soft_shadows";
            case Item::Prompts: return "controls.prompts";
            case Item::MouseCamera: return "controls.mouse_camera";
            case Item::MouseSensitivity: return "controls.mouse_sensitivity";
            case Item::MouseInvertY: return "controls.invert_y";
            case Item::WalkSpeed: return "controls.walk_speed";
            case Item::MouseMenus: return "controls.mouse_menus";
            default: break;
            }
            return "";
        }

        std::string Label(Item item)
        {
            if (IsBinding(item))
                return ActionLabel(ActionOf(item), Portuguese());
            switch (item)
            {
            case Item::Master: return T({ "MASTER VOLUME", "VOLUME GERAL" });
            case Item::Music: return T({ "MUSIC", "MÚSICA" });
            case Item::Effects: return T({ "EFFECTS", "EFEITOS" });
            case Item::Voice: return T({ "VOICES", "VOZES" });
            case Item::Window: return T({ "DISPLAY MODE", "MODO DE TELA" });
            case Item::Resolution: return T({ "RESOLUTION", "RESOLUÇÃO" });
            case Item::FpsLimit: return T({ "FPS LIMIT", "LIMITE DE FPS" });
            case Item::VSync: return "V-SYNC";
            case Item::AntiAliasing: return "ANTI-ALIASING";
            case Item::Overlay: return "OVERLAY (F1)";
            case Item::Upscaler: return "DLSS / FSR";
            case Item::UpscalerQuality: return T({ "UPSCALER MODE", "MODO DO UPSCALER" });
            case Item::Sharpness: return T({ "SHARPNESS (FSR)", "NITIDEZ (FSR)" });
            case Item::MotionBlur: return "MOTION BLUR";
            case Item::DepthOfField: return T({ "DEPTH OF FIELD", "PROFUNDIDADE DE CAMPO" });
            case Item::Anisotropy: return T({ "ANISOTROPIC FILTERING", "FILTRO ANISOTRÓPICO" });
            case Item::ShadowSoftness: return T({ "SHADOW SOFTNESS", "SUAVIDADE DAS SOMBRAS" });
            case Item::ShadowResolution: return T({ "SHADOW RESOLUTION", "RESOLUÇÃO DAS SOMBRAS" });
            case Item::HdRemaster: return "HD AI REMASTER";
            case Item::Language: return T({ "LANGUAGE", "IDIOMA" });
            case Item::MojoColors: return T({ "MOJO COLORS", "CORES DO MOJO" });
            case Item::CoopMode: return T({ "CO-OP", "COOPERATIVO" });
            case Item::InvertAxis: return T({ "INVERT AXIS", "INVERTER EIXO" });
            case Item::Autosave: return T({ "AUTOSAVE", "SALVAMENTO AUTOMÁTICO" });
            case Item::SkipCutscenes: return T({ "SKIP CUTSCENES", "PULAR CENAS" });
            case Item::Storage: return T({ "STORAGE", "ARMAZENAMENTO" });
            case Item::Subtitles: return T({ "SUBTITLES", "LEGENDAS" });
            case Item::SubtitleSize: return T({ "SUBTITLE SIZE", "TAMANHO DA LEGENDA" });
            case Item::SubtitleBackground: return T({ "SUBTITLE BACKGROUND", "FUNDO DA LEGENDA" });
            case Item::FreeCamera: return T({ "FREE CAMERA (EXPERIMENTAL)", "CÂMERA LIVRE (EXPERIMENTAL)" });
            case Item::CameraSensitivity: return T({ "CAMERA SENSITIVITY", "SENSIBILIDADE DA CÂMERA" });
            case Item::CameraInvertY: return T({ "INVERT CAMERA Y", "INVERTER EIXO Y" });
            case Item::CameraDistance: return T({ "CAMERA DISTANCE", "DISTÂNCIA DA CÂMERA" });
            case Item::LightSsao: return T({ "AMBIENT OCCLUSION (SSAO)", "OCLUSÃO DE AMBIENTE (SSAO)" });
            case Item::LightGi: return T({ "INDIRECT LIGHT (SSGI)", "LUZ INDIRETA (SSGI)" });
            case Item::LightGiIntensity: return T({ "INDIRECT LIGHT INTENSITY", "INTENSIDADE DA LUZ INDIRETA" });
            case Item::LightContactShadows: return T({ "CONTACT SHADOWS", "SOMBRAS DE CONTATO" });
            case Item::LightSoftShadows: return T({ "SOFT SHADOWS", "SOMBRAS SUAVES" });
            case Item::Prompts: return T({ "BUTTON ICONS", "ÍCONES DOS BOTÕES" });
            case Item::MouseCamera: return T({ "MOUSE CAMERA", "CÂMERA PELO MOUSE" });
            case Item::MouseSensitivity: return T({ "MOUSE SENSITIVITY", "SENSIBILIDADE DO MOUSE" });
            case Item::MouseInvertY: return T({ "INVERT MOUSE Y", "INVERTER Y DO MOUSE" });
            case Item::WalkSpeed: return T({ "WALK SPEED", "VELOCIDADE AO ANDAR" });
            case Item::MouseMenus: return T({ "MOUSE IN MENUS", "MOUSE NOS MENUS" });
            case Item::HeaderSubtitles: return T({ "SUBTITLES", "LEGENDAS" });
            case Item::HeaderCamera: return T({ "THIRD-PERSON CAMERA", "CÂMERA EM TERCEIRA PESSOA" });
            case Item::HeaderLighting: return T({ "LIGHTING (REMASTER)", "ILUMINAÇÃO (REMASTER)" });
            case Item::HeaderDev: return T({ "DEVELOPER", "DESENVOLVEDOR" });
            case Item::DevConsole: return T({ "DEVELOPER CONSOLE", "CONSOLE DE DESENVOLVEDOR" });
            default: break;
            }
            return "";
        }

        // The game reads it at boot: it applies after a restart, which leaving the screen offers
        // (marked with the game's "rotate" icon). Resolution and upscaler apply when the screen
        // closes (the renderer rebuilds its targets).
        bool RestartOnly(Item item)
        {
            switch (item)
            {
            case Item::Language: case Item::MojoColors: case Item::HdRemaster:
                return true;
            default:
                return false;
            }
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
            const char* restart = CanRestart() ? T({ "The game restarts to apply it (it asks first).", "O jogo reinicia para aplicar (pergunta antes)." })
                                               : T({ "Applies when the game restarts.", "Vale ao reiniciar o jogo." });
            switch (item)
            {
            case Item::Master: return { T({ "Volume of everything the game plays.", "Volume de tudo o que o jogo toca." }), T({ "Applies right away.", "Vale na hora." }) };
            case Item::Music: return { T({ "Volume of the music.", "Volume da música." }), T({ "Applies right away.", "Vale na hora." }) };
            case Item::Effects: return { T({ "Volume of the sound effects and ambience.", "Volume dos efeitos e do som ambiente." }), T({ "Applies right away.", "Vale na hora." }) };
            case Item::Voice: return { T({ "Volume of the dialogue.", "Volume das vozes." }), T({ "Applies right away.", "Vale na hora." }) };
            case Item::Window:
                return { T({ "Window, borderless (a window the size of the monitor)", "Janela, sem bordas (uma janela do tamanho do monitor)" }),
                         T({ "or exclusive fullscreen. Alt+Enter also switches.", "ou tela cheia exclusiva. Alt+Enter também alterna." }) };
            case Item::Resolution:
                return { T({ "Internal resolution. With DLSS/FSR on, it is", "Resolução interna. Com DLSS/FSR ligado, é a" }),
                         T({ "the upscaler's output resolution.", "resolução de saída do upscaler." }) };
            case Item::FpsLimit:
                return { T({ "30 is the original. The game speed does not change.", "30 é o original. A velocidade do jogo não muda." }),
                         T({ "With V-Sync on, also limited by the monitor.", "Com V-Sync ligado, também limitado pelo monitor." }) };
            case Item::VSync:
                return { T({ "Waits for the monitor's refresh: no tearing.", "Espera a atualização do monitor: sem tearing." }),
                         T({ "Off by default.", "Desligado por padrão." }) };
            case Item::AntiAliasing:
                return { T({ "FXAA/SMAA smooth the edges of the final image.", "FXAA/SMAA suavizam as bordas na imagem final." }),
                         T({ "With DLSS/FSR on it can stay off.", "Com DLSS/FSR ligado pode ficar desligado." }) };
            case Item::Overlay:
                return { T({ "Performance overlay: FPS, frame time and graph.", "Overlay de desempenho: FPS, frame time e gráfico." }),
                         T({ "F1 switches it at any time.", "O F1 alterna a qualquer momento." }) };
            case Item::Upscaler:
                return { T({ "Temporal AA + upscaling of the 3D scene, no", "AA temporal + upscaling da cena 3D, sem" }),
                         T({ "frame generation. DLSS: NVIDIA RTX only.", "frame generation. DLSS: só NVIDIA RTX." }) };
            case Item::UpscalerQuality:
                return { T({ "Native = DLAA / FSR Native AA. The others", "Nativo = DLAA / FSR Native AA. Os outros" }),
                         T({ "draw the scene smaller and upscale it.", "desenham a cena menor e reescalam." }) };
            case Item::Sharpness:
                return { T({ "FSR 3.1 sharpening (RCAS) after upscaling.", "Nitidez do FSR 3.1 (RCAS) após o upscaling." }),
                         T({ "DLSS 4 does not use it.", "O DLSS 4 não usa este ajuste." }) };
            case Item::MotionBlur:
                return { T({ "Strength of the game's motion blur.", "Intensidade do motion blur do jogo." }),
                         T({ "0% turns it off, 100% = original.", "0% desliga, 100% = original." }) };
            case Item::DepthOfField:
                return { T({ "Distance blur of the game's scenes.", "Desfoque de distância das cenas do jogo." }),
                         T({ "0% turns it off, 100% = original.", "0% desliga, 100% = original." }) };
            case Item::Anisotropy:
                return { T({ "Sharpness of textures seen at an angle", "Nitidez das texturas vistas de lado" }),
                         T({ "(ground, walls, paths in the distance).", "(chão, paredes, caminhos ao longe)." }) };
            case Item::ShadowSoftness:
                return { T({ "Radius of the filter on the shadows' edges.", "Raio do filtro da borda das sombras." }),
                         T({ "0% = hard edge, 100% = original.", "0% = borda dura, 100% = original." }) };
            case Item::ShadowResolution:
                return { T({ "The game uses stencil shadows, without shadow", "O jogo usa sombras por stencil, sem mapa de" }),
                         T({ "maps. Their sharpness follows the resolution.", "sombra. A nitidez segue a resolução." }) };
            case Item::HdRemaster:
                if (!HdRemasterAvailable())
                    return { T({ "The remaster files are not on this PC.", "Os arquivos do remaster não estão neste PC." }),
                             T({ "See the README, HD AI Remaster.", "Veja o README, HD AI Remaster." }) };
                if (HdRemasterWholeGame())
                    return { T({ "AI-remastered textures and models (whole game).", "Texturas e modelos remasterizados por IA (jogo todo)." }),
                             restart };
                return { T({ "AI-remastered textures and models (level 1).", "Texturas e modelos remasterizados por IA (fase 1)." }),
                         restart };
            case Item::Language:
            {
                // Language packs (languages\): their own lines, e.g. that the voices stay in English.
                const Language& language = LanguageById(g_values.language);
                if (language.help[0])
                    return { language.help[0], language.help[1] ? language.help[1] : "" };
                return { T({ "Language of the texts and the voices.", "Idioma dos textos e das vozes." }), restart };
            }
            case Item::MojoColors:
                return { T({ "PS2: large one light blue, extra large purple.", "PS2: grande azul bem claro, extra grande roxo." }),
                         T({ "Xbox 360: all blue (the original).", "Xbox 360: todos azuis (o original)." }) };
            case Item::CoopMode:
                return { T({ "LEAPFROG: take turns jumping through the challenges.", "PULA-SELA: vocês se revezam pulando pelos desafios." }),
                         T({ "PIGGYBACK: ride in your partner's backpack.", "CAVALINHO: fique na mochila do seu parceiro." }) };
            case Item::InvertAxis:
                return { T({ "Inverts the camera's axis (the game's own option).", "Inverte o eixo da câmera (a opção do próprio jogo)." }),
                         T({ "Applies right away.", "Vale na hora." }) };
            case Item::Autosave:
                return { T({ "Saves the game in the slot chosen for it.", "Salva o jogo no slot escolhido para ele." }),
                         T({ "Off: the game does not save by itself.", "Desligado: o jogo não salva sozinho." }) };
            case Item::SkipCutscenes:
                return { T({ "Hold B (ESC, SPACE, ENTER) during a cutscene or video", "Segure B (ESC, ESPAÇO, ENTER) numa cena ou vídeo" }),
                         T({ "to skip it; a prompt shows in the corner.", "para pulá-la; um aviso aparece no canto." }) };
            case Item::Storage:
                return { T({ "Saves are kept on the PC, in the userdata folder.", "Os saves ficam no PC, na pasta userdata." }),
                         T({ "The Y button of the main menu picks it.", "O botão Y do menu principal o escolhe." }) };
            case Item::Subtitles:
                if (!subtitles::SoundCount())
                    return { T({ "No subtitles for this language: see", "Sem legendas para este idioma: veja" }),
                             T({ "tools/subtitles.py (README, Legendas).", "tools/subtitles.py (README, Legendas)." }) };
                return { T({ "Subtitles in cutscenes and dialogue (transcribed", "Legendas nas cenas e falas (transcritas" }),
                         T({ "from the voices), in the language of the texts.", "das vozes), no idioma dos textos." }) };
            case Item::SubtitleSize:
                return { T({ "Size of the subtitle letters.", "Tamanho das letras da legenda." }), T({ "100% = default.", "100% = padrão." }) };
            case Item::SubtitleBackground:
                return { T({ "Opacity of the box behind the text.", "Opacidade da caixa atrás do texto." }),
                         T({ "0% = outline only.", "0% = só o contorno." }) };
            case Item::FreeCamera:
                return { T({ "Third-person orbit camera: right stick / mouse,", "Câmera orbital em terceira pessoa: analógico" }),
                         T({ "R3 recenters. Cutscenes keep the game's camera.", "direito / mouse, R3 recentra. Cenas: a do jogo." }) };
            case Item::CameraSensitivity:
                return { T({ "Turn speed of the free camera.", "Velocidade de giro da câmera livre." }), T({ "100% = default.", "100% = padrão." }) };
            case Item::CameraInvertY:
                return { T({ "Up on the stick / mouse looks down.", "Para cima no analógico / mouse olha para baixo." }),
                         T({ "Free camera only.", "Só na câmera livre." }) };
            case Item::CameraDistance:
                return { T({ "Distance from the camera to the character.", "Distância da câmera ao personagem." }),
                         T({ "Walls bring it closer by themselves.", "Paredes a aproximam sozinhas." }) };
            case Item::DevConsole:
                return { T({ "The key left of 1 (or F10) opens a console:", "A tecla antes do 1 (ou F10) abre um console:" }),
                         T({ "unlock all, mojo, god, skin, level, lua... (help).", "unlock all, mojo, god, skin, level, lua... (help)." }) };
            case Item::LightSsao:
                return { T({ "Darkens corners, contacts and creases of the scene.", "Escurece cantos, contatos e dobras da cena." }),
                         T({ "LOW: half resolution, HIGH: full. F8 toggles it all.", "BAIXA: meia resolução, ALTA: cheia. F8 liga/desliga." }) };
            case Item::LightGi:
                return { T({ "Light bounced by nearby surfaces tints the scene", "A luz rebatida nas superfícies próximas tinge a" }),
                         T({ "with their colour (screen space, one bounce).", "cena com a cor delas (espaço de tela, um rebote)." }) };
            case Item::LightGiIntensity:
                return { T({ "Strength of the indirect light.", "Intensidade da luz indireta." }), T({ "100% = default.", "100% = padrão." }) };
            case Item::LightContactShadows:
                return { T({ "Small shadows under characters, objects and", "Sombras pequenas sob personagens, objetos e" }),
                         T({ "ledges, where they meet the ground.", "beiradas, onde encostam no chão." }) };
            case Item::LightSoftShadows:
                return { T({ "The game's shadows with a wider, smoother filter.", "As sombras do jogo com um filtro mais largo e suave." }),
                         T({ "SHADOW SOFTNESS (GRAPHICS) still sets the radius.", "A SUAVIDADE DAS SOMBRAS (GRÁFICOS) ajusta o raio." }) };
            case Item::Prompts:
                return { T({ "Icons in menus and hints: the last device used,", "Ícones nos menus e dicas: o último aparelho usado," }),
                         T({ "or always Xbox / PlayStation / keyboard.", "ou sempre Xbox / PlayStation / teclado." }) };
            case Item::MouseCamera:
                return { T({ "The mouse turns the camera in gameplay", "O mouse gira a câmera na gameplay" }),
                         T({ "(the cursor is hidden and kept in the window).", "(o cursor fica escondido e preso na janela)." }) };
            case Item::MouseSensitivity:
                return { T({ "Mouse movement -> camera speed.", "Movimento do mouse -> velocidade da câmera." }), T({ "100% = default.", "100% = padrão." }) };
            case Item::MouseInvertY:
                return { T({ "Mouse / camera keys: up looks down.", "Mouse / teclas da câmera: para cima olha para baixo." }), "" };
            case Item::WalkSpeed:
                return { T({ "How far the stick goes while WALK is held", "Quanto o analógico vai com ANDAR DEVAGAR apertado" }),
                         T({ "(Crash walks instead of running).", "(o Crash anda em vez de correr)." }) };
            case Item::MouseMenus:
                return { T({ "Pointing at an item selects it, click accepts,", "Apontar um item seleciona, clique confirma," }),
                         T({ "right button goes back, the wheel scrolls.", "botão direito volta, a roda rola." }) };
            default: break;
            }
            return {};
        }

        // Sliders: value, minimum, maximum and step.
        int* Percent(Item item, int* minimum = nullptr, int* maximum = nullptr, int* step = nullptr)
        {
            int min = 0, max = 100, stepValue = 5;
            int* value = nullptr;
            switch (item)
            {
            case Item::Master: value = &g_values.masterVolume; break;
            case Item::Music: value = &g_values.musicVolume; break;
            case Item::Effects: value = &g_values.effectsVolume; break;
            case Item::Voice: value = &g_values.voiceVolume; break;
            case Item::Sharpness: value = &g_values.sharpness; stepValue = 10; break;
            case Item::MotionBlur: value = &g_values.motionBlur; stepValue = 10; break;
            case Item::DepthOfField: value = &g_values.depthOfField; stepValue = 10; break;
            case Item::ShadowSoftness: value = &g_values.shadowSoftness; stepValue = 25; max = 200; break;
            case Item::SubtitleSize: value = &g_values.subtitleSize; stepValue = 25; min = 50; max = 200; break;
            case Item::SubtitleBackground: value = &g_values.subtitleBackground; stepValue = 25; break;
            case Item::CameraSensitivity: value = &g_values.cameraSensitivity; stepValue = 25; min = 25; max = 300; break;
            case Item::CameraDistance: value = &g_values.cameraDistance; stepValue = 10; min = 50; max = 200; break;
            case Item::LightGiIntensity: value = &g_values.giIntensity; stepValue = 10; max = 200; break;
            case Item::MouseSensitivity: value = &g_values.controls.mouseSensitivity; stepValue = 10; min = 10; max = 400; break;
            case Item::WalkSpeed: value = &g_values.controls.walkSpeed; stepValue = 5; min = 10; max = 90; break;
            default: break;
            }
            if (minimum)
                *minimum = min;
            if (maximum)
                *maximum = max;
            if (step)
                *step = stepValue;
            return value;
        }

        std::string UpscalerLabel(Upscaler upscaler)
        {
            const char* missing = T({ " (N/A)", " (N/D)" });
            switch (upscaler)
            {
            case Upscaler::Dlss: return std::string("DLSS") + (gpu::upscaler::Available(Upscaler::Dlss) ? "" : missing);
            case Upscaler::Fsr: return std::string("FSR 3.1") + (gpu::upscaler::Available(Upscaler::Fsr) ? "" : missing);
            default: return OnOff(false);
            }
        }

        std::string QualityLabel()
        {
            switch (g_values.upscalerQuality)
            {
            case UpscalerQuality::Quality: return T({ "QUALITY", "QUALIDADE" });
            case UpscalerQuality::Balanced: return T({ "BALANCED", "EQUILIBRADO" });
            case UpscalerQuality::Performance: return T({ "PERFORMANCE", "DESEMPENHO" });
            case UpscalerQuality::UltraPerformance: return T({ "ULTRA PERFORMANCE", "ULTRA DESEMPENHO" });
            default:
                return g_values.upscaler == Upscaler::Fsr ? T({ "NATIVE (AA)", "NATIVO (AA)" }) : T({ "NATIVE (DLAA)", "NATIVO (DLAA)" });
            }
        }

        std::string Value(Item item)
        {
            if (IsBinding(item))
            {
                if (g_capturing && item == TABS[g_tab].items[size_t(g_row[g_tab])])
                    return T({ "PRESS A KEY...", "PRESSIONE UMA TECLA..." });
                std::string keys;
                for (uint16_t key : g_values.controls.keys[size_t(ActionOf(item))])
                {
                    if (key)
                        keys += (keys.empty() ? "" : " / ") + KeyName(key);
                }
                return keys.empty() ? T({ "NONE", "NENHUMA" }) : keys;
            }
            if (int* percent = Percent(item))
                return std::to_string(*percent) + "%";
            switch (item)
            {
            case Item::Window:
                switch (g_values.window)
                {
                case WindowMode::Borderless: return T({ "BORDERLESS", "SEM BORDAS" });
                case WindowMode::Fullscreen: return T({ "FULLSCREEN", "TELA CHEIA" });
                default: return T({ "WINDOWED", "JANELA" });
                }
            case Item::VSync: return OnOff(g_values.vsync);
            case Item::AntiAliasing:
                switch (g_values.antiAliasing)
                {
                case AntiAliasing::Fxaa: return "FXAA";
                case AntiAliasing::Smaa: return "SMAA";
                default: return OnOff(false);
                }
            case Item::Overlay:
                switch (g_values.overlay)
                {
                case OverlayMode::Fps: return T({ "FPS ONLY", "SÓ FPS" });
                case OverlayMode::FrameTime: return "FPS + FRAME TIME";
                case OverlayMode::Full: return T({ "FULL", "COMPLETO" });
                default: return OnOff(false);
                }
            case Item::Resolution: return ResolutionOf(g_values.renderHeight).name;
            case Item::FpsLimit:
                return g_values.fpsLimit ? std::to_string(g_values.fpsLimit) + (g_values.fpsLimit == 30 ? " (ORIGINAL)" : "")
                                         : std::string(T({ "UNLIMITED", "SEM LIMITE" }));
            case Item::Upscaler: return UpscalerLabel(g_values.upscaler);
            case Item::UpscalerQuality: return QualityLabel();
            case Item::Anisotropy: return g_values.anisotropy > 1 ? std::to_string(g_values.anisotropy) + "X" : OnOff(false);
            case Item::ShadowResolution: return T({ "SAME AS RESOLUTION", "SEGUE A RESOLUÇÃO" });
            case Item::HdRemaster: return HdRemasterAvailable() ? OnOff(g_values.hdRemaster) : T({ "UNAVAILABLE", "INDISPONÍVEL" });
            case Item::Language: return LanguageById(g_values.language).name;
            case Item::MojoColors: return g_values.ps2MojoColors ? "PS2" : "XBOX 360";
            case Item::CoopMode: return g_values.coopLeapfrog ? T({ "LEAPFROG", "PULA-SELA" }) : T({ "PIGGYBACK", "CAVALINHO" });
            case Item::InvertAxis: return OnOff(g_values.invertAxis);
            case Item::Autosave: return OnOff(g_values.autosave);
            case Item::SkipCutscenes: return OnOff(g_values.skipCutscenes);
            case Item::Subtitles:
                return g_values.subtitles && !subtitles::SoundCount() ? T({ "ON (NO TEXT)", "LIGADO (SEM TEXTO)" }) : OnOff(g_values.subtitles);
            case Item::Storage: return "HARD DRIVE";
            case Item::FreeCamera: return OnOff(g_values.freeCamera);
            case Item::CameraInvertY: return OnOff(g_values.cameraInvertY);
            case Item::DevConsole: return OnOff(g_values.devConsole);
            case Item::LightSsao:
                switch (g_values.ssao)
                {
                case AoQuality::Low: return T({ "LOW", "BAIXA" });
                case AoQuality::High: return T({ "HIGH", "ALTA" });
                default: return OnOff(false);
                }
            case Item::LightGi: return OnOff(g_values.gi);
            case Item::LightContactShadows: return OnOff(g_values.contactShadows);
            case Item::LightSoftShadows: return OnOff(g_values.softShadows);
            case Item::Prompts:
                switch (g_values.controls.prompts)
                {
                case PromptStyle::Xbox: return "XBOX";
                case PromptStyle::PlayStation: return "PLAYSTATION";
                case PromptStyle::Keyboard: return T({ "KEYBOARD", "TECLADO" });
                default: return T({ "AUTOMATIC", "AUTOMÁTICO" });
                }
            case Item::MouseCamera: return OnOff(g_values.controls.mouseCamera);
            case Item::MouseInvertY: return OnOff(g_values.controls.invertY);
            case Item::MouseMenus: return OnOff(g_values.controls.mouseMenus);
            default: return "";
            }
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
            menu_art::DrawText(g_canvas, font, u, v.X(left), v.Y(y), cell * v.s, style);
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
            const menu_art::Image* corner = menu_art::GetArt(menu_art::Art::MessageFrame);
            int left = v.IX(x0), top = v.IY(y0), right = v.IX(x1), bottom = v.IY(y1);
            if (!corner)
            {
                g_canvas.Fill(left, top, right, bottom, { 0, 0, 0, 150 });
                return;
            }
            int halfW = (right - left) / 2, halfH = (bottom - top) / 2;
            float k = float(halfH) / float(corner->h);
            int curl = std::min(corner->w / 3, corner->w);  // the curl is in the left third
            int curlW = std::min(int(std::lround(curl * k)), halfW);
            for (int q = 0; q < 4; q++)
            {
                bool flipX = q & 1, flipY = q & 2;
                int qx = flipX ? right - halfW : left, qy = flipY ? bottom - halfH : top;
                const menu_art::Image& c = menu_art::Scaled(*corner, 0, 0, curl, corner->h, curlW, halfH, flipX, flipY);
                const menu_art::Image& edge = menu_art::Scaled(*corner, curl, 0, corner->w - curl, corner->h, halfW - curlW, halfH, flipX, flipY);
                g_canvas.Blit(c, flipX ? qx + halfW - curlW : qx, qy, g_palette->frame);
                g_canvas.Blit(edge, flipX ? qx : qx + curlW, qy, g_palette->frame);
            }
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
            constexpr float BOX_L = 230.0f, BOX_R = 1050.0f, BOX_T = 184.0f, LINE = 42.0f, CELL = 30.0f;
            bool warn = !g_fromMainMenu;
            int lines = 4 + (warn ? 1 : 0);
            float boxB = BOX_T + 34.0f + LINE * lines + 26.0f;
            DrawFrame(v, BOX_L, BOX_T, BOX_R, boxB);
            float maxW = BOX_R - BOX_L - 90.0f, y = BOX_T + 30.0f, cx = VIRTUAL_W * 0.5f;
            auto line = [&](const std::string& text, const TextStyle& style) {
                Draw(v, FontId::Small, text, cx, y, CELL, style, 0.5f, maxW);
                y += LINE;
            };
            line(T({ "Some options only apply after a restart:", "Algumas opções só valem depois de reiniciar:" }), pal.help);
            line(RestartItems(), pal.selected);
            if (warn)
                line(T({ "Progress that was not saved will be lost.", "O progresso não salvo será perdido." }), pal.warning);
            if (g_restartAt)
            {
                line(T({ "Restarting...", "Reiniciando..." }), pal.help);
                return;
            }
            line(T({ "Restart the game now?", "Reiniciar o jogo agora?" }), pal.help);
            // A / B with the game's button icons (\xC2\xB1 = A, \xC2\xB2 = B).
            std::string yes = std::string("\xC2\xB1 ") + T({ "RESTART", "REINICIAR" });
            std::string no = std::string("\xC2\xB2 ") + T({ "LATER", "DEPOIS" });
            float gap = 70.0f, yesW = Width(FontId::Small, yes, CELL), noW = Width(FontId::Small, no, CELL);
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
            for (const Tab& tab : TABS)
                total += Width(FontId::Large, T(tab.name), TAB_CELL);
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
                float end = Draw(v, FontId::Small, Value(item), valueX, y, ROW_CELL, style, 0.0f, PANEL_R - valueX - arrowW - 10.0f);
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
            float helpMax = PANEL_R - PANEL_L - 70.0f;
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

        template <typename E>
        E Cycle(E value, int count, int direction)
        {
            return E((int(value) + count + direction) % count);
        }

        void Change(int direction)
        {
            Item item = SelectedItem();
            Values before = g_values;
            int minimum, maximum, step;
            if (int* percent = Percent(item, &minimum, &maximum, &step))
            {
                *percent = std::clamp(*percent + direction * step, minimum, maximum);
            }
            else
            {
                switch (item)
                {
                case Item::Window:
                    g_values.window = Cycle(g_values.window, 3, direction);
                    break;
                case Item::VSync:
                    g_values.vsync = !g_values.vsync;
                    break;
                case Item::FpsLimit:
                {
                    constexpr int LIMITS[] = { 30, 60, 120, 0 };
                    int index = int(std::find(std::begin(LIMITS), std::end(LIMITS), g_values.fpsLimit) - std::begin(LIMITS)) % 4;
                    g_values.fpsLimit = LIMITS[(index + 4 + direction) % 4];
                    break;
                }
                case Item::Resolution:
                {
                    size_t count;
                    const settings::Resolution* list = Resolutions(count);
                    size_t index = size_t(&ResolutionOf(g_values.renderHeight) - list);
                    g_values.renderHeight = list[(index + count + direction) % count].height;
                    break;
                }
                case Item::AntiAliasing:
                    g_values.antiAliasing = Cycle(g_values.antiAliasing, 3, direction);
                    break;
                case Item::Overlay:
                    g_values.overlay = Cycle(g_values.overlay, 4, direction);
                    break;
                case Item::Upscaler:
                    g_values.upscaler = Cycle(g_values.upscaler, 3, direction);
                    break;
                case Item::UpscalerQuality:
                    g_values.upscalerQuality = Cycle(g_values.upscalerQuality, 5, direction);
                    break;
                case Item::Anisotropy:
                {
                    constexpr int LEVELS[] = { 1, 2, 4, 8, 16 };
                    int index = int(std::find(std::begin(LEVELS), std::end(LEVELS), g_values.anisotropy) - std::begin(LEVELS)) % 5;
                    g_values.anisotropy = LEVELS[(index + 5 + direction) % 5];
                    break;
                }
                case Item::Language:
                {
                    size_t count;
                    const Language* languages = Languages(count);
                    size_t index = size_t(&LanguageById(g_values.language) - languages);
                    g_values.language = languages[(index + count + direction) % count].id;
                    break;
                }
                case Item::MojoColors:
                    g_values.ps2MojoColors = !g_values.ps2MojoColors;
                    break;
                case Item::CoopMode:
                    g_values.coopLeapfrog = !g_values.coopLeapfrog;
                    break;
                case Item::InvertAxis:
                    g_values.invertAxis = !g_values.invertAxis;
                    break;
                case Item::Autosave:
                    g_values.autosave = !g_values.autosave;
                    break;
                case Item::SkipCutscenes:
                    g_values.skipCutscenes = !g_values.skipCutscenes;
                    break;
                case Item::HdRemaster:
                    if (!HdRemasterAvailable())
                        return;  // shown as unavailable; the saved value is kept for when the data is back
                    g_values.hdRemaster = !g_values.hdRemaster;
                    break;
                case Item::Subtitles:
                    g_values.subtitles = !g_values.subtitles;
                    break;
                case Item::FreeCamera:
                    g_values.freeCamera = !g_values.freeCamera;
                    break;
                case Item::CameraInvertY:
                    g_values.cameraInvertY = !g_values.cameraInvertY;
                    break;
                case Item::DevConsole:
                    g_values.devConsole = !g_values.devConsole;
                    break;
                case Item::LightSsao:
                    g_values.ssao = Cycle(g_values.ssao, 3, direction);
                    break;
                case Item::LightGi:
                    g_values.gi = !g_values.gi;
                    break;
                case Item::LightContactShadows:
                    g_values.contactShadows = !g_values.contactShadows;
                    break;
                case Item::LightSoftShadows:
                    g_values.softShadows = !g_values.softShadows;
                    break;
                case Item::Prompts:
                    g_values.controls.prompts = Cycle(g_values.controls.prompts, 4, direction);
                    break;
                case Item::MouseCamera:
                    g_values.controls.mouseCamera = !g_values.controls.mouseCamera;
                    break;
                case Item::MouseInvertY:
                    g_values.controls.invertY = !g_values.controls.invertY;
                    break;
                case Item::MouseMenus:
                    g_values.controls.mouseMenus = !g_values.controls.mouseMenus;
                    break;
                default:
                    return;
                }
            }
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
            g_closedAt = GetTickCount64();
            // The game leaves the options screen with its own transition (ExitBack): the PS2
            // options screen of the main menu's SETTINGS, or the in-game options.
            if (leaveScreen)
                g_backUntil = g_closedAt + BACK_PRESS_MS;
            Log("[settings] screen closed%s", leaveScreen ? "" : " (the game left its options screen)");
        }

        // Buttons that went down this frame, plus auto-repeat of a held direction.
        uint16_t Pressed(uint16_t buttons)
        {
            ULONGLONG now = GetTickCount64();
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
                g_openedAt = GetTickCount64();
                // The art is read at startup; give it a moment if the screen opens very early.
                menu_art::Ready(2000);
                Log("[settings] screen opened%s", InGame() ? " (pause menu OPTIONS)" : "");
            }
            if (!g_open)
            {
                if (g_releasing && !buttons && GetTickCount64() >= g_backUntil)
                    g_releasing = false;
                g_previousButtons = buttons;
                if (g_closing && GetTickCount64() - g_closedAt >= FADE_OUT_MS)
                    g_closing = false;
                return g_closing;
            }
            // In a game: the game left its options screen by itself (it should not: its input is
            // blocked while the settings are open).
            if (InGame() && g_frontendNode != g_optionsNode && !g_restartAt)
            {
                Close(false);
                return true;  // fading out
            }
            // Input only once the screen is shown (the game's transition first).
            if (GetTickCount64() - g_openedAt < OPEN_DELAY_MS)
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
                if (GetTickCount64() >= g_restartAt)
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
                    g_restartAt = GetTickCount64() + RESTART_DELAY_MS;
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
        g_requested = true;
    }

    bool IsOpen()
    {
        return g_open;
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
        return (GetTickCount64() < g_backUntil ? XINPUT_GAMEPAD_B : 0) | MenuAutopilotButtons() | boot::Buttons();
    }

    void Draw(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE target, uint32_t width, uint32_t height)
    {
        if (!Update() || !width || !height)
            return;
        Render(int(width), int(height));
        // Fade (and rise) in after the game's transition; fade out when closing.
        ULONGLONG now = GetTickCount64();
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
        gpu::overlay::DrawBitmap(list, target, width, height, pixels, int(width), int(height), 0, rise, version,
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
