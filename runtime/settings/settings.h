#pragma once

// Runtime settings: userdata\settings.toml, read at startup and written when the settings screen
// (settings_menu.h) closes. A missing file is created with the defaults; unknown keys and bad
// values are ignored (logged) and the defaults are used for them.
//
//   [audio]   master, music, effects, voice = 0..100 (%)
//   [video]   window = "windowed" | "borderless" | "fullscreen", vsync = true|false,
//             fps_limit = 30 | 60 | 120 | 0 (no limit), resolution = "1280x720" | "1600x900" |
//             "1920x1080" | "2560x1440" | "3840x2160" | the other aspects' (internal),
//             aspect = "auto" | "16:9" | "16:10" | "21:9" | "32:9", hud_16_9 = true|false
//             (gpu/widescreen.h),
//             overlay = "off" | "fps" | "frametime" | "full" (show_fps = true|false still read),
//             antialiasing = "off" | "fxaa" | "smaa", gpu_safe_mode = "off" | "on" | a list (gpu/rhi.h
//             SafeMode), gpu_diagnostics = true|false (on the next launch)
//   [graphics] upscaler = "off" | "dlss" | "fsr", upscaler_quality = "native" | "quality" |
//             "balanced" | "performance" | "ultra_performance", sharpness = 0..100 (%),
//             motion_blur = 0..100 (%), depth_of_field = 0..100 (%), anisotropic_filtering =
//             1 | 2 | 4 | 8 | 16, shadow_softness = 0..200 (%), hd_textures = true|false,
//             texture_dump = true|false, hd_remaster = true|false (hd_remaster.cpp)
//   [game]    language = "english" | "french" | "german" | "spanish" | "italian" | "dutch", or the
//             id of a language pack in languages\ (language_pack.h), e.g. "pt-br",
//             mojo_colors = "ps2" | "xbox360" (mojo_colors.cpp), subtitles = true|false,
//             subtitle_size = 50..200 (%), subtitle_background = 0..100 (%) (subtitles/subtitles.h),
//             coop_mode = "leapfrog" | "piggyback", invert_axis = true|false, autosave = true|false
//             (the game's own options, game_options.cpp), skip_cutscenes = true|false (hold to
//             skip a cutscene, skip_cinematics.h)
//   [controls] keyboard / mouse bindings and options (controls.h)
//   [lighting] the lighting remaster (gpu/lighting.h), all off by default: ssao = "off" | "low" |
//             "high", gi = true|false, gi_intensity = 0..200 (%), contact_shadows = true|false,
//             soft_shadows = true|false (the game's stencil shadows with a wider filter)
//   [multiplayer] color = "orange" | "red" | ... (the Crash's colour in a session, online/mp_colors.h)
//   [dev]     console = true|false (the developer console, dev/dev_console.h), hulk_skin =
//             true|false (the unused Crash_Hulk skin unlocked by the console, dev/dev_skins.cpp)
//   [launcher] view = "simple" | "advanced" (the launcher's last view; the game ignores it),
//             skip = true|false (Android: the app opens the game, not the launcher)
//
// Live: master volume, music/effects/voice volume (through the game's own volume categories,
// audio/game_volumes.h), window mode, V-Sync, frame limit, FPS overlay, anti-aliasing, sharpness,
// motion blur, depth of field, anisotropic filtering, shadow softness, subtitles, the lighting
// remaster. When the settings
// screen closes: the internal resolution and the upscaler and its quality (the renderer rebuilds
// its render targets at the new sizes, gpu::renderer::Reconfigure). With a restart, which the
// settings screen offers and does by itself (Restart, restart.cpp): mojo colours, language (the
// game picks its texts and voice archive at boot) and the HD AI Remaster mod (the mod list is
// fixed at boot). On the next launch
// only (no menu item): the HD texture packs and the texture dump.
//
// CRASH_SETTINGS="audio.music=50;game.language=french" overrides values for one run (tests); the
// settings screen saves whatever is current when it closes.

#include <cstdint>
#include <filesystem>
#include <string>

#include "controls.h"

