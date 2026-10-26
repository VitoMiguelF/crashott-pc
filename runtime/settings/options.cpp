#include "options.h"

#include <algorithm>
#include <iterator>

namespace settings::options
{
    namespace
    {
        const char* T(bool pt, const Text& text)
        {
            return pt ? text.pt : text.en;
        }

        std::string OnOff(bool pt, bool value)
        {
            return value ? T(pt, { "ON", "LIGADO" }) : T(pt, { "OFF", "DESLIGADO" });
        }

        template <typename E>
        E Cycle(E value, int count, int direction)
        {
            return E((int(value) + count + direction) % count);
        }

        std::vector<Item> ControlItems()
        {
            std::vector<Item> items = { Item::Player1, Item::Player2, Item::Prompts, Item::MouseCamera, Item::MouseSensitivity,
                                        Item::MouseInvertY, Item::WalkSpeed, Item::MouseMenus };
            for (int i = 0; i < int(ACTION_COUNT); i++)
                items.push_back(BindingItem(Action(i)));
            return items;
        }
    }

    bool IsHeader(Item item)
    {
        return item == Item::HeaderSubtitles || item == Item::HeaderCamera || item == Item::HeaderLighting || item == Item::HeaderDev ||
               item == Item::HeaderKeys;
    }

    bool IsBinding(Item item)
    {
        return int(item) >= int(Item::Bind0) && int(item) < int(Item::Bind0) + int(ACTION_COUNT);
    }

    Action ActionOf(Item item)
    {
        return Action(int(item) - int(Item::Bind0));
    }

    Item BindingItem(Action action)
    {
        return Item(int(Item::Bind0) + int(action));
    }

    const Tab* Tabs()
    {
        static Tab tabs[TAB_COUNT] = {
            { "game", { "GAME", "JOGO" }, { Item::Language, Item::CoopMode, Item::InvertAxis, Item::Autosave, Item::SkipCutscenes, Item::Storage } },
            { "audio", { "AUDIO", "ÁUDIO" },
              { Item::Master, Item::Music, Item::Effects, Item::Voice, Item::HeaderSubtitles, Item::Subtitles, Item::SubtitleSize,
                Item::SubtitleBackground } },
            { "video", { "VIDEO", "VÍDEO" }, { Item::Window, Item::Resolution, Item::FpsLimit, Item::VSync, Item::AntiAliasing, Item::Overlay, Item::Aspect,
                Item::Hud16x9 } },
            { "graphics", { "GRAPHICS", "GRÁFICOS" },
              { Item::Upscaler, Item::UpscalerQuality, Item::Sharpness, Item::MotionBlur, Item::DepthOfField, Item::Anisotropy,
                Item::ShadowSoftness, Item::ShaderCompilation } },
            { "controls", { "CONTROLS", "CONTROLES" }, ControlItems() },
            { "definitive", { "DEFINITIVE EDITION", "DEFINITIVE EDITION" },
              { Item::MojoColors, Item::HdRemaster, Item::HeaderLighting, Item::LightSsao, Item::LightGi, Item::LightGiIntensity,
                Item::LightContactShadows, Item::LightSoftShadows, Item::HeaderCamera, Item::FreeCamera, Item::CameraSensitivity,
                Item::CameraInvertY, Item::CameraDistance, Item::HeaderDev, Item::DevConsole } },
        };
#if defined(__ANDROID__)
        // Android has no HD AI Remaster (by design), no DLSS / FSR and no window mode: those rows go.
        static const bool trimmed = [] {
            for (Tab& tab : tabs)
                std::erase_if(tab.items, [](Item item) {
                    return item == Item::HdRemaster || item == Item::Upscaler || item == Item::UpscalerQuality || item == Item::Sharpness ||
                           item == Item::Window;
                });
            return true;
        }();
        (void)trimmed;
#endif
        return tabs;
    }

