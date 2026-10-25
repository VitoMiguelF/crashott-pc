#include "settings.h"
#include "../diagnostics.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

namespace settings
{
    namespace
    {
        // XConfig languages: 1 English, 3 German, 4 French, 5 Spanish, 6 Italian. The game turns
        // them into text bible letters (sub_82288230) and keeps F/G/I/S; everything else becomes
        // E, or D when the console country is the Netherlands (sub_821D80C0), which then also
        // mounts dutch.rcf (sub_821F9498). The Nordic/Russian voice archives are not in this copy.
        constexpr Language GAME_LANGUAGES[] = {
            { "english", "ENGLISH", 1, 103, 'E', "english.rcf" },
            { "french", "FRANÇAIS", 4, 34, 'F', "french.rcf" },
            { "german", "DEUTSCH", 3, 24, 'G', "german.rcf" },
            { "spanish", "ESPAÑOL", 5, 31, 'S', "spanish.rcf" },
            { "italian", "ITALIANO", 6, 50, 'I', "italian.rcf" },
            { "dutch", "NEDERLANDS", 1, 74, 'D', "dutch.rcf" },
        };
        // The game's languages, then the language packs (AddLanguage, only during startup).
        std::vector<Language> LANGUAGES(std::begin(GAME_LANGUAGES), std::end(GAME_LANGUAGES));

        // The resolutions of each aspect (the usual monitor sizes; 21:9 monitors are 64:27 or 43:18).
        const std::vector<Resolution> RESOLUTIONS_16_9 = {
            { "1280x720", 1280, 720 }, { "1600x900", 1600, 900 }, { "1920x1080", 1920, 1080 }, { "2560x1440", 2560, 1440 }, { "3840x2160", 3840, 2160 },
        };
        const std::vector<Resolution> RESOLUTIONS_16_10 = {
            { "1280x800", 1280, 800 }, { "1440x900", 1440, 900 }, { "1680x1050", 1680, 1050 }, { "1920x1200", 1920, 1200 },
            { "2560x1600", 2560, 1600 }, { "3840x2400", 3840, 2400 },
        };
        const std::vector<Resolution> RESOLUTIONS_21_9 = {
            { "1720x720", 1720, 720 }, { "2560x1080", 2560, 1080 }, { "3440x1440", 3440, 1440 }, { "3840x1600", 3840, 1600 }, { "5120x2160", 5120, 2160 },
        };
        const std::vector<Resolution> RESOLUTIONS_32_9 = {
            { "2560x720", 2560, 720 }, { "3840x1080", 3840, 1080 }, { "5120x1440", 5120, 1440 }, { "7680x2160", 7680, 2160 },
        };
        // Auto: the list of the monitor's aspect, plus the monitor's own resolution (AutoResolutions).
        std::vector<Resolution> g_autoResolutions;
        std::string g_monitorName;
        std::once_flag g_autoOnce;

        const std::vector<Resolution>& NamedResolutions(Aspect aspect)
        {
            switch (aspect)
            {
            case Aspect::R16_10: return RESOLUTIONS_16_10;
            case Aspect::R21_9: return RESOLUTIONS_21_9;
            case Aspect::R32_9: return RESOLUTIONS_32_9;
            default: return RESOLUTIONS_16_9;
            }
        }

        const std::vector<Resolution>& AutoResolutions()
        {
            std::call_once(g_autoOnce, [] {
                g_autoResolutions = NamedResolutions(EffectiveAspect(Aspect::Auto));
                int width, height;
                MonitorSize(width, height);
                bool listed = false;
                for (const Resolution& resolution : g_autoResolutions)
                    listed |= resolution.width == width && resolution.height == height;
                if (!listed && width >= 640 && height >= 480)
                {
                    // A monitor of its own size (e.g. 1366x768, 3840x1600 is listed): its native
                    // resolution replaces the entry of the same height, or goes in by height.
                    g_monitorName = std::to_string(width) + "x" + std::to_string(height);
                    Resolution native{ g_monitorName.c_str(), width, height };
                    auto it = std::find_if(g_autoResolutions.begin(), g_autoResolutions.end(), [&](const Resolution& r) { return r.height >= height; });
                    if (it != g_autoResolutions.end() && it->height == height)
                        *it = native;
                    else
                        g_autoResolutions.insert(it, native);
                }
            });
            return g_autoResolutions;
        }

        const std::vector<Resolution>& ResolutionList(Aspect aspect)
        {
            return aspect == Aspect::Auto ? AutoResolutions() : NamedResolutions(aspect);
        }