namespace settings
{
    enum class WindowMode : uint8_t { Windowed, Borderless, Fullscreen };
    enum class AntiAliasing : uint8_t { Off, Fxaa, Smaa };
    // Temporal upscaling / anti-aliasing of the 3D scene (gpu/upscaler.h). With an upscaler on,
    // video.resolution is the output of the upscaler (the HUD and the post-processing are drawn
    // at it) and the 3D scene is drawn at that resolution times the quality's scale.
    enum class Upscaler : uint8_t { Off, Dlss, Fsr };
    enum class UpscalerQuality : uint8_t { Native, Quality, Balanced, Performance, UltraPerformance };
    // Performance overlay (gpu/overlay.h): off, FPS only, FPS + frame time (with the graph), full.
    enum class OverlayMode : uint8_t { Off, Fps, FrameTime, Full };
    // Screen-space ambient occlusion of the lighting remaster: off, half resolution, full resolution.
    enum class AoQuality : uint8_t { Off, Low, High };
    // How the renderer compiles pipelines (gpu/rhi.h rhi::ShaderCompilation): in the background
    // (async, the default), when a draw needs one (sync), or async and skipping the draws that can
    // wait (aggressive, settings.toml only).
    enum class ShaderCompilation : uint8_t { Async, Sync, Aggressive };

    struct Language
    {
        const char* id;        // value in settings.toml
        const char* name;      // shown in the settings screen
        uint32_t xconfig;      // XConfig user language (ExGetXConfigSetting 3/9)
        uint8_t country;       // XConfig country (3/14): Dutch is picked by country (NL)
        char letter;           // text bible letter the game ends up using
        const char* voiceRcf;  // voice archive the game mounts
        // Language packs (language_pack.h): the pack's folder, and two optional help lines for the
        // settings screen. The fields above are the base language's. nullptr for the game's own.
        const char* pack = nullptr;
        const char* help[2] = { nullptr, nullptr };
    };

    // The languages this copy of the game can show and speak (texts + voice archive present),
    // followed by the language packs found at startup.
    const Language* Languages(size_t& count);
    const Language& LanguageById(const std::string& id);
    // Adds a language pack (LoadLanguagePacks, before Load); false if the id is taken.
    bool AddLanguage(const Language& language);

    // Aspect ratio of the picture (video.aspect): the game is laid out for 16:9; wider screens
    // see more to the sides (Hor+, camera/hor_plus.cpp), 16:10 a little more above and below,
    // and the HUD stays in a 16:9 area (gpu/widescreen.h). Auto: the monitor's.
    enum class Aspect : uint8_t { Auto, R16_9, R16_10, R21_9, R32_9 };

    // Internal render resolutions: the game draws 1280x720, the renderer scales its render targets
    // and resolves by height / 720 (and, for another aspect than 16:9, wider or narrower; see
    // gpu/widescreen.h). The list depends on the aspect.
    struct Resolution
    {
        const char* name;  // "1920x1080"
        int width;
        int height;
    };
    // The resolutions of `aspect` (Auto: the monitor's, with its native resolution), by height.
    const Resolution* Resolutions(Aspect aspect, size_t& count);
    // The resolution of `aspect` with `height`, or the nearest height in its list.
    const Resolution& ResolutionOf(Aspect aspect, int height);
    // The aspect that Auto stands for (the monitor's, the closest of the others).
    Aspect EffectiveAspect(Aspect aspect);
    // The primary monitor's resolution (CRASH_MONITOR=WxH overrides it, for tests).
    void MonitorSize(int& width, int& height);
    // The internal resolution scale chosen at startup (1, 1.25, 1.5, 2 or 3), and its width / height.
    float RenderScale();
    float RenderAspect();

