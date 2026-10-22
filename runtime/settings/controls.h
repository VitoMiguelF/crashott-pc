#pragma once

// Keyboard and mouse bindings for gameplay ([controls] in settings.toml), read by
// kernel/pc_input.cpp, which turns them into the controller 0 the game reads.
//
// Each action has up to two keys. A key is a Windows virtual key code (VK_*), the mouse buttons
// included (VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1/2), or KEY_WHEEL_UP / KEY_WHEEL_DOWN.
// In settings.toml each action is a string of key names separated by commas:
//
//   [controls]
//   jump = "SPACE"
//   light_attack = "MOUSE1, J"
//   mouse_sensitivity = 100     # %, mouse -> camera
//   prompts = "auto"            # auto | xbox | playstation | keyboard (button icons)
//   player1 = "auto"            # local co-op: the controller of each player (kernel/pad_slots.h):
//   player2 = "auto"            # auto | xbox1..xbox4 (XInput) | playstation1..playstation4
//
// The game's own controls (scripts\mixins\inputmap_methods.lua, Crash.bfig/Crashlua.blua; see
// runtime/NOTES.md "Controles de PC"): A jump, X light attack, Y heavy attack, B action/jack/
// counter, RT block (+ stick: dodge; in the air: slide), RB finishing move, LB modifier (LB+Y
// cheer...), and the left stick rotated (7 of its 8 octants within 0.75 s) for the spin moves.

#include <array>
#include <cstdint>
#include <string>

namespace settings
{
    enum class Action : uint8_t
    {
        MoveForward, MoveBack, MoveLeft, MoveRight,
        Walk,          // left stick at walk_speed % instead of full
        Jump,          // A
        LightAttack,   // X
        HeavyAttack,   // Y
        Interact,      // B: action, jack, counter
        Block,         // RT: block, dodge (with a direction), slide (in the air)
        RotateStick,   // the left stick turns in circles while held (spin combos with an attack)
        SpinAttack,    // rotate + light attack repeated: Crash's spin
        Finisher,      // RB
        Modifier,      // LB
        LeftTrigger,   // LT
        CameraUp, CameraDown, CameraLeft, CameraRight,  // right stick
        Pause,         // START
        Map,           // BACK
        Count
    };
    constexpr size_t ACTION_COUNT = size_t(Action::Count);
    constexpr size_t KEYS_PER_ACTION = 2;

    // Pseudo key codes for the mouse wheel (unassigned virtual key codes).
    constexpr uint16_t KEY_WHEEL_UP = 0x0E, KEY_WHEEL_DOWN = 0x0F;

    // Which button icons the menus and the HUD show.
    enum class PromptStyle : uint8_t { Auto, Xbox, PlayStation, Keyboard };

    struct Controls
    {
        std::array<std::array<uint16_t, KEYS_PER_ACTION>, ACTION_COUNT> keys{};  // 0 = none
        int mouseSensitivity = 100;  // %: mouse -> right stick (camera)
        bool mouseCamera = true;     // the mouse turns the camera in gameplay
        bool invertY = false;        // mouse / camera keys up = down
        int walkSpeed = 40;          // % of the left stick while Walk is held
        PromptStyle prompts = PromptStyle::Auto;
        bool mouseMenus = true;      // the frontend menus follow the mouse
        // Local co-op: the controller of player 1 / player 2 (PlayerDeviceKey; "auto" = connection
        // order, and START on the title screen picks player 1). A single pad is always player 1.
        std::array<std::string, 2> players{ "auto", "auto" };

        Controls();  // the defaults
        bool operator==(const Controls&) const = default;
    };

    const char* ActionId(Action action);                    // settings.toml key: "light_attack"
    const char* ActionLabel(Action action, bool portuguese);  // the settings screen: "LIGHT ATTACK"
    std::string KeyName(uint16_t key);                      // "W", "SPACE", "MOUSE1"; "" for 0
    bool ParseKey(const std::string& name, uint16_t& key);  // case-insensitive
    const char* PromptStyleName(PromptStyle style);         // "auto", "xbox", "playstation", "keyboard"
    // [controls] player1 / player2 values: "auto", "xbox1".."xbox4", "playstation1".."playstation4".
    bool ValidPlayerDevice(const std::string& key);

    // A [controls] line: false when `key` is not a controls setting; `valid` false for a bad value.
    bool ApplyControl(Controls& controls, const std::string& key, const std::string& value, bool& valid);
    // The [controls] section of settings.toml.
    std::string SerializeControls(const Controls& controls);
}