        std::mutex g_mutex;
        Values g_values;
        Values g_startup;
        std::filesystem::path g_path;

        std::string Trim(std::string text)
        {
            size_t begin = text.find_first_not_of(" \t\r\n");
            size_t end = text.find_last_not_of(" \t\r\n");
            return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
        }

        std::string Lower(std::string text)
        {
            for (char& c : text)
                c = char(std::tolower(uint8_t(c)));
            return text;
        }

        // Value text without a trailing comment; strings lose their quotes.
        std::string ParseValue(const std::string& raw, bool& isString)
        {
            std::string value = Trim(raw);
            isString = !value.empty() && value[0] == '"';
            if (isString)
            {
                size_t close = value.find('"', 1);
                return close == std::string::npos ? value.substr(1) : value.substr(1, close - 1);
            }
            size_t comment = value.find('#');
            return Trim(comment == std::string::npos ? value : value.substr(0, comment));
        }

        bool ParseBool(const std::string& text, bool& out)
        {
            if (text == "true")
                out = true;
            else if (text == "false")
                out = false;
            else
                return false;
            return true;
        }

        bool ParsePercent(const std::string& text, int& out, long maximum = 100)
        {
            char* end;
            long value = strtol(text.c_str(), &end, 10);
            if (end == text.c_str() || *end)
                return false;
            out = int(std::clamp(value, 0L, maximum));
            return true;
        }