    struct Values
    {
        int masterVolume = 50;
        int musicVolume = 100;
        int effectsVolume = 100;
        int voiceVolume = 100;
        WindowMode window = WindowMode::Windowed;
        bool vsync = false;  // off = as before (Present without waiting for the vblank)
        int fpsLimit = 30;   // 30 (the original), 60, 120 or 0 = no limit (gpu::SetFrameLimit)
#if defined(__ANDROID__)
        int renderHeight = 540;  // a phone's GPU: 0.75x the game's lines (docs/internals/android_port.md)
#else
        int renderHeight = 720;  // internal resolution (Resolutions(aspect)), with `aspect`
#endif
        Aspect aspect = Aspect::Auto;
        bool hud16x9 = true;      // video.hud_16_9: the HUD and menus in the middle 16:9 area (else the HUD corners at the screen's edges)
        OverlayMode overlay = OverlayMode::Off;
        AntiAliasing antiAliasing = AntiAliasing::Off;
        // gpu_safe_mode: "off", "on" or a list of the renderer's conservative paths (gpu/rhi.h
        // SafeMode); gpu_diagnostics: the GPU's details in the log. Both on the next launch.
        std::string gpuSafeMode = "off";
        bool gpuDiagnostics = false;
        Upscaler upscaler = Upscaler::Off;
        UpscalerQuality upscalerQuality = UpscalerQuality::Native;
        int sharpness = 0;          // %: FSR's RCAS sharpening (DLSS 4 has none)
        int motionBlur = 100;       // % of the game's blur length (0 = no motion blur)
        int depthOfField = 100;     // % of the game's depth of field blur radius
        int anisotropy = 16;        // anisotropic filtering: 1 (off), 2, 4, 8, 16
        int shadowSoftness = 100;   // % of the radius of the game's shadow filter (0..200)
        ShaderCompilation shaderCompilation = ShaderCompilation::Async;  // applies at once
        bool hdTextures = true;     // use the HD texture packs of the mods (gpu/hd_textures.h), on the next launch
        bool textureDump = false;   // dump the game's textures to userdata\texture_dump (tools/hd_textures.py)
        bool hdRemaster = false;    // the AI-remastered mod in <data>\remaster_full or remaster_l1 (hd_remaster.cpp, paths.h), on the next launch
        std::string language = "english";
        bool ps2MojoColors = true;  // large mojo light blue, extra large purple, as on the PS2 (mojo_colors.cpp)
        bool subtitles = false;     // subtitles for the cutscenes and dialogue (subtitles/subtitles.h)
        int subtitleSize = 100;     // % of the default letter size
        int subtitleBackground = 50;  // % opacity of the box behind the text (0 = outline only)
        // The game's own options (its pause menu's OPTIONS screen, now covered by the settings
        // screen), kept here and applied to the game live, also over what a loaded save says
        // (GameOptionsTick, game_options.cpp).
        bool coopLeapfrog = true;  // co-op mode: LEAPFROG (the game's default) or PIGGYBACK
        bool invertAxis = false;   // INVERT AXIS: the camera's axis, as the game's option
        bool autosave = true;      // AUTOSAVE: off = never; on = when the game has a save slot
        // skip_cutscenes: "hold to skip" over the cutscenes and the intro videos, with a prompt in
        // a corner (skip_cinematics.h), live
        bool skipCutscenes = true;
        Controls controls;          // [controls]: keyboard and mouse bindings (controls.h, kernel/pc_input.cpp), live
        // [camera]: the free third-person camera (camera/free_camera.h), experimental, live
        bool freeCamera = false;      // free_camera: orbit camera around the player instead of the game's
        int cameraSensitivity = 100;  // sensitivity: % of the stick / mouse turn speed (25..300)
        bool cameraInvertY = false;   // invert_y
        int cameraDistance = 100;     // distance: % of the default distance (50..200)
        // [lighting]: the lighting remaster (gpu/lighting.h), live
        AoQuality ssao = AoQuality::Off;  // ssao: screen-space ambient occlusion
        bool gi = false;                  // gi: one-bounce indirect light (screen space)
        int giIntensity = 100;            // gi_intensity: % (0..200)
        bool contactShadows = false;      // contact_shadows: short shadows towards the game's key light
        bool softShadows = false;         // soft_shadows: wider rotated-disk filter on the game's stencil shadows
        // [multiplayer] color: the player's Crash colour in a session (online/mp_colors.h), an id
        // ("orange", "red", "pink", "purple", "blue", "cyan", "green", "yellow", "brown", "white",
        // "gray", "black"); chosen in the MULTIPLAYER screens, live
        std::string mpColour = "orange";
        // [dev]: developer tools (dev/dev_console.h), live
        bool devConsole = false;  // console: the developer console (the key left of 1, or F10)
        bool hulkSkin = false;    // hulk_skin: the unused Crash_Hulk skin is in the skin menu (console "unlock skin hulk")
        // [multiplayer] name: the player's name online (A-Z 0-9, 12 letters), typed on the MULTIPLAYER
        // screens (online/mp_menu.cpp); empty = CRASH_MP_NAME or the OS user's (mp_lobby.h LocalName).
        std::string mpName;
        // [paths] game: the game folder (UTF-8), when it is not next to the exe (paths.h); set by the
        // folder picker on the first run.
        std::string gamePath;
        // [launcher] view: the launcher opens on its last view, the few main options (simple) or
        // all the tabs (advanced); saved by the launcher as soon as it changes.
        bool launcherAdvanced = false;
        // [launcher] skip (Android): the app's icon opens the game straight away (LauncherActivity.java).
        bool launcherSkip = false;

        bool operator==(const Values&) const = default;
    };

    // Reads <userdata>\settings.toml (creates it with the defaults if missing). Call once, early.
    void Load(const std::filesystem::path& userDataDir);
    // Writes the current values to settings.toml.
    void Save();
    // [paths] game of <userDataDir>\settings.toml (UTF-8), empty if absent. Read before Load (paths.cpp).
    std::string ReadGameFolder(const std::filesystem::path& userDataDir);

