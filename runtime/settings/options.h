#pragma once

// The options of the SETTINGS screen (settings_menu.cpp) and of the launcher (launcher/launcher.cpp),
// defined once so that both show the same tabs, rows, names, values and help, in English and
// Brazilian Portuguese: the tabs and their rows (Tabs), the settings.toml key of each row (Key),
// its name (Label), its value as shown (Value), the two help lines under the rows (Help), the
// sliders' ranges (Percent) and what LEFT / RIGHT do to the values (Change). Each screen adds its
// own rows (the launcher's SETUP tab and quick LIGHTING row; the key bindings' capture) and what
// only it knows (Context: the upscalers and subtitles available, whether the game can restart).

#include "settings.h"

#include <string>
#include <utility>
#include <vector>

namespace settings::options
{
    enum class Item
    {
        Master, Music, Effects, Voice,
        Window, Resolution, FpsLimit, VSync, AntiAliasing, Overlay, Aspect, Hud16x9,
        Upscaler, UpscalerQuality, Sharpness, MotionBlur, DepthOfField, Anisotropy, ShadowSoftness, ShaderCompilation, HdRemaster,
        Language, MojoColors, CoopMode, InvertAxis, Autosave, SkipCutscenes, Storage,
        Subtitles, SubtitleSize, SubtitleBackground,
        FreeCamera, CameraSensitivity, CameraInvertY, CameraDistance,
        LightSsao, LightGi, LightGiIntensity, LightContactShadows, LightSoftShadows,
        Lighting,  // every [lighting] option at once (LightingPreset): the launcher's simple view
        Prompts, MouseCamera, MouseSensitivity, MouseInvertY, WalkSpeed, MouseMenus,
        Player1, Player2,
        DevConsole,
        GameFolder, ModsFolder, SavesFolder, ExtraData,  // the launcher's SETUP tab (launcher.cpp)
        HeaderSubtitles, HeaderCamera, HeaderLighting, HeaderDev, HeaderKeys,  // sub-group headings (not selectable)
        Bind0,  // + settings::Action: the key bindings (controls.h), one row per action
    };

    bool IsHeader(Item item);
    bool IsBinding(Item item);
    Action ActionOf(Item item);
    Item BindingItem(Action action);

    struct Text
    {
        const char* en;
        const char* pt;
    };

    // The tabs, switched with LB / RB; both screens open on the first. The `configuracoes*`,
    // `controles_tela` and `iluminacao_tela` profiles of config/run.toml rely on the order: GAME
    // (LANGUAGE, CO-OP, ...), AUDIO, VIDEO (RESOLUTION 2nd row, ANTI-ALIASING 5th, ASPECT 7th), GRAPHICS
    // (DLSS / FSR first), CONTROLS, DEFINITIVE EDITION (the port's own extras). DOWN skips the
    // sub-group headings.
    struct Tab
    {
        const char* id;  // in the log
        Text name;
        std::vector<Item> items;
    };
    constexpr int TAB_COUNT = 6;
    const Tab* Tabs();  // TAB_COUNT tabs

    // What the screen showing the options knows.
    struct Context
    {
        bool pt = false;               // Brazilian Portuguese (the language chosen, Portuguese())
        bool launcher = false;         // the launcher: everything applies when the game starts
        bool canRestart = true;        // the game restarts by itself for the options it reads at boot
        bool dlss = true, fsr = true;  // the upscalers can run here (DLSS: an NVIDIA RTX GPU with DLSS installed)
        bool haveSubtitles = true;     // there are subtitles for the chosen language
    };
    bool Portuguese(const Values& values);

    const char* Key(Item item);  // as in settings.toml ("graphics.upscaler"); also the name in the log
    std::string Label(Item item, bool pt);
    // Bindings: the keys ("Q / MOUSE 4"). Player 1 / 2: the controller by its number (the game
    // shows the connected ones by name).
    std::string Value(Item item, const Values& values, const Context& context);
    std::pair<std::string, std::string> Help(Item item, const Values& values, const Context& context);
    // Sliders: the value in `values`, its minimum, maximum and step; nullptr for the other rows.
    int* Percent(Item item, Values& values, int* minimum = nullptr, int* maximum = nullptr, int* step = nullptr);
    // LEFT (-1) / RIGHT (+1). Player 1 / 2 cycle AUTO and controllers 1-4 (the game lists the
    // connected ones itself). DLSS / FSR skips the upscalers `context` says can't run here. False
    // for the rows that are not values (headers, SETUP, bindings).
    bool Change(Item item, Values& values, int direction, const Context& context = Context());
    // The row's value can't run on this PC (DLSS without an NVIDIA RTX GPU, FSR on a GPU without
    // what it needs): the screens draw it greyed out ("DLSS (REQUIRES NVIDIA RTX)").
    bool Unavailable(Item item, const Values& values, const Context& context);
    // The game reads it at boot (the SETTINGS screen offers a restart; the launcher starts the game).
    bool RestartOnly(Item item);

    // The launcher's LIGHTING row: ORIGINAL turns every [lighting] option off; REMASTER turns them
    // all on at the recommended values (SSAO HIGH, indirect light 100%, contact and soft shadows);
    // any other mix, set one by one in DEFINITIVE EDITION, is CUSTOM.
    enum class LightingPreset : uint8_t { Original, Remaster, Custom };
    LightingPreset LightingPresetOf(const Values& values);
    void SetLightingPreset(Values& values, LightingPreset preset);
}