        void Apply(Values& values, const std::string& section, const std::string& key, const std::string& value, int line)
        {
            bool ok = true;
            std::string name = section + "." + key;
            std::string lower = Lower(value);
            if (name == "audio.master")
                ok = ParsePercent(value, values.masterVolume);
            else if (name == "audio.music")
                ok = ParsePercent(value, values.musicVolume);
            else if (name == "audio.effects")
                ok = ParsePercent(value, values.effectsVolume);
            else if (name == "audio.voice")
                ok = ParsePercent(value, values.voiceVolume);
            else if (name == "video.window")
            {
                if (lower == "windowed")
                    values.window = WindowMode::Windowed;
                else if (lower == "borderless")
                    values.window = WindowMode::Borderless;
                else if (lower == "fullscreen")
                    values.window = WindowMode::Fullscreen;
                else
                    ok = false;
            }
            else if (name == "video.vsync")
                ok = ParseBool(lower, values.vsync);
            else if (name == "video.fps_limit")
            {
                if (lower == "30" || lower == "60" || lower == "120" || lower == "0")
                    values.fpsLimit = atoi(lower.c_str());
                else if (lower == "off" || lower == "unlimited")
                    values.fpsLimit = 0;
                else
                    ok = false;
            }
            else if (name == "video.resolution")
            {
                // "WxH": the height; the width follows video.aspect (ResolutionOf).
                int width = 0, height = 0;
                ok = sscanf(lower.c_str(), "%dx%d", &width, &height) == 2 && height >= 480 && height <= 4320;
                if (ok)
                    values.renderHeight = height;
            }
            else if (name == "video.aspect")
            {
                if (lower == "auto")
                    values.aspect = Aspect::Auto;
                else if (lower == "16:9")
                    values.aspect = Aspect::R16_9;
                else if (lower == "16:10")
                    values.aspect = Aspect::R16_10;
                else if (lower == "21:9")
                    values.aspect = Aspect::R21_9;
                else if (lower == "32:9")
                    values.aspect = Aspect::R32_9;
                else
                    ok = false;
            }
            else if (name == "video.hud_16_9")
                ok = ParseBool(lower, values.hud16x9);
            else if (name == "video.overlay")
                ok = ParseOverlayMode(lower, values.overlay);
            else if (name == "video.show_fps")  // before the overlay modes
            {
                bool show;
                ok = ParseBool(lower, show);
                if (ok)
                    values.overlay = show ? OverlayMode::Full : OverlayMode::Off;
            }
            else if (name == "video.antialiasing")
            {
                if (lower == "off")
                    values.antiAliasing = AntiAliasing::Off;
                else if (lower == "fxaa")
                    values.antiAliasing = AntiAliasing::Fxaa;
                else if (lower == "smaa")
                    values.antiAliasing = AntiAliasing::Smaa;
                else
                    ok = false;
            }
            else if (name == "video.gpu_safe_mode")  // checked by the renderer (rhi::ParseSafeMode)
                values.gpuSafeMode = lower;
            else if (name == "video.gpu_diagnostics")
                ok = ParseBool(lower, values.gpuDiagnostics);
            else if (name == "video.renderer")
            {
                // The Direct3D 12 / Vulkan choice of the 2026-10-23 builds: Vulkan is the only renderer now.
            }
            else if (name == "graphics.upscaler")
            {
                if (lower == "off")
                    values.upscaler = Upscaler::Off;
                else if (lower == "dlss" || lower == "dlaa")
                    values.upscaler = Upscaler::Dlss;
                else if (lower == "fsr" || lower == "fsr3" || lower == "fsr3.1")
                    values.upscaler = Upscaler::Fsr;
                else
                    ok = false;
            }
            else if (name == "graphics.upscaler_quality")
            {
                ok = false;
                for (int i = 0; i <= int(UpscalerQuality::UltraPerformance); i++)
                {
                    if (lower == UpscalerQualityName(UpscalerQuality(i)))
                    {
                        values.upscalerQuality = UpscalerQuality(i);
                        ok = true;
                    }
                }
                if (lower == "dlaa" || lower == "native_aa")
                {
                    values.upscalerQuality = UpscalerQuality::Native;
                    ok = true;
                }
            }
            else if (name == "graphics.sharpness")
                ok = ParsePercent(value, values.sharpness);
            else if (name == "graphics.motion_blur")
                ok = ParsePercent(value, values.motionBlur);
            else if (name == "graphics.depth_of_field")
                ok = ParsePercent(value, values.depthOfField);
            else if (name == "graphics.shadow_softness")
                ok = ParsePercent(value, values.shadowSoftness, 200);
            else if (name == "graphics.anisotropic_filtering")
            {
                if (lower == "off")
                    lower = "1";
                ok = lower == "1" || lower == "2" || lower == "4" || lower == "8" || lower == "16";
                if (ok)
                    values.anisotropy = atoi(lower.c_str());
            }
            else if (name == "graphics.hd_textures")
                ok = ParseBool(lower, values.hdTextures);
            else if (name == "graphics.texture_dump")
                ok = ParseBool(lower, values.textureDump);
            else if (name == "graphics.hd_remaster")
                ok = ParseBool(lower, values.hdRemaster);
            else if (name == "game.language")
            {
                size_t count;
                const Language* languages = Languages(count);
                ok = std::any_of(languages, languages + count, [&](const Language& l) { return lower == l.id; });
                if (ok)
                    values.language = lower;
            }
            else if (name == "game.mojo_colors")
            {
                if (lower == "ps2")
                    values.ps2MojoColors = true;
                else if (lower == "xbox360" || lower == "x360" || lower == "360")
                    values.ps2MojoColors = false;
                else
                    ok = false;
            }
            else if (name == "game.subtitles")
                ok = ParseBool(lower, values.subtitles);
            else if (name == "game.subtitle_size")
            {
                int size;
                ok = ParsePercent(value, size, 200) && size >= 50;
                if (ok)
                    values.subtitleSize = size;
            }
            else if (name == "game.subtitle_background")
                ok = ParsePercent(value, values.subtitleBackground);
            else if (name == "game.coop_mode")
            {
                if (lower == "leapfrog")
                    values.coopLeapfrog = true;
                else if (lower == "piggyback")
                    values.coopLeapfrog = false;
                else
                    ok = false;
            }
            else if (name == "game.invert_axis")
                ok = ParseBool(lower, values.invertAxis);
            else if (name == "game.autosave")
                ok = ParseBool(lower, values.autosave);
            else if (name == "game.skip_cutscenes")
                ok = ParseBool(lower, values.skipCutscenes);
            else if (section == "controls" && ApplyControl(values.controls, key, value, ok))
                ;
            else if (name == "paths.game")
                values.gamePath = value;
            else if (name == "lighting.ssao")
            {
                if (lower == "off" || lower == "false" || lower == "0")
                    values.ssao = AoQuality::Off;
                else if (lower == "low" || lower == "true" || lower == "1")
                    values.ssao = AoQuality::Low;
                else if (lower == "high" || lower == "2")
                    values.ssao = AoQuality::High;
                else
                    ok = false;
            }
            else if (name == "lighting.gi")
                ok = ParseBool(lower, values.gi);
            else if (name == "lighting.gi_intensity")
                ok = ParsePercent(value, values.giIntensity, 200);
            else if (name == "lighting.contact_shadows")
                ok = ParseBool(lower, values.contactShadows);
            else if (name == "lighting.soft_shadows")
                ok = ParseBool(lower, values.softShadows);
            else if (name == "camera.free_camera")
                ok = ParseBool(lower, values.freeCamera);
            else if (name == "camera.sensitivity")
            {
                int percent;
                ok = ParsePercent(value, percent, 300) && percent >= 25;
                if (ok)
                    values.cameraSensitivity = percent;
            }
            else if (name == "camera.invert_y")
                ok = ParseBool(lower, values.cameraInvertY);
            else if (name == "camera.distance")
            {
                int percent;
                ok = ParsePercent(value, percent, 200) && percent >= 50;
                if (ok)
                    values.cameraDistance = percent;
            }
            else if (name == "dev.console")
                ok = ParseBool(lower, values.devConsole);
            else if (name == "dev.hulk_skin")
                ok = ParseBool(lower, values.hulkSkin);
            else if (name == "launcher.view")
            {
                ok = lower == "simple" || lower == "advanced";
                if (ok)
                    values.launcherAdvanced = lower == "advanced";
            }
            else
            {
                Log("[settings] %s:%d: unknown setting %s (ignored)", g_path.filename().string().c_str(), line, name.c_str());
                return;
            }
            if (!ok)
                Log("[settings] %s:%d: invalid value \"%s\" for %s (default kept)", g_path.filename().string().c_str(), line, value.c_str(), name.c_str());
        }