    bool Portuguese(const Values& values)
    {
        return values.language.rfind("pt", 0) == 0;
    }

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
        case Item::Aspect: return "video.aspect";
        case Item::Hud16x9: return "video.hud_16_9";
        case Item::Upscaler: return "graphics.upscaler";
        case Item::UpscalerQuality: return "graphics.upscaler_quality";
        case Item::Sharpness: return "graphics.sharpness";
        case Item::MotionBlur: return "graphics.motion_blur";
        case Item::DepthOfField: return "graphics.depth_of_field";
        case Item::Anisotropy: return "graphics.anisotropic_filtering";
        case Item::ShadowSoftness: return "graphics.shadow_softness";
        case Item::ShaderCompilation: return "graphics.shader_compilation";
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
        case Item::Lighting: return "lighting";
        case Item::Prompts: return "controls.prompts";
        case Item::Player1: return "controls.player1";
        case Item::Player2: return "controls.player2";
        case Item::MouseCamera: return "controls.mouse_camera";
        case Item::MouseSensitivity: return "controls.mouse_sensitivity";
        case Item::MouseInvertY: return "controls.invert_y";
        case Item::WalkSpeed: return "controls.walk_speed";
        case Item::MouseMenus: return "controls.mouse_menus";
        default: break;
        }
        return "";
    }

    std::string Label(Item item, bool pt)
    {
        if (IsBinding(item))
            return ActionLabel(ActionOf(item), pt);
        auto t = [&](const Text& text) { return std::string(T(pt, text)); };
        switch (item)
        {
        case Item::Master: return t({ "MASTER VOLUME", "VOLUME GERAL" });
        case Item::Music: return t({ "MUSIC", "MÚSICA" });
        case Item::Effects: return t({ "EFFECTS", "EFEITOS" });
        case Item::Voice: return t({ "VOICES", "VOZES" });
        case Item::Window: return t({ "DISPLAY MODE", "MODO DE TELA" });
        case Item::Resolution: return t({ "RESOLUTION", "RESOLUÇÃO" });
        case Item::FpsLimit: return t({ "FPS LIMIT", "LIMITE DE FPS" });
        case Item::VSync: return "V-SYNC";
        case Item::AntiAliasing: return "ANTI-ALIASING";
        case Item::Overlay: return "OVERLAY (F1)";
        case Item::Aspect: return t({ "ASPECT RATIO", "PROPORÇÃO" });
        case Item::Hud16x9: return t({ "HUD AT 16:9", "HUD EM 16:9" });
        case Item::Upscaler: return "DLSS / FSR";
        case Item::UpscalerQuality: return t({ "UPSCALER MODE", "MODO DO UPSCALER" });
        case Item::Sharpness: return t({ "SHARPNESS (FSR)", "NITIDEZ (FSR)" });
        case Item::MotionBlur: return "MOTION BLUR";
        case Item::DepthOfField: return t({ "DEPTH OF FIELD", "PROFUNDIDADE DE CAMPO" });
        case Item::Anisotropy: return t({ "ANISOTROPIC FILTERING", "FILTRO ANISOTRÓPICO" });
        case Item::ShadowSoftness: return t({ "SHADOW SOFTNESS", "SUAVIDADE DAS SOMBRAS" });
        case Item::ShaderCompilation: return t({ "SHADER COMPILATION", "COMPILAÇÃO DE SHADERS" });
        case Item::HdRemaster: return "HD AI REMASTER";
        case Item::Language: return t({ "LANGUAGE", "IDIOMA" });
        case Item::MojoColors: return t({ "MOJO COLORS", "CORES DO MOJO" });
        case Item::CoopMode: return t({ "CO-OP", "COOPERATIVO" });
        case Item::InvertAxis: return t({ "INVERT AXIS", "INVERTER EIXO" });
        case Item::Autosave: return t({ "AUTOSAVE", "SALVAMENTO AUTOMÁTICO" });
        case Item::SkipCutscenes: return t({ "SKIP CUTSCENES", "PULAR CENAS" });
        case Item::Storage: return t({ "STORAGE", "ARMAZENAMENTO" });
        case Item::Subtitles: return t({ "SUBTITLES", "LEGENDAS" });
        case Item::SubtitleSize: return t({ "SUBTITLE SIZE", "TAMANHO DA LEGENDA" });
        case Item::SubtitleBackground: return t({ "SUBTITLE BACKGROUND", "FUNDO DA LEGENDA" });
        case Item::FreeCamera: return t({ "FREE CAMERA (EXPERIMENTAL)", "CÂMERA LIVRE (EXPERIMENTAL)" });
        case Item::CameraSensitivity: return t({ "CAMERA SENSITIVITY", "SENSIBILIDADE DA CÂMERA" });
        case Item::CameraInvertY: return t({ "INVERT CAMERA Y", "INVERTER EIXO Y" });
        case Item::CameraDistance: return t({ "CAMERA DISTANCE", "DISTÂNCIA DA CÂMERA" });
        case Item::LightSsao: return t({ "AMBIENT OCCLUSION (SSAO)", "OCLUSÃO DE AMBIENTE (SSAO)" });
        case Item::LightGi: return t({ "INDIRECT LIGHT (SSGI)", "LUZ INDIRETA (SSGI)" });
        case Item::LightGiIntensity: return t({ "INDIRECT LIGHT INTENSITY", "INTENSIDADE DA LUZ INDIRETA" });
        case Item::LightContactShadows: return t({ "CONTACT SHADOWS", "SOMBRAS DE CONTATO" });
        case Item::LightSoftShadows: return t({ "SOFT SHADOWS", "SOMBRAS SUAVES" });
        case Item::Lighting: return t({ "LIGHTING", "ILUMINAÇÃO" });
        case Item::Prompts: return t({ "BUTTON ICONS", "ÍCONES DOS BOTÕES" });
        case Item::Player1: return t({ "PLAYER 1", "JOGADOR 1" });
        case Item::Player2: return t({ "PLAYER 2", "JOGADOR 2" });
        case Item::MouseCamera: return t({ "MOUSE CAMERA", "CÂMERA PELO MOUSE" });
        case Item::MouseSensitivity: return t({ "MOUSE SENSITIVITY", "SENSIBILIDADE DO MOUSE" });
        case Item::MouseInvertY: return t({ "INVERT MOUSE Y", "INVERTER Y DO MOUSE" });
        case Item::WalkSpeed: return t({ "WALK SPEED", "VELOCIDADE AO ANDAR" });
        case Item::MouseMenus: return t({ "MOUSE IN MENUS", "MOUSE NOS MENUS" });
        case Item::GameFolder: return t({ "GAME FOLDER", "PASTA DO JOGO" });
        case Item::ModsFolder: return "MODS";
        case Item::SavesFolder: return t({ "SAVES AND OPTIONS", "SAVES E OPÇÕES" });
        case Item::ExtraData: return t({ "EXTRA DATA", "DADOS EXTRAS" });
        case Item::HeaderSubtitles: return t({ "SUBTITLES", "LEGENDAS" });
        case Item::HeaderCamera: return t({ "THIRD-PERSON CAMERA", "CÂMERA EM TERCEIRA PESSOA" });
        case Item::HeaderLighting: return t({ "LIGHTING (REMASTER)", "ILUMINAÇÃO (REMASTER)" });
        case Item::HeaderDev: return t({ "DEVELOPER", "DESENVOLVEDOR" });
        case Item::HeaderKeys: return t({ "KEYBOARD AND MOUSE", "TECLADO E MOUSE" });
        case Item::DevConsole: return t({ "DEVELOPER CONSOLE", "CONSOLE DE DESENVOLVEDOR" });
        default: break;
        }
        return "";
    }

    std::string Value(Item item, const Values& values, const Context& context)
    {
        bool pt = context.pt;
        auto t = [&](const Text& text) { return std::string(T(pt, text)); };
        if (IsBinding(item))
        {
            std::string keys;
            for (uint16_t key : values.controls.keys[size_t(ActionOf(item))])
            {
                if (key)
                    keys += (keys.empty() ? "" : " / ") + KeyName(key);
            }
            return keys.empty() ? t({ "NONE", "NENHUMA" }) : keys;
        }
        if (int* percent = Percent(item, const_cast<Values&>(values)))
            return std::to_string(*percent) + "%";
        switch (item)
        {
        case Item::Window:
            switch (values.window)
            {
            case WindowMode::Borderless: return t({ "BORDERLESS", "SEM BORDAS" });
            case WindowMode::Fullscreen: return t({ "FULLSCREEN", "TELA CHEIA" });
            default: return t({ "WINDOWED", "JANELA" });
            }
        case Item::VSync: return OnOff(pt, values.vsync);
        case Item::AntiAliasing:
            switch (values.antiAliasing)
            {
            case AntiAliasing::Fxaa: return "FXAA";
            case AntiAliasing::Smaa: return "SMAA";
            default: return OnOff(pt, false);
            }
        case Item::Overlay:
            switch (values.overlay)
            {
            case OverlayMode::Fps: return t({ "FPS ONLY", "SÓ FPS" });
            case OverlayMode::FrameTime: return "FPS + FRAME TIME";
            case OverlayMode::Full: return t({ "FULL", "COMPLETO" });
            default: return OnOff(pt, false);
            }
        case Item::Resolution: return ResolutionOf(values.aspect, values.renderHeight).name;
        case Item::Aspect:
            if (values.aspect == Aspect::Auto)
                return std::string("AUTO (") + AspectName(EffectiveAspect(Aspect::Auto)) + ")";
            return std::string(AspectName(values.aspect)) + (values.aspect == Aspect::R16_9 ? " (ORIGINAL)" : "");
        case Item::Hud16x9: return OnOff(pt, values.hud16x9);
        case Item::FpsLimit:
            return values.fpsLimit ? std::to_string(values.fpsLimit) + (values.fpsLimit == 30 ? " (ORIGINAL)" : "") : t({ "UNLIMITED", "SEM LIMITE" });
        case Item::Upscaler:
            switch (values.upscaler)
            {
            case Upscaler::Dlss: return std::string("DLSS") + (context.dlss ? "" : T(pt, { " (REQUIRES NVIDIA RTX)", " (REQUER NVIDIA RTX)" }));
            case Upscaler::Fsr: return std::string("FSR 3.1") + (context.fsr ? "" : T(pt, { " (UNSUPPORTED GPU)", " (PLACA SEM SUPORTE)" }));
            default: return OnOff(pt, false);
            }
        case Item::UpscalerQuality:
            switch (values.upscalerQuality)
            {
            case UpscalerQuality::Quality: return t({ "QUALITY", "QUALIDADE" });
            case UpscalerQuality::Balanced: return t({ "BALANCED", "EQUILIBRADO" });
            case UpscalerQuality::Performance: return t({ "PERFORMANCE", "DESEMPENHO" });
            case UpscalerQuality::UltraPerformance: return t({ "ULTRA PERFORMANCE", "ULTRA DESEMPENHO" });
            default: return values.upscaler == Upscaler::Fsr ? t({ "NATIVE (AA)", "NATIVO (AA)" }) : t({ "NATIVE (DLAA)", "NATIVO (DLAA)" });
            }
        case Item::Anisotropy: return values.anisotropy > 1 ? std::to_string(values.anisotropy) + "X" : OnOff(pt, false);
        case Item::ShaderCompilation:
            switch (values.shaderCompilation)
            {
            case ShaderCompilation::Sync: return t({ "SYNCHRONOUS", "SINCRONIZADA" });
            case ShaderCompilation::Aggressive: return t({ "AGGRESSIVE", "AGRESSIVA" });
            default: return t({ "ASYNCHRONOUS", "ASSÍNCRONA" });
            }
        case Item::HdRemaster: return HdRemasterAvailable() ? OnOff(pt, values.hdRemaster) : t({ "UNAVAILABLE", "INDISPONÍVEL" });
        case Item::Language: return LanguageById(values.language).name;
        case Item::MojoColors: return values.ps2MojoColors ? "PS2" : "XBOX 360";
        case Item::CoopMode: return values.coopLeapfrog ? t({ "LEAPFROG", "PULA-SELA" }) : t({ "PIGGYBACK", "CAVALINHO" });
        case Item::InvertAxis: return OnOff(pt, values.invertAxis);
        case Item::Autosave: return OnOff(pt, values.autosave);
        case Item::SkipCutscenes: return OnOff(pt, values.skipCutscenes);
        case Item::Storage: return "HARD DRIVE";
        case Item::Subtitles:
            return values.subtitles && !context.haveSubtitles ? t({ "ON (NO TEXT)", "LIGADO (SEM TEXTO)" }) : OnOff(pt, values.subtitles);
        case Item::FreeCamera: return OnOff(pt, values.freeCamera);
        case Item::CameraInvertY: return OnOff(pt, values.cameraInvertY);
        case Item::DevConsole: return OnOff(pt, values.devConsole);
        case Item::LightSsao:
            switch (values.ssao)
            {
            case AoQuality::Low: return t({ "LOW", "BAIXA" });
            case AoQuality::High: return t({ "HIGH", "ALTA" });
            default: return OnOff(pt, false);
            }
        case Item::LightGi: return OnOff(pt, values.gi);
        case Item::LightContactShadows: return OnOff(pt, values.contactShadows);
        case Item::LightSoftShadows: return OnOff(pt, values.softShadows);
        case Item::Lighting:
            switch (LightingPresetOf(values))
            {
            case LightingPreset::Original: return "ORIGINAL";
            case LightingPreset::Remaster: return "REMASTER";
            default: return t({ "CUSTOM", "PERSONALIZADO" });
            }
        case Item::Prompts:
            switch (values.controls.prompts)
            {
            case PromptStyle::Xbox: return "XBOX";
            case PromptStyle::PlayStation: return "PLAYSTATION";
            case PromptStyle::Keyboard: return t({ "KEYBOARD", "TECLADO" });
            default: return t({ "AUTOMATIC", "AUTOMÁTICO" });
            }
        case Item::Player1:
        case Item::Player2:
        {
            const std::string& key = values.controls.players[item == Item::Player2];
            if (key == "auto")
                return "AUTO";
            return key.compare(0, 4, "xbox") == 0 ? t({ "CONTROLLER ", "CONTROLE " }) + key.substr(4) + " (XINPUT)" : "PLAYSTATION " + key.substr(11);
        }
        case Item::MouseCamera: return OnOff(pt, values.controls.mouseCamera);
        case Item::MouseInvertY: return OnOff(pt, values.controls.invertY);
        case Item::MouseMenus: return OnOff(pt, values.controls.mouseMenus);
        default: return "";
        }
    }

    std::pair<std::string, std::string> Help(Item item, const Values& values, const Context& context)
    {
        bool pt = context.pt;
        auto t = [&](const Text& text) { return std::string(T(pt, text)); };
        // The second line of the options that apply right away in the game / at its restart.
        std::string now = t({ "Applies when the game starts.", "Vale ao abrir o jogo." });
        std::string live = context.launcher ? now : t({ "Applies right away.", "Vale na hora." });
        std::string restart = context.launcher     ? now
                              : context.canRestart ? t({ "The game restarts to apply it (it asks first).", "O jogo reinicia para aplicar (pergunta antes)." })
                                                   : t({ "Applies when the game restarts.", "Vale ao reiniciar o jogo." });
        switch (item)
        {
        case Item::Master: return { t({ "Volume of everything the game plays.", "Volume de tudo o que o jogo toca." }), live };
        case Item::Music: return { t({ "Volume of the music.", "Volume da música." }), live };
        case Item::Effects: return { t({ "Volume of the sound effects and ambience.", "Volume dos efeitos e do som ambiente." }), live };
        case Item::Voice: return { t({ "Volume of the dialogue.", "Volume das vozes." }), live };
        case Item::Window:
            return { t({ "Window, borderless (a window the size of the monitor)", "Janela, sem bordas (uma janela do tamanho do monitor)" }),
                     t({ "or exclusive fullscreen. Alt+Enter also switches.", "ou tela cheia exclusiva. Alt+Enter também alterna." }) };
        case Item::Resolution:
            return { t({ "Internal resolution. With DLSS/FSR on, it is", "Resolução interna. Com DLSS/FSR ligado, é a" }),
                     t({ "the upscaler's output resolution.", "resolução de saída do upscaler." }) };
        case Item::FpsLimit:
            return { t({ "30 is the original. The game speed does not change.", "30 é o original. A velocidade do jogo não muda." }),
                     t({ "With V-Sync on, also limited by the monitor.", "Com V-Sync ligado, também limitado pelo monitor." }) };
        case Item::VSync:
            return { t({ "Waits for the monitor's refresh: no tearing.", "Espera a atualização do monitor: sem tearing." }),
                     t({ "Off by default.", "Desligado por padrão." }) };
        case Item::AntiAliasing:
            return { t({ "FXAA/SMAA smooth the edges of the final image.", "FXAA/SMAA suavizam as bordas na imagem final." }),
                     t({ "With DLSS/FSR on it can stay off.", "Com DLSS/FSR ligado pode ficar desligado." }) };
        case Item::Aspect:
            return { t({ "Wider screens see more to the sides (21:9, 32:9);", "Telas mais largas veem mais dos lados (21:9, 32:9);" }),
                     t({ "AUTO uses the monitor's; 16:10 sees more above.", "AUTO usa a do monitor; 16:10 vê mais em cima." }) };
        case Item::Hud16x9:
            return { t({ "On: HUD and menus in the middle 16:9 area.", "Ligado: HUD e menus na área 16:9 do meio." }),
                     t({ "Off: the HUD corners go to the screen's edges.", "Desligado: os cantos do HUD vão às bordas da tela." }) };
        case Item::Overlay:
            return { t({ "Performance overlay: FPS, frame time and graph.", "Overlay de desempenho: FPS, frame time e gráfico." }),
                     t({ "F1 switches it at any time.", "O F1 alterna a qualquer momento." }) };
        case Item::Upscaler:
            if (values.upscaler == Upscaler::Dlss && !context.dlss)
                return { t({ "DLSS needs an NVIDIA RTX card (with DLSS installed):", "O DLSS precisa de uma placa NVIDIA RTX (com o DLSS instalado):" }),
                         t({ "this PC plays with FSR or without an upscaler.", "este PC joga com FSR ou sem upscaler." }) };
            if (values.upscaler == Upscaler::Fsr && !context.fsr)
                return { t({ "This GPU or driver can't run FSR 3.1:", "Esta placa ou driver não roda o FSR 3.1:" }),
                         t({ "the game plays without an upscaler.", "o jogo roda sem upscaler." }) };
            return { t({ "Temporal AA + upscaling of the 3D scene, no", "AA temporal + upscaling da cena 3D, sem" }),
                     context.dlss ? t({ "frame generation. DLSS: NVIDIA RTX only.", "frame generation. DLSS: só NVIDIA RTX." })
                                  : t({ "frame generation. DLSS (requires NVIDIA RTX): not here.", "frame generation. DLSS (requer NVIDIA RTX): não aqui." }) };
        case Item::UpscalerQuality:
            return { t({ "Native = DLAA / FSR Native AA. The others", "Nativo = DLAA / FSR Native AA. Os outros" }),
                     t({ "draw the scene smaller and upscale it.", "desenham a cena menor e reescalam." }) };
        case Item::Sharpness:
            return { t({ "FSR 3.1 sharpening (RCAS) after upscaling.", "Nitidez do FSR 3.1 (RCAS) após o upscaling." }),
                     t({ "DLSS 4 does not use it.", "O DLSS 4 não usa este ajuste." }) };
        case Item::MotionBlur:
            return { t({ "Strength of the game's motion blur.", "Intensidade do motion blur do jogo." }),
                     t({ "0% turns it off, 100% = original.", "0% desliga, 100% = original." }) };
        case Item::DepthOfField:
            return { t({ "Distance blur of the game's scenes.", "Desfoque de distância das cenas do jogo." }),
                     t({ "0% turns it off, 100% = original.", "0% desliga, 100% = original." }) };
        case Item::Anisotropy:
            return { t({ "Sharpness of textures seen at an angle", "Nitidez das texturas vistas de lado" }),
                     t({ "(ground, walls, paths in the distance).", "(chão, paredes, caminhos ao longe)." }) };
        case Item::ShadowSoftness:
            return { t({ "Radius of the filter on the shadows' edges.", "Raio do filtro da borda das sombras." }),
                     t({ "0% = hard edge, 100% = original.", "0% = borda dura, 100% = original." }) };
        case Item::ShaderCompilation:
            switch (values.shaderCompilation)
            {
            case ShaderCompilation::Sync:
                return { t({ "Compiles each new effect when it first shows up", "Compila cada efeito novo quando ele aparece" }),
                         t({ "(short hitches). Use it if ASYNCHRONOUS fails.", "(engasgos curtos). Use se a ASSÍNCRONA falhar." }) };
            case ShaderCompilation::Aggressive:
                return { t({ "In the background, and skips for a few frames", "Em segundo plano, e pula por alguns quadros" }),
                         t({ "what can wait (effects may show up late).", "o que pode esperar (efeitos podem aparecer depois)." }) };
            default:
                return { t({ "Compiles new shaders and effects in the background:", "Compila shaders e efeitos novos em segundo plano:" }),
                         t({ "no hitches. ", "sem engasgos. " }) + live };
            }
        case Item::HdRemaster:
            if (!HdRemasterAvailable())
                return { t({ "The remaster files are not on this PC", "Os arquivos do remaster não estão neste PC" }),
                         t({ "(data\\remaster_full next to the game).", "(data\\remaster_full ao lado do jogo)." }) };
            if (HdRemasterWholeGame())
                return { t({ "AI-remastered textures and models (whole game).", "Texturas e modelos remasterizados por IA (jogo todo)." }), restart };
            return { t({ "AI-remastered textures and models (level 1).", "Texturas e modelos remasterizados por IA (fase 1)." }), restart };
        case Item::Language:
        {
            // Language packs (languages\): their own lines, e.g. that the voices stay in English.
            const Language& language = LanguageById(values.language);
            if (language.help[0])
                return { language.help[0], language.help[1] ? language.help[1] : "" };
            return { t({ "Language of the texts and the voices.", "Idioma dos textos e das vozes." }), restart };
        }
        case Item::MojoColors:
            return { t({ "PS2: large one light blue, extra large purple.", "PS2: grande azul bem claro, extra grande roxo." }),
                     t({ "Xbox 360: all blue (the original).", "Xbox 360: todos azuis (o original)." }) };
        case Item::CoopMode:
            return { t({ "LEAPFROG: take turns jumping through the challenges.", "PULA-SELA: vocês se revezam pulando pelos desafios." }),
                     t({ "PIGGYBACK: ride in your partner's backpack.", "CAVALINHO: fique na mochila do seu parceiro." }) };
        case Item::InvertAxis: return { t({ "Inverts the camera's axis (the game's own option).", "Inverte o eixo da câmera (a opção do próprio jogo)." }), live };
        case Item::Autosave:
            return { t({ "Saves the game in the slot chosen for it.", "Salva o jogo no slot escolhido para ele." }),
                     t({ "Off: the game does not save by itself.", "Desligado: o jogo não salva sozinho." }) };
        case Item::SkipCutscenes:
            return { t({ "Hold B (ESC, SPACE, ENTER) during a cutscene or video", "Segure B (ESC, ESPAÇO, ENTER) numa cena ou vídeo" }),
                     t({ "to skip it; a prompt shows in the corner.", "para pulá-la; um aviso aparece no canto." }) };
        case Item::Storage:
            return { t({ "Saves are kept on the PC, in the userdata folder.", "Os saves ficam no PC, na pasta userdata." }),
                     t({ "The Y button of the main menu picks it.", "O botão Y do menu principal o escolhe." }) };
        case Item::Subtitles:
            if (!context.haveSubtitles)
                return { t({ "No subtitles for this language on this PC", "Sem legendas para este idioma neste PC" }),
                         t({ "(data\\subtitles, README \"Subtitles\").", "(data\\subtitles, README \"Legendas\")." }) };
            return { t({ "Subtitles in cutscenes and dialogue (transcribed", "Legendas nas cenas e falas (transcritas" }),
                     t({ "from the voices), in the language of the texts.", "das vozes), no idioma dos textos." }) };
        case Item::SubtitleSize: return { t({ "Size of the subtitle letters.", "Tamanho das letras da legenda." }), t({ "100% = default.", "100% = padrão." }) };
        case Item::SubtitleBackground:
            return { t({ "Opacity of the box behind the text.", "Opacidade da caixa atrás do texto." }), t({ "0% = outline only.", "0% = só o contorno." }) };
        case Item::FreeCamera:
            return { t({ "Third-person orbit camera: right stick / mouse,", "Câmera orbital em terceira pessoa: analógico" }),
                     t({ "R3 recenters. Cutscenes keep the game's camera.", "direito / mouse, R3 recentra. Cenas: a do jogo." }) };
        case Item::CameraSensitivity:
            return { t({ "Turn speed of the free camera.", "Velocidade de giro da câmera livre." }), t({ "100% = default.", "100% = padrão." }) };
        case Item::CameraInvertY:
            return { t({ "Up on the stick / mouse looks down.", "Para cima no analógico / mouse olha para baixo." }), t({ "Free camera only.", "Só na câmera livre." }) };
        case Item::CameraDistance:
            return { t({ "Distance from the camera to the character.", "Distância da câmera ao personagem." }),
                     t({ "Walls bring it closer by themselves.", "Paredes a aproximam sozinhas." }) };
        case Item::DevConsole:
            return { t({ "The key left of 1 (or F10) opens a console:", "A tecla antes do 1 (ou F10) abre um console:" }),
                     t({ "unlock all, mojo, god, skin, level, lua... (help).", "unlock all, mojo, god, skin, level, lua... (help)." }) };
        case Item::LightSsao:
            return { t({ "Darkens corners, contacts and creases of the scene.", "Escurece cantos, contatos e dobras da cena." }),
                     t({ "LOW: half resolution, HIGH: full. F8 toggles it all.", "BAIXA: meia resolução, ALTA: cheia. F8 liga/desliga." }) };
        case Item::LightGi:
            return { t({ "Light bounced by nearby surfaces tints the scene", "A luz rebatida nas superfícies próximas tinge a" }),
                     t({ "with their colour (screen space, one bounce).", "cena com a cor delas (espaço de tela, um rebote)." }) };
        case Item::LightGiIntensity:
            return { t({ "Strength of the indirect light.", "Intensidade da luz indireta." }), t({ "100% = default.", "100% = padrão." }) };
        case Item::LightContactShadows:
            return { t({ "Small shadows under characters, objects and", "Sombras pequenas sob personagens, objetos e" }),
                     t({ "ledges, where they meet the ground.", "beiradas, onde encostam no chão." }) };
        case Item::LightSoftShadows:
            return { t({ "The game's shadows with a wider, smoother filter.", "As sombras do jogo com um filtro mais largo e suave." }),
                     t({ "SHADOW SOFTNESS (GRAPHICS) still sets the radius.", "A SUAVIDADE DAS SOMBRAS (GRÁFICOS) ajusta o raio." }) };
        case Item::Lighting:
            switch (LightingPresetOf(values))
            {
            case LightingPreset::Original:
                return { t({ "The game's original lighting.", "A iluminação original do jogo." }),
                         t({ "REMASTER: SSAO, indirect light, contact and soft shadows.", "REMASTER: SSAO, luz indireta, sombras de contato e suaves." }) };
            case LightingPreset::Remaster:
                return { t({ "Ambient occlusion, indirect light, contact and", "Oclusão de ambiente, luz indireta, sombras de" }),
                         t({ "soft shadows (each one in ADVANCED). F8 toggles.", "contato e suaves (cada uma no AVANÇADO). F8 alterna." }) };
            default:
                return { t({ "A mix of the lighting options, set one by one in", "Uma mistura das opções de iluminação, ajustadas em" }),
                         t({ "ADVANCED > DEFINITIVE EDITION.", "AVANÇADO > DEFINITIVE EDITION." }) };
            }
        case Item::Player1:
            return { t({ "Controller of player 1 (with the keyboard and mouse).", "Controle do jogador 1 (junto com teclado e mouse)." }),
                     t({ "AUTO: the one that pressed START on the title screen.", "AUTO: o que apertou START na tela título." }) };
        case Item::Player2:
            return { t({ "Controller of player 2: B joins the game, Y leaves", "Controle do jogador 2: B entra no jogo, Y desce" }),
                     t({ "the backpack. A single controller is always player 1.", "da mochila. Com um controle só, ele é o jogador 1." }) };
        case Item::Prompts:
            return { t({ "Icons in menus and hints: the last device used,", "Ícones nos menus e dicas: o último aparelho usado," }),
                     t({ "or always Xbox / PlayStation / keyboard.", "ou sempre Xbox / PlayStation / teclado." }) };
        case Item::MouseCamera:
            return { t({ "The mouse turns the camera in gameplay", "O mouse gira a câmera na gameplay" }),
                     t({ "(the cursor is hidden and kept in the window).", "(o cursor fica escondido e preso na janela)." }) };
        case Item::MouseSensitivity:
            return { t({ "Mouse movement -> camera speed.", "Movimento do mouse -> velocidade da câmera." }), t({ "100% = default.", "100% = padrão." }) };
        case Item::MouseInvertY: return { t({ "Mouse / camera keys: up looks down.", "Mouse / teclas da câmera: para cima olha para baixo." }), "" };
        case Item::WalkSpeed:
            return { t({ "How far the stick goes while WALK is held", "Quanto o analógico vai com ANDAR DEVAGAR apertado" }),
                     t({ "(Crash walks instead of running).", "(o Crash anda em vez de correr)." }) };
        case Item::MouseMenus:
            return { t({ "Pointing at an item selects it, click accepts,", "Apontar um item seleciona, clique confirma," }),
                     t({ "right button goes back, the wheel scrolls.", "botão direito volta, a roda rola." }) };
        default: break;
        }
        return {};
    }

    int* Percent(Item item, Values& values, int* minimum, int* maximum, int* step)
    {
        int min = 0, max = 100, stepValue = 5;
        int* value = nullptr;
        switch (item)
        {
        case Item::Master: value = &values.masterVolume; break;
        case Item::Music: value = &values.musicVolume; break;
        case Item::Effects: value = &values.effectsVolume; break;
        case Item::Voice: value = &values.voiceVolume; break;
        case Item::Sharpness: value = &values.sharpness; stepValue = 10; break;
        case Item::MotionBlur: value = &values.motionBlur; stepValue = 10; break;
        case Item::DepthOfField: value = &values.depthOfField; stepValue = 10; break;
        case Item::ShadowSoftness: value = &values.shadowSoftness; stepValue = 25; max = 200; break;
        case Item::SubtitleSize: value = &values.subtitleSize; stepValue = 25; min = 50; max = 200; break;
        case Item::SubtitleBackground: value = &values.subtitleBackground; stepValue = 25; break;
        case Item::CameraSensitivity: value = &values.cameraSensitivity; stepValue = 25; min = 25; max = 300; break;
        case Item::CameraDistance: value = &values.cameraDistance; stepValue = 10; min = 50; max = 200; break;
        case Item::LightGiIntensity: value = &values.giIntensity; stepValue = 10; max = 200; break;
        case Item::MouseSensitivity: value = &values.controls.mouseSensitivity; stepValue = 10; min = 10; max = 400; break;
        case Item::WalkSpeed: value = &values.controls.walkSpeed; stepValue = 5; min = 10; max = 90; break;
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

    bool Unavailable(Item item, const Values& values, const Context& context)
    {
        return item == Item::Upscaler && ((values.upscaler == Upscaler::Dlss && !context.dlss) || (values.upscaler == Upscaler::Fsr && !context.fsr));
    }

    bool Change(Item item, Values& values, int direction, const Context& context)
    {
        int minimum, maximum, step;
        if (int* percent = Percent(item, values, &minimum, &maximum, &step))
        {
            *percent = std::clamp(*percent + direction * step, minimum, maximum);
            return true;
        }
        switch (item)
        {
        case Item::Window: values.window = Cycle(values.window, 3, direction); break;
        case Item::VSync: values.vsync = !values.vsync; break;
        case Item::FpsLimit:
        {
            constexpr int LIMITS[] = { 30, 60, 120, 0 };
            int index = int(std::find(std::begin(LIMITS), std::end(LIMITS), values.fpsLimit) - std::begin(LIMITS)) % 4;
            values.fpsLimit = LIMITS[(index + 4 + direction) % 4];
            break;
        }
        case Item::Resolution:
        {
            size_t count;
            const Resolution* list = Resolutions(values.aspect, count);
            size_t index = size_t(&ResolutionOf(values.aspect, values.renderHeight) - list);
            values.renderHeight = list[(index + count + direction) % count].height;
            break;
        }
        case Item::Aspect:
            // The resolution follows: the same height, or the nearest one of the new aspect's list.
            values.aspect = Cycle(values.aspect, 5, direction);
            values.renderHeight = ResolutionOf(values.aspect, values.renderHeight).height;
            break;
        case Item::Hud16x9: values.hud16x9 = !values.hud16x9; break;
        case Item::AntiAliasing: values.antiAliasing = Cycle(values.antiAliasing, 3, direction); break;
        case Item::Overlay: values.overlay = Cycle(values.overlay, 4, direction); break;
        case Item::Upscaler:
            // The ones this PC can't run are skipped (OFF always can).
            for (int i = 0; i < 3; i++)
            {
                values.upscaler = Cycle(values.upscaler, 3, direction);
                if (!Unavailable(item, values, context))
                    break;
            }
            break;
        case Item::UpscalerQuality: values.upscalerQuality = Cycle(values.upscalerQuality, 5, direction); break;
        case Item::ShaderCompilation:
            // ASYNCHRONOUS <-> SYNCHRONOUS (AGGRESSIVE only from settings.toml).
            values.shaderCompilation = values.shaderCompilation == ShaderCompilation::Sync ? ShaderCompilation::Async : ShaderCompilation::Sync;
            break;
        case Item::Anisotropy:
        {
            constexpr int LEVELS[] = { 1, 2, 4, 8, 16 };
            int index = int(std::find(std::begin(LEVELS), std::end(LEVELS), values.anisotropy) - std::begin(LEVELS)) % 5;
            values.anisotropy = LEVELS[(index + 5 + direction) % 5];
            break;
        }
        case Item::Language:
        {
            size_t count;
            const Language* languages = Languages(count);
            size_t index = size_t(&LanguageById(values.language) - languages);
            values.language = languages[(index + count + direction) % count].id;
            break;
        }
        case Item::MojoColors: values.ps2MojoColors = !values.ps2MojoColors; break;
        case Item::CoopMode: values.coopLeapfrog = !values.coopLeapfrog; break;
        case Item::InvertAxis: values.invertAxis = !values.invertAxis; break;
        case Item::Autosave: values.autosave = !values.autosave; break;
        case Item::SkipCutscenes: values.skipCutscenes = !values.skipCutscenes; break;
        case Item::HdRemaster:
            if (HdRemasterAvailable())  // shown as unavailable: the saved value is kept for when the data is back
                values.hdRemaster = !values.hdRemaster;
            break;
        case Item::Subtitles: values.subtitles = !values.subtitles; break;
        case Item::FreeCamera: values.freeCamera = !values.freeCamera; break;
        case Item::CameraInvertY: values.cameraInvertY = !values.cameraInvertY; break;
        case Item::DevConsole: values.devConsole = !values.devConsole; break;
        case Item::LightSsao: values.ssao = Cycle(values.ssao, 3, direction); break;
        case Item::LightGi: values.gi = !values.gi; break;
        case Item::LightContactShadows: values.contactShadows = !values.contactShadows; break;
        case Item::LightSoftShadows: values.softShadows = !values.softShadows; break;
        case Item::Lighting:
        {
            // ORIGINAL <-> REMASTER; from CUSTOM, RIGHT goes to REMASTER and LEFT to ORIGINAL.
            LightingPreset preset = LightingPresetOf(values);
            if (preset == LightingPreset::Custom)
                preset = direction > 0 ? LightingPreset::Remaster : LightingPreset::Original;
            else
                preset = preset == LightingPreset::Original ? LightingPreset::Remaster : LightingPreset::Original;
            SetLightingPreset(values, preset);
            break;
        }
        case Item::Prompts: values.controls.prompts = Cycle(values.controls.prompts, 4, direction); break;
        case Item::Player1:
        case Item::Player2:
        {
            static const char* const KEYS[] = { "auto", "xbox1", "xbox2", "xbox3", "xbox4", "playstation1", "playstation2", "playstation3", "playstation4" };
            constexpr int COUNT = int(std::size(KEYS));
            std::string& key = values.controls.players[item == Item::Player2];
            int index = 0;
            while (index < COUNT && key != KEYS[index])
                index++;
            key = KEYS[((index % COUNT) + COUNT + direction) % COUNT];
            break;
        }
        case Item::MouseCamera: values.controls.mouseCamera = !values.controls.mouseCamera; break;
        case Item::MouseInvertY: values.controls.invertY = !values.controls.invertY; break;
        case Item::MouseMenus: values.controls.mouseMenus = !values.controls.mouseMenus; break;
        default: return false;
        }
        return true;
    }

    bool RestartOnly(Item item)
    {
        return item == Item::Language || item == Item::MojoColors || item == Item::HdRemaster;
    }

    LightingPreset LightingPresetOf(const Values& values)
    {
        Values original = values, remaster = values;
        SetLightingPreset(original, LightingPreset::Original);
        SetLightingPreset(remaster, LightingPreset::Remaster);
        // The intensity of the indirect light only counts with the indirect light on.
        auto same = [&](const Values& preset) {
            return values.ssao == preset.ssao && values.gi == preset.gi && (!values.gi || values.giIntensity == preset.giIntensity) &&
                   values.contactShadows == preset.contactShadows && values.softShadows == preset.softShadows;
        };
        if (same(original))
            return LightingPreset::Original;
        if (same(remaster))
            return LightingPreset::Remaster;
        return LightingPreset::Custom;
    }

    void SetLightingPreset(Values& values, LightingPreset preset)
    {
        if (preset == LightingPreset::Custom)
            return;
        bool on = preset == LightingPreset::Remaster;
        values.ssao = on ? AoQuality::High : AoQuality::Off;
        values.gi = on;
        if (on)
            values.giIntensity = Values().giIntensity;
        values.contactShadows = on;
        values.softShadows = on;
    }
}