    // Current values (thread-safe copy) and replacing them (the settings screen).
    Values Get();
    void Set(const Values& values);
    // The values in effect: the ones the game was started with, except for the renderer's
    // (resolution, upscaler and quality), which follow RendererApplied.
    const Values& AtStartup();
    // Game-side options of `values` that differ from the ones in effect: they need a restart.
    bool NeedsRestart(const Values& values);
    // The renderer's options of `values` differ from the ones in effect (resolution, upscaler, quality).
    bool RendererDiffers(const Values& values);
    // The renderer applied these resolution / aspect / upscaler / quality (render thread).
    void RendererApplied(int renderHeight, Aspect aspect, Upscaler upscaler, UpscalerQuality quality);

    // Restarting the game (restart.cpp), for the options it reads at boot. CRASH_RESTART:
    // "relaunch" (default: a new process with the same arguments, environment and folder, then
    // this one exits), "exit" (exit with RESTART_EXIT_CODE; tools/run_game.py starts the game
    // again and keeps tracking it) or "off" (no restart: those options apply on the next launch).
    enum class RestartMode : uint8_t { Relaunch, Exit, Off };
    constexpr int RESTART_EXIT_CODE = 4;
    RestartMode GetRestartMode();
    // main(), first thing: "--restarted <pid>" (given by Restart) waits for the previous process
    // to exit and marks this run as restarted (the intros are skipped: direct_boot.h).
    void HandleRestartArguments(int argc, char** argv);
    bool Restarted();
    // CRASH_MENU_AUTOPILOT (menu_autopilot.cpp): presses each menu's button as soon as its frontend
    // node is shown ("newgame": from the logos to the opening cutscene of episode 1).
    void MenuAutopilotNode(uint32_t node);
    uint16_t MenuAutopilotButtons();
    // Saves settings.toml and restarts. Returns only if starting the new process failed.
    void Restart();

    // Shortcuts read often.
    float MasterVolume();  // 0..1
    const Language& StartupLanguage();

    // Applies co-op mode, invert axis and autosave to the game (game_options.cpp). Game thread,
    // every read of the controller.
    void GameOptionsTick();

    // Registers the built-in archive patches of frontend_patches.cpp (after Load).
    void RegisterFrontendPatches();
    // Whether the PS2 options screen (where SETTINGS leads) was emptied for the settings screen,
    // leaving the frontend's background (frontend_patches.cpp); known once the game read it.
    bool SettingsScreenEmptied();
    // Registers the "PS2" mojo colour patches of mojo_colors.cpp when game.mojo_colors is "ps2".
    void RegisterMojoPatches();
    // The blue Snipe's shots: a blue copy of the Snipe projectile in its package and the hooks that
    // give it to a blue Snipe (snipe_colors.cpp). CRASH_SNIPE_COLORS=0 turns it off.
    void RegisterSnipePatches();

    // HD AI Remaster (hd_remaster.cpp): the AI-remastered mod (HD texture pack + package mod with
    // the subdivided models) of the whole game, <data>\remaster_full, or else the level 1 one,
    // <data>\remaster_l1 (paths::Data(): data\ next to the exe, private\ in a development build).
    // It is never copied to mods\: with graphics.hd_remaster on,
    // EnableHdRemaster (after vfs::Init and Load) adds its folder to the mod list, after the mods
    // of the mods folder. CRASH_HD_REMASTER_DIR=<folder> points to another copy (tests).
    const std::filesystem::path& HdRemasterDir();
    bool HdRemasterAvailable();  // the folder has the mod's rcf or hd_textures data
    bool HdRemasterWholeGame();  // the whole-game mod (not only level 1) is the one found
    void EnableHdRemaster();

    const char* WindowModeName(WindowMode mode);
    const char* AspectName(Aspect aspect);                        // "auto", "16:9", "16:10", "21:9", "32:9"
    const char* AntiAliasingName(AntiAliasing mode);
    const char* UpscalerName(Upscaler upscaler);                  // "off", "dlss", "fsr"
    const char* UpscalerQualityName(UpscalerQuality quality);     // "native", "quality", ...
    // Scene resolution / output resolution of an upscaler quality (1, 0.667, 0.588, 0.5, 0.333).
    float UpscalerRatio(UpscalerQuality quality);
    // Scale of the 3D scene relative to the game's 720p chosen at startup: RenderScale() times
    // the ratio of the upscaler quality (RenderScale() with the upscaler off).
    float SceneScale();
    const char* OverlayModeName(OverlayMode mode);              // "off", "fps", "frametime", "full"
    const char* AoQualityName(AoQuality quality);               // "off", "low", "high"
    const char* ShaderCompilationName(ShaderCompilation mode);  // "async", "sync", "aggressive"
    bool ParseOverlayMode(const std::string& text, OverlayMode& mode);  // also true/false, 1/0
}