        std::string Serialize(const Values& v)
        {
            std::ostringstream out;
            out << "# CrashRecomp: configuracoes do runtime (tela SETTINGS no menu principal).\n"
                   "# Editavel a mao com o jogo fechado; valores invalidos voltam ao padrao.\n\n";
            if (!v.gamePath.empty())
                out << "[paths]\n"
                       "game = \"" << v.gamePath << "\"   # pasta do jogo (default.xex, default.rcf); sem esta linha: a pasta game\\ ao lado do exe\n\n";
            out << "[audio]\n"
                   "master = " << v.masterVolume << "     # 0-100, vale na hora\n"
                   "music = " << v.musicVolume << "      # 0-100, musica (vale na hora)\n"
                   "effects = " << v.effectsVolume << "    # 0-100, efeitos e ambiente (vale na hora)\n"
                   "voice = " << v.voiceVolume << "      # 0-100, dialogos (vale na hora)\n\n"
                   "[video]\n"
                   "window = \"" << WindowModeName(v.window) << "\"   # windowed | borderless | fullscreen\n"
                   "vsync = " << (v.vsync ? "true" : "false") << "\n"
                   "fps_limit = " << v.fpsLimit << "   # 30 (original) | 60 | 120 | 0 = sem limite\n"
                   "resolution = \"" << ResolutionOf(v.aspect, v.renderHeight).name << "\"   # resolucao interna: 1280x720 (original) | 1600x900 | 1920x1080 | 2560x1440 | 3840x2160; 21:9 2560x1080 | 3440x1440...; 32:9 5120x1440...; 16:10 1280x800... (a largura segue aspect; vale ao sair da tela SETTINGS)\n"
                   "aspect = \"" << AspectName(v.aspect) << "\"   # auto (o do monitor) | 16:9 (original) | 16:10 | 21:9 | 32:9: telas mais largas veem mais dos lados (vale ao sair da tela SETTINGS)\n"
                   "hud_16_9 = " << (v.hud16x9 ? "true" : "false") << "   # HUD e menus na area 16:9 do meio; false = cantos do HUD nas bordas da tela (vale na hora)\n"
                   "overlay = \"" << OverlayModeName(v.overlay) << "\"   # off | fps | frametime | full (F1 alterna)\n"
                   "antialiasing = \"" << AntiAliasingName(v.antiAliasing) << "\"   # off | fxaa | smaa (pos-processamento da imagem final)\n"
                   "gpu_safe_mode = \"" << v.gpuSafeMode << "\"   # off | on | lista: barriers, instances, serial, memory, depth, present, cache (telas pretas em alguns drivers; ao reiniciar)\n"
                   "gpu_diagnostics = " << (v.gpuDiagnostics ? "true" : "false") << "   # detalhes da GPU e do driver no log (ao reiniciar)\n\n"
                   "[graphics]\n"
                   "upscaler = \"" << UpscalerName(v.upscaler) << "\"   # off | dlss (DLSS/DLAA, so NVIDIA RTX) | fsr (FSR 3.1, qualquer GPU) (vale ao sair da tela SETTINGS)\n"
                   "upscaler_quality = \"" << UpscalerQualityName(v.upscalerQuality) << "\"   # native (DLAA / FSR Native AA) | quality | balanced | performance | ultra_performance (vale ao sair da tela SETTINGS)\n"
                   "sharpness = " << v.sharpness << "   # 0-100, nitidez do FSR (RCAS); o DLSS 4 nao tem\n"
                   "motion_blur = " << v.motionBlur << "   # 0-100 %, 0 = sem motion blur\n"
                   "depth_of_field = " << v.depthOfField << "   # 0-100 %, 0 = sem profundidade de campo\n"
                   "anisotropic_filtering = " << v.anisotropy << "   # 1 (desligado) | 2 | 4 | 8 | 16\n"
                   "shadow_softness = " << v.shadowSoftness << "   # 0-200 %, raio do filtro das sombras (sombras por stencil: nao ha resolucao de sombra)\n"
                   "hd_textures = " << (v.hdTextures ? "true" : "false") << "   # texturas HD das pastas hd_textures dos mods (tools\\hd_textures.py) (ao reiniciar)\n"
                   "texture_dump = " << (v.textureDump ? "true" : "false") << "   # grava as texturas do jogo em userdata\\texture_dump para o tools\\hd_textures.py (ao reiniciar)\n"
                   "hd_remaster = " << (v.hdRemaster ? "true" : "false") << "   # HD AI Remaster: texturas e modelos remasterizados por IA (data\\remaster_full ao lado do exe, ou so a fase 1 em data\\remaster_l1; README \"HD AI Remaster\") (reinicia o jogo)\n\n"
                   "[game]\n"
                   "language = \"" << v.language << "\"   # ";
            for (size_t i = 0; i < LANGUAGES.size(); i++)
                out << (i ? " | " : "") << LANGUAGES[i].id;
            out << " (reinicia o jogo; os da pasta languages\\ usam as vozes do idioma base)\n";
            out << "mojo_colors = \"" << (v.ps2MojoColors ? "ps2" : "xbox360")
                << "\"   # ps2 (mojo grande azul bem claro, extra grande roxo lilas) | xbox360 (todos azuis, como no original) (reinicia o jogo)\n";
            out << "subtitles = " << (v.subtitles ? "true" : "false") << "   # legendas nas cenas e falas (tools\\subtitles.py; README \"Legendas\")\n"
                << "subtitle_size = " << v.subtitleSize << "   # 50-200 %, tamanho da letra das legendas\n"
                << "subtitle_background = " << v.subtitleBackground << "   # 0-100 %, opacidade da caixa atras das legendas (0 = so contorno)\n"
                << "coop_mode = \"" << (v.coopLeapfrog ? "leapfrog" : "piggyback") << "\"   # leapfrog (pula-sela) | piggyback (cavalinho), vale na hora\n"
                << "invert_axis = " << (v.invertAxis ? "true" : "false") << "   # inverter eixo (a opcao do jogo), vale na hora\n"
                << "autosave = " << (v.autosave ? "true" : "false") << "   # salvamento automatico no slot escolhido; false = nunca\n"
                << "skip_cutscenes = " << (v.skipCutscenes ? "true" : "false") << "   # segurar B (ou ESC) pula cenas e videos, com aviso no canto; vale na hora\n\n";
            out << SerializeControls(v.controls);
            out << "\n[camera]\n"
                << "free_camera = " << (v.freeCamera ? "true" : "false") << "   # camera livre em terceira pessoa (experimental; README \"Camera livre\"), vale na hora\n"
                << "sensitivity = " << v.cameraSensitivity << "   # 25-300 %, velocidade do analogico direito / mouse\n"
                << "invert_y = " << (v.cameraInvertY ? "true" : "false") << "   # inverte o eixo vertical da camera\n"
                << "distance = " << v.cameraDistance << "   # 50-200 %, distancia da camera ao personagem\n";
            out << "\n[lighting]\n"
                << "ssao = \"" << AoQualityName(v.ssao) << "\"   # off | low (meia resolucao) | high (resolucao cheia): oclusao de ambiente em espaco de tela (README \"Iluminacao (remaster)\"), vale na hora\n"
                << "gi = " << (v.gi ? "true" : "false") << "   # luz indireta de um rebote em espaco de tela (a cor das superficies proximas), vale na hora\n"
                << "gi_intensity = " << v.giIntensity << "   # 0-200 %, intensidade da luz indireta\n"
                << "contact_shadows = " << (v.contactShadows ? "true" : "false") << "   # sombras de contato sob personagens, objetos e beiradas, vale na hora\n"
                << "soft_shadows = " << (v.softShadows ? "true" : "false") << "   # sombras do jogo com filtro mais largo e suave (disco girado), vale na hora\n";
            out << "\n[dev]\n"
                << "console = " << (v.devConsole ? "true" : "false") << "   # console de desenvolvedor (tecla antes do 1 ou F10; guia, \"Console de desenvolvedor\"), vale na hora\n"
                << "hulk_skin = " << (v.hulkSkin ? "true" : "false") << "   # a skin Crash_Hulk (nao usada no jogo original) no menu de skins; o console liga com \"unlock skin hulk\"\n";
            out << "\n[launcher]\n"
                << "view = \"" << (v.launcherAdvanced ? "advanced" : "simple") << "\"   # simple (as opcoes principais) | advanced (todas as abas): a ultima vista do CrashLauncher\n";
            return out.str();
        }
    }

    const Language* Languages(size_t& count)
    {
        count = LANGUAGES.size();
        return LANGUAGES.data();
    }

    bool AddLanguage(const Language& language)
    {
        if (std::any_of(LANGUAGES.begin(), LANGUAGES.end(), [&](const Language& l) { return Lower(language.id) == l.id; }))
            return false;
        LANGUAGES.push_back(language);
        return true;
    }

    const Language& LanguageById(const std::string& id)
    {
        for (const Language& language : LANGUAGES)
        {
            if (id == language.id)
                return language;
        }
        return LANGUAGES[0];
    }

    const Resolution* Resolutions(Aspect aspect, size_t& count)
    {
        const std::vector<Resolution>& list = ResolutionList(aspect);
        count = list.size();
        return list.data();
    }

    const Resolution& ResolutionOf(Aspect aspect, int height)
    {
        const std::vector<Resolution>& list = ResolutionList(aspect);
        const Resolution* best = &list[0];
        for (const Resolution& resolution : list)
        {
            if (std::abs(resolution.height - height) < std::abs(best->height - height))
                best = &resolution;
        }
        return *best;
    }

    void MonitorSize(int& width, int& height)
    {
        static int cachedWidth = 0, cachedHeight = 0;
        if (!cachedWidth)
        {
            const char* forced = getenv("CRASH_MONITOR");
            if (!forced || sscanf(forced, "%dx%d", &cachedWidth, &cachedHeight) != 2 || cachedWidth <= 0 || cachedHeight <= 0)
            {
                // The primary monitor's mode in physical pixels (not DPI scaled).
                DEVMODEW mode{};
                mode.dmSize = sizeof(mode);
                if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsWidth && mode.dmPelsHeight)
                    cachedWidth = int(mode.dmPelsWidth), cachedHeight = int(mode.dmPelsHeight);
                else
                    cachedWidth = 1920, cachedHeight = 1080;
            }
        }
        width = cachedWidth;
        height = cachedHeight;
    }

    Aspect EffectiveAspect(Aspect aspect)
    {
        if (aspect != Aspect::Auto)
            return aspect;
        int width, height;
        MonitorSize(width, height);
        float ratio = float(width) / float(height);
        // Between the aspects: 16:10 1.6, 16:9 1.78, 21:9 2.37-2.4, 32:9 3.56.
        return ratio < 1.69f ? Aspect::R16_10 : ratio < 2.05f ? Aspect::R16_9 : ratio < 2.9f ? Aspect::R21_9 : Aspect::R32_9;
    }

    const char* AspectName(Aspect aspect)
    {
        switch (aspect)
        {
        case Aspect::R16_9: return "16:9";
        case Aspect::R16_10: return "16:10";
        case Aspect::R21_9: return "21:9";
        case Aspect::R32_9: return "32:9";
        default: return "auto";
        }
    }

    float RenderScale()
    {
        return float(ResolutionOf(g_startup.aspect, g_startup.renderHeight).height) / 720.0f;
    }

    float RenderAspect()
    {
        const Resolution& resolution = ResolutionOf(g_startup.aspect, g_startup.renderHeight);
        return float(resolution.width) / float(resolution.height);
    }

    const char* WindowModeName(WindowMode mode)
    {
        switch (mode)
        {
        case WindowMode::Borderless: return "borderless";
        case WindowMode::Fullscreen: return "fullscreen";
        default: return "windowed";
        }
    }

    const char* OverlayModeName(OverlayMode mode)
    {
        switch (mode)
        {
        case OverlayMode::Fps: return "fps";
        case OverlayMode::FrameTime: return "frametime";
        case OverlayMode::Full: return "full";
        default: return "off";
        }
    }

    const char* AoQualityName(AoQuality quality)
    {
        switch (quality)
        {
        case AoQuality::Low: return "low";
        case AoQuality::High: return "high";
        default: return "off";
        }
    }

    bool ParseOverlayMode(const std::string& text, OverlayMode& mode)
    {
        std::string lower = Lower(Trim(text));
        if (lower == "off" || lower == "false" || lower == "0" || lower == "desligado")
            mode = OverlayMode::Off;
        else if (lower == "fps")
            mode = OverlayMode::Fps;
        else if (lower == "frametime" || lower == "frame_time")
            mode = OverlayMode::FrameTime;
        else if (lower == "full" || lower == "true" || lower == "1" || lower == "completo")
            mode = OverlayMode::Full;
        else
            return false;
        return true;
    }

    const char* AntiAliasingName(AntiAliasing mode)
    {
        switch (mode)
        {
        case AntiAliasing::Fxaa: return "fxaa";
        case AntiAliasing::Smaa: return "smaa";
        default: return "off";
        }
    }

    const char* UpscalerName(Upscaler upscaler)
    {
        switch (upscaler)
        {
        case Upscaler::Dlss: return "dlss";
        case Upscaler::Fsr: return "fsr";
        default: return "off";
        }
    }

    const char* UpscalerQualityName(UpscalerQuality quality)
    {
        switch (quality)
        {
        case UpscalerQuality::Quality: return "quality";
        case UpscalerQuality::Balanced: return "balanced";
        case UpscalerQuality::Performance: return "performance";
        case UpscalerQuality::UltraPerformance: return "ultra_performance";
        default: return "native";
        }
    }

    float UpscalerRatio(UpscalerQuality quality)
    {
        // The usual DLSS / FSR scale factors per axis: 1.5x, 1.7x, 2x, 3x.
        switch (quality)
        {
        case UpscalerQuality::Quality: return 1.0f / 1.5f;
        case UpscalerQuality::Balanced: return 1.0f / 1.7f;
        case UpscalerQuality::Performance: return 0.5f;
        case UpscalerQuality::UltraPerformance: return 1.0f / 3.0f;
        default: return 1.0f;
        }
    }

    float SceneScale()
    {
        if (g_startup.upscaler == Upscaler::Off)
            return RenderScale();
        return RenderScale() * UpscalerRatio(g_startup.upscalerQuality);
    }

    void Load(const std::filesystem::path& userDataDir)
    {
        std::lock_guard lock(g_mutex);
        g_path = userDataDir / "settings.toml";
        Values values;
        // Decided from the path, not the stream: after reading to EOF the stream is in a fail state.
        std::error_code existsError;
        bool exists = std::filesystem::exists(g_path, existsError) || existsError;
        std::ifstream in(g_path);
        if (exists && !in)
            Log("[settings] could not open %s, using defaults", g_path.string().c_str());
        if (in)
        {
            std::string text, section;
            int line = 0;
            while (std::getline(in, text))
            {
                line++;
                std::string trimmed = Trim(text);
                if (trimmed.empty() || trimmed[0] == '#')
                    continue;
                if (trimmed[0] == '[')
                {
                    section = Lower(Trim(trimmed.substr(1, trimmed.find(']') - 1)));
                    continue;
                }
                size_t equals = trimmed.find('=');
                if (equals == std::string::npos)
                {
                    Log("[settings] %s:%d: expected key = value", g_path.filename().string().c_str(), line);
                    continue;
                }
                bool isString;
                std::string value = ParseValue(trimmed.substr(equals + 1), isString);
                Apply(values, section, Lower(Trim(trimmed.substr(0, equals))), value, line);
            }
            Log("[settings] loaded %s", g_path.string().c_str());
        }
        if (!exists)
        {
            // Written before the CRASH_SETTINGS overrides so per-run values never reach the file.
            std::error_code error;
            std::filesystem::create_directories(userDataDir, error);
            std::ofstream out(g_path, std::ios::trunc);
            out << Serialize(values);
            Log("[settings] created %s", g_path.string().c_str());
        }
        // CRASH_SETTINGS="audio.music=50;video.antialiasing=fxaa": overrides for this run (tests).
        if (const char* overrides = getenv("CRASH_SETTINGS"))
        {
            std::istringstream list(overrides);
            std::string item;
            while (std::getline(list, item, ';'))
            {
                size_t dot = item.find('.'), equals = item.find('=');
                if (dot == std::string::npos || equals == std::string::npos || equals < dot)
                    continue;
                bool isString;
                std::string value = ParseValue(item.substr(equals + 1), isString);
                Apply(values, Lower(Trim(item.substr(0, dot))), Lower(Trim(item.substr(dot + 1, equals - dot - 1))), value, 0);
            }
            Log("[settings] CRASH_SETTINGS overrides: %s", overrides);
        }
        g_values = g_startup = values;
        Log("[settings] audio %d/%d/%d/%d, window %s, vsync %s, fps limit %d, resolution %s, overlay %s, aa %s, language %s, mojo colors %s", values.masterVolume,
            values.musicVolume, values.effectsVolume, values.voiceVolume, WindowModeName(values.window), values.vsync ? "on" : "off",
            values.fpsLimit, ResolutionOf(values.aspect, values.renderHeight).name,
            OverlayModeName(values.overlay), AntiAliasingName(values.antiAliasing), values.language.c_str(),
            values.ps2MojoColors ? "ps2" : "xbox360");
        Log("[settings] graphics: upscaler %s (%s), sharpness %d%%, motion blur %d%%, depth of field %d%%, anisotropic %dx, shadow softness %d%%, "
            "hd textures %s, texture dump %s, hd ai remaster %s",
            UpscalerName(values.upscaler), UpscalerQualityName(values.upscalerQuality), values.sharpness, values.motionBlur, values.depthOfField,
            values.anisotropy, values.shadowSoftness, values.hdTextures ? "on" : "off", values.textureDump ? "on" : "off",
            values.hdRemaster ? "on" : "off");
        Log("[settings] lighting: ssao %s, gi %s (%d%%), contact shadows %s, soft shadows %s", AoQualityName(values.ssao), values.gi ? "on" : "off",
            values.giIntensity, values.contactShadows ? "on" : "off", values.softShadows ? "on" : "off");
    }

    std::string ReadGameFolder(const std::filesystem::path& userDataDir)
    {
        std::ifstream in(userDataDir / "settings.toml");
        std::string text, section;
        while (std::getline(in, text))
        {
            std::string trimmed = Trim(text);
            if (trimmed.empty() || trimmed[0] == '#')
                continue;
            if (trimmed[0] == '[')
            {
                section = Lower(Trim(trimmed.substr(1, trimmed.find(']') - 1)));
                continue;
            }
            size_t equals = trimmed.find('=');
            if (section == "paths" && equals != std::string::npos && Lower(Trim(trimmed.substr(0, equals))) == "game")
            {
                bool isString;
                return ParseValue(trimmed.substr(equals + 1), isString);
            }
        }
        return {};
    }

    void Save()
    {
        std::string text;
        std::filesystem::path path;
        {
            std::lock_guard lock(g_mutex);
            text = Serialize(g_values);
            path = g_path;
        }
        if (path.empty())
            return;
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream out(temporary, std::ios::trunc | std::ios::binary);
            out << text;
            if (!out)
            {
                Log("[settings] could not write %s", temporary.string().c_str());
                return;
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error)
            Log("[settings] could not replace %s: %s", path.string().c_str(), error.message().c_str());
        else
            Log("[settings] saved %s", path.string().c_str());
    }

    Values Get()
    {
        std::lock_guard lock(g_mutex);
        return g_values;
    }

    void Set(const Values& values)
    {
        std::lock_guard lock(g_mutex);
        g_values = values;
    }

    const Values& AtStartup()
    {
        return g_startup;
    }

    bool NeedsRestart(const Values& v)
    {
        const Values& boot = g_startup;
        return v.language != boot.language || v.ps2MojoColors != boot.ps2MojoColors ||
               (v.hdRemaster != boot.hdRemaster && HdRemasterAvailable());
    }

    bool RendererDiffers(const Values& v)
    {
        const Values& now = g_startup;
        const Resolution& wanted = ResolutionOf(v.aspect, v.renderHeight);
        const Resolution& current = ResolutionOf(now.aspect, now.renderHeight);
        return wanted.width != current.width || wanted.height != current.height || v.upscaler != now.upscaler ||
               (v.upscaler != Upscaler::Off && v.upscalerQuality != now.upscalerQuality);
    }

    void RendererApplied(int renderHeight, Aspect aspect, Upscaler upscaler, UpscalerQuality quality)
    {
        g_startup.renderHeight = renderHeight;
        g_startup.aspect = aspect;
        g_startup.upscaler = upscaler;
        g_startup.upscalerQuality = quality;
    }

    float MasterVolume()
    {
        std::lock_guard lock(g_mutex);
        return float(g_values.masterVolume) / 100.0f;
    }

    const Language& StartupLanguage()
    {
        return LanguageById(g_startup.language);
    }
}
