// Keyboard and mouse as controller 0 (pc_input.h).

#include "pc_input.h"
#include "input.h"
#include "hid_pads.h"
#include "../diagnostics.h"
#include "../fs/rcf_overlay.h"
#include "../settings/settings.h"
#include "../settings/menu_mouse.h"
#include "../settings/settings_menu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <cstring>

namespace input::pc
{
    namespace
    {
        // Key state, written by the window thread. Bit 0: held. Bit 1: went down since the game's
        // last read (a press shorter than a game tick still counts once).
        std::array<std::atomic<uint8_t>, 256> g_keys{};
        std::atomic<int> g_wheelUp{ 0 }, g_wheelDown{ 0 };
        std::atomic<bool> g_focused{ false };
        std::atomic<int> g_rawX{ 0 }, g_rawY{ 0 };          // for the right stick
        std::atomic<int> g_lookX{ 0 }, g_lookY{ 0 };        // for MouseLookDelta
        std::atomic<bool> g_lookTaken{ false };
        std::atomic<ULONGLONG> g_lastMouseMove{ 0 };
        // The cursor (client pixels) and the window's client size; mouse events for the menus.
        std::atomic<int> g_cursorX{ -1 }, g_cursorY{ -1 }, g_clientW{ 0 }, g_clientH{ 0 };
        std::atomic<uint32_t> g_cursorMoves{ 0 }, g_leftClicks{ 0 }, g_rightClicks{ 0 };
        std::atomic<bool> g_leftHeld{ false };
        std::atomic<bool> g_menuClick{ false };       // a left click the game's menus have not used (TakeMenuClick)
        std::atomic<ULONGLONG> g_menuClickTime{ 0 };
        std::atomic<int> g_pressAccept{ 0 };          // game reads left with A down (PressMenuAccept)
        std::atomic<int> g_menuWheel{ 0 }, g_settingsWheel{ 0 };  // notches not yet used (+ = up)
        std::atomic<uint32_t> g_settingsRight{ 0 };                // right clicks on the settings screen
        // A test script moves the mouse (ScriptMouse): the real one is ignored over the window.
        std::atomic<bool> g_scriptedMouse{ false };
        // Key capture for the settings screen (BeginKeyCapture): 1 = waiting, the key in the high bits.
        std::atomic<uint32_t> g_capture{ 0 };
        // Typing (SetTextEntry): the characters typed and not yet taken.
        std::atomic<bool> g_textEntry{ false };
        std::mutex g_typedMutex;
        std::u32string g_typed;

        // The character a key types in text entry, or 0.
        char32_t TypedChar(uint32_t key)
        {
            if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
                return char32_t(key);
            if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
                return char32_t('0' + (key - VK_NUMPAD0));
            if (key == VK_RETURN)
                return U'\r';
            return 0;
        }

        void Type(uint32_t key)
        {
            char32_t c = g_textEntry ? TypedChar(key) : 0;
            if (!c)
                return;
            std::lock_guard lock(g_typedMutex);
            if (g_typed.size() < 64)
                g_typed += c;
        }

        // Frontend: the node shown and when (game thread), for InMenus.
        std::atomic<uint32_t> g_frontendNode{ 0xFFFFFFFF };
        std::atomic<ULONGLONG> g_frontendTime{ 0 };
        // Frontend.bfig nodes that are gameplay, not menus: the "game running" node (target of
        // ExitGameRunning) and the cinematics that go back to it (owners of ExitPlayerSkips,
        // ExitSequencerDone, ExitArenaCinematic, ExitWhenDone1). Found by FindGameplayNodes.
        std::mutex g_nodesMutex;
        std::vector<uint32_t> g_gameplayNodes;
        // The pause menu (target of ExitPause): Esc resumes there (START), as it paused.
        std::vector<uint32_t> g_pauseNodes;

        // Exit records: [0xBC27359D][u32 length][name, NUL, padded to 4][u32 owner node][u32 x2]
        // [f32 -1][u32][f32 -1][u32 target node]. Read-only.
        bool FindGameplayNodes(std::vector<uint8_t>& data)
        {
            auto be32 = [&](size_t at) {
                return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) | (uint32_t(data[at + 2]) << 8) | data[at + 3];
            };
            struct Exit
            {
                std::string name;
                uint32_t owner, target;
            };
            std::vector<Exit> exits;
            for (size_t at = 0; at + 8 <= data.size(); at += 4)
            {
                if (be32(at) != 0xBC27359D)
                    continue;
                uint32_t length = be32(at + 4);
                size_t fields = (at + 8 + length + 1 + 3) & ~size_t(3);
                if (length > 200 || fields + 28 > data.size())
                    continue;
                std::string name(reinterpret_cast<const char*>(&data[at + 8]), strnlen(reinterpret_cast<const char*>(&data[at + 8]), length));
                exits.push_back({ name, be32(fields), be32(fields + 24) });
            }
            std::vector<uint32_t> nodes;
            for (const Exit& exit : exits)
            {
                if (exit.name == "ExitGameRunning" && std::find(nodes.begin(), nodes.end(), exit.target) == nodes.end())
                    nodes.push_back(exit.target);
            }
            size_t running = nodes.size();
            for (const Exit& exit : exits)
            {
                bool cinematic = exit.name == "ExitPlayerSkips" || exit.name == "ExitSequencerDone" || exit.name == "ExitArenaCinematic" ||
                                 exit.name == "ExitWhenDone1";
                if (cinematic && std::find(nodes.begin(), nodes.begin() + running, exit.target) != nodes.begin() + running &&
                    std::find(nodes.begin(), nodes.end(), exit.owner) == nodes.end())
                    nodes.push_back(exit.owner);
            }
            std::vector<uint32_t> pause;
            for (const Exit& exit : exits)
            {
                if (exit.name == "ExitPause" && std::find(pause.begin(), pause.end(), exit.target) == pause.end())
                    pause.push_back(exit.target);
            }
            Log("[input] Frontend.bfig: %zu gameplay node(s) (game running: %u, %zu cinematics), pause menu %u; the other screens are menus",
                nodes.size(), running ? nodes[0] : 0u, nodes.size() - running, pause.empty() ? 0u : pause[0]);
            std::lock_guard lock(g_nodesMutex);
            g_gameplayNodes = std::move(nodes);
            g_pauseNodes = std::move(pause);
            return false;  // nothing changed
        }

        std::atomic<uint8_t> g_device{ uint8_t(Device::Xbox) };
        std::atomic<uint32_t> g_promptGeneration{ 0 };

        constexpr uint8_t HELD = 1, PRESSED = 2;

        settings::Controls Controls()
        {
            // settings::Get copies every value; the bindings change only from the settings screen,
            // so a copy per game read (30 Hz) is cheap enough.
            return settings::Get().controls;
        }

        // Held now, or pressed and released since the game's last read.
        bool Down(uint16_t key, const std::vector<uint16_t>& scriptKeys)
        {
            if (!key)
                return false;
            if (std::find(scriptKeys.begin(), scriptKeys.end(), key) != scriptKeys.end())
                return true;
            if (key == settings::KEY_WHEEL_UP)
                return g_wheelUp.load() > 0;
            if (key == settings::KEY_WHEEL_DOWN)
                return g_wheelDown.load() > 0;
            return (g_keys[key & 0xFF].load() & (HELD | PRESSED)) != 0;
        }

        bool ActionDown(const settings::Controls& controls, settings::Action action, const std::vector<uint16_t>& scriptKeys)
        {
            for (uint16_t key : controls.keys[size_t(action)])
            {
                if (Down(key, scriptKeys))
                    return true;
            }
            return false;
        }

        bool AnyKeyHeld()
        {
            for (const auto& key : g_keys)
            {
                if (key.load())
                    return true;
            }
            return false;
        }

        bool OnPauseMenu()
        {
            uint32_t node = g_frontendNode.load();
            std::lock_guard lock(g_nodesMutex);
            return std::find(g_pauseNodes.begin(), g_pauseNodes.end(), node) != g_pauseNodes.end();
        }

        // Menu keys (fixed): arrows / WASD = D-pad, Enter / Space = A, Esc = B (START on the pause
        // menu, which it opened), Backspace / Delete
        // = X (the frontend's "X BACK" in the name entry, "X ERASE"), Tab = Y ("Y BACK", "Y NO",
        // "Y DONE"), P = START ("DONE START"), Q / E = LB / RB ("CONTINUE STORY RB").
        WORD MenuButtons(const std::vector<uint16_t>& scriptKeys)
        {
            WORD buttons = 0;
            auto key = [&](uint16_t k) { return Down(k, scriptKeys); };
            // Typing a name: the letters and Enter are text (SetTextEntry), not menu keys.
            bool letters = !g_textEntry;
            if (key(VK_UP) || (letters && key('W'))) buttons |= XINPUT_GAMEPAD_DPAD_UP;
            if (key(VK_DOWN) || (letters && key('S'))) buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
            if (key(VK_LEFT) || (letters && key('A'))) buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
            if (key(VK_RIGHT) || (letters && key('D'))) buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            if ((letters && key(VK_RETURN)) || key(VK_SPACE)) buttons |= XINPUT_GAMEPAD_A;
            if (key(VK_ESCAPE)) buttons |= OnPauseMenu() ? XINPUT_GAMEPAD_START : XINPUT_GAMEPAD_B;
            if (key(VK_BACK) || key(VK_DELETE)) buttons |= XINPUT_GAMEPAD_X;
            if (key(VK_TAB)) buttons |= XINPUT_GAMEPAD_Y;
            if (letters && key('P')) buttons |= XINPUT_GAMEPAD_START;
            if (letters && key('Q')) buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
            if (letters && key('E')) buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
            return buttons;
        }

        SHORT Axis(float value)
        {
            return SHORT(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
        }

        // The left stick going round (RotateStick / SpinAttack): one step per game read.
        struct Rotation
        {
            bool active = false;
            float angle = 0.0f;  // radians, 0 = right, pi/2 = forward
            int reads = 0;       // game reads since it started
        };
        Rotation g_rotation;
        constexpr float ROTATION_STEP = 6.2831853f / 9.0f;  // a turn every 9 reads (0.3 s at 30 Hz)
        // SpinAttack taps X once the first turn is complete, then 2 reads down, 2 up.
        constexpr int SPIN_FIRST_PRESS = 9, SPIN_PERIOD = 4;

        void ClearPresses()
        {
            for (auto& key : g_keys)
                key.fetch_and(uint8_t(~PRESSED));
            if (g_wheelUp.load() > 0)
                g_wheelUp--;
            if (g_wheelDown.load() > 0)
                g_wheelDown--;
        }
    }

    // ---- window thread ----

    void OnKey(uint32_t key, bool down)
    {
        if (key >= 256)
            return;
        if (g_scriptedMouse && (key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2))
            return;
        // Being captured for the settings screen: kept for it, not seen as a press.
        if (down && g_capture.load() == 1)
        {
            g_capture = key << 8 | 2;
            return;
        }
        if (down && !(g_keys[key].load() & HELD))
            Type(key);  // not the auto-repeat of a held key
        if (down)
            g_keys[key].store(HELD | PRESSED);
        else
            g_keys[key].fetch_and(uint8_t(~HELD));
        if (key == VK_LBUTTON)
        {
            g_leftHeld = down;
            if (down)
            {
                g_leftClicks++;
                g_menuClickTime = GetTickCount64();
                g_menuClick = true;
            }
        }
        if (down && key == VK_RBUTTON)
        {
            g_rightClicks++;
            if (settings::menu::IsOpen())
                g_settingsRight++;
        }
        if (down)
            NoteDevice(Device::Keyboard);
    }

    void OnWheel(int notches)
    {
        // Each notch counts as a key press lasting two game reads.
        if (notches > 0)
            g_wheelUp += 2 * notches;
        else if (notches < 0)
            g_wheelDown += -2 * notches;
        if (settings::menu::IsOpen())
            g_settingsWheel += notches;
        else
            g_menuWheel += notches;
        NoteDevice(Device::Keyboard);
    }

    namespace
    {

        void SetCursor(int x, int y, int clientWidth, int clientHeight)
        {
            if (x != g_cursorX.load() || y != g_cursorY.load())
            {
                g_cursorMoves++;
                g_lastMouseMove = GetTickCount64();
            }
            g_cursorX = x, g_cursorY = y, g_clientW = clientWidth, g_clientH = clientHeight;
        }
    }

    void OnCursor(int x, int y, int clientWidth, int clientHeight)
    {
        if (g_scriptedMouse)
        {
            g_clientW = clientWidth, g_clientH = clientHeight;
            return;
        }
        if (x != g_cursorX.load() || y != g_cursorY.load())
        {
            g_cursorMoves++;
            g_lastMouseMove = GetTickCount64();
        }
        g_cursorX = x, g_cursorY = y, g_clientW = clientWidth, g_clientH = clientHeight;
    }

    void OnRawMouse(int dx, int dy)
    {
        g_rawX += dx;
        g_rawY += dy;
        g_lookX += dx;
        g_lookY += dy;
        if (dx || dy)
            g_lastMouseMove = GetTickCount64();
    }

    void OnFocus(bool focused)
    {
        g_focused = focused;
        if (!focused)
        {
            for (auto& key : g_keys)
                key.store(0);
            g_wheelUp = g_wheelDown = 0;
            if (!g_scriptedMouse)
                g_leftHeld = false;
        }
    }

    CursorMode WantedCursor()
    {
        if (!g_focused)
            return CursorMode::Visible;
        if (!InMenus() && !settings::menu::IsOpen())
        {
            settings::Controls controls = Controls();
            return controls.mouseCamera ? CursorMode::Captured : CursorMode::Hidden;
        }
        // Menus and the settings screen: visible, hidden after 3 s without moving (it comes back
        // with the first motion).
        return GetTickCount64() - g_lastMouseMove.load() > 3000 ? CursorMode::Hidden : CursorMode::Visible;
    }

    // ---- game thread ----

    bool InMenus()
    {
        // The Frontend tree's conditions run every frame while one of its screens is up; in
        // gameplay they stop (or sit on the in-game node, which is not a menu).
        ULONGLONG last = g_frontendTime.load();
        if (!last || GetTickCount64() - last > 250)
            return false;
        uint32_t node = g_frontendNode.load();
        std::lock_guard lock(g_nodesMutex);
        return std::find(g_gameplayNodes.begin(), g_gameplayNodes.end(), node) == g_gameplayNodes.end();
    }

    void Init()
    {
        rcf_overlay::RegisterBuiltinPatch("default", "fighttrees\\frontend.bfig", "nos de gameplay x menus para teclado/mouse (so leitura)",
                                          FindGameplayNodes);
        menu_mouse::Init();
        hid::Start();
    }

    void BeginKeyCapture()
    {
        g_capture = 1;
    }

    void CancelKeyCapture()
    {
        g_capture = 0;
    }

    bool CapturedKey(uint16_t& key)
    {
        uint32_t value = g_capture.load();
        if ((value & 0xFF) != 2 || !g_capture.compare_exchange_strong(value, 0))
            return false;
        key = uint16_t(value >> 8);
        return true;
    }

    void ScriptKeyDown(uint16_t key)
    {
        uint32_t waiting = 1;
        if (!g_capture.compare_exchange_strong(waiting, uint32_t(key) << 8 | 2))
            Type(key);
    }

    void SetTextEntry(bool on)
    {
        if (g_textEntry.exchange(on) == on)
            return;
        std::lock_guard lock(g_typedMutex);
        g_typed.clear();
    }

    bool TextEntry()
    {
        return g_textEntry;
    }

    std::u32string TakeTypedText()
    {
        std::lock_guard lock(g_typedMutex);
        std::u32string typed;
        typed.swap(g_typed);
        return typed;
    }

    void UseScriptedMouse()
    {
        g_scriptedMouse = true;
    }

    void ScriptMouse(float x, float y, int click)
    {
        // 1280x720 game image -> client pixels (the image is 16:9, centred).
        int w = g_clientW, h = g_clientH;
        if (w <= 0 || h <= 0)
            w = 1280, h = 720;
        float iw = float(w), ih = float(h);
        if (iw > ih * 16.0f / 9.0f)
            iw = ih * 16.0f / 9.0f;
        else
            ih = iw * 9.0f / 16.0f;
        g_scriptedMouse = true;
        if (click == 5)
        {
            OnWheel(int(std::lround(x)));
            return;
        }
        SetCursor(int((w - iw) * 0.5f + x / 1280.0f * iw), int((h - ih) * 0.5f + y / 720.0f * ih), w, h);
        if (click == 1 || click == 3)
        {
            g_leftClicks++;
            g_menuClickTime = GetTickCount64();
            g_menuClick = true;
            g_leftHeld = click == 3;
            NoteDevice(Device::Keyboard);
        }
        else if (click == 4)
            g_leftHeld = false;
        else if (click == 2)
        {
            g_rightClicks++;
            if (settings::menu::IsOpen())
                g_settingsRight++;
        }
    }

    bool CursorOnImage(float& x, float& y)
    {
        // The game's image is 16:9, fitted into the window and centred (gpu/d3d12_renderer.cpp,
        // present: the front buffer's aspect), with black bars around it.
        int cx = g_cursorX, cy = g_cursorY, w = g_clientW, h = g_clientH;
        if (cx < 0 || w <= 0 || h <= 0)
            return false;
        float iw = float(w), ih = float(h);
        if (iw > ih * 16.0f / 9.0f)
            iw = ih * 16.0f / 9.0f;
        else
            ih = iw * 9.0f / 16.0f;
        float u = (cx + 0.5f - (w - iw) * 0.5f) / iw, v = (cy + 0.5f - (h - ih) * 0.5f) / ih;
        if (u < 0 || u > 1 || v < 0 || v > 1)
            return false;
        x = u * 1280.0f, y = v * 720.0f;
        return true;
    }

    bool CursorInClient(float& u, float& v)
    {
        int cx = g_cursorX, cy = g_cursorY, w = g_clientW, h = g_clientH;
        if (cx < 0 || w <= 0 || h <= 0)
            return false;
        u = (cx + 0.5f) / float(w), v = (cy + 0.5f) / float(h);
        return true;
    }

    uint32_t CursorMoves()
    {
        return g_cursorMoves.load();
    }

    uint32_t LeftClicks()
    {
        return g_leftClicks.load();
    }

    bool LeftHeld()
    {
        return g_leftHeld.load();
    }

    bool TakeMenuClick()
    {
        if (!g_menuClick.exchange(false))
            return false;
        return GetTickCount64() - g_menuClickTime.load() <= 400;
    }

    int TakeMenuWheel()
    {
        return g_menuWheel.exchange(0);
    }

    int TakeSettingsWheel()
    {
        return g_settingsWheel.exchange(0);
    }

    void PressMenuAccept()
    {
        g_pressAccept = 2;
    }

    void NoteFrontendNode(uint32_t node)
    {
        g_frontendNode = node;
        g_frontendTime = GetTickCount64();
    }

    namespace
    {
        // The frontend's screens with the mouse, besides what settings/menu_mouse.cpp does on a
        // menu (hover selects the item through the game's own SetSelection, a click selects and
        // accepts it, the wheel steps through the items): the right button goes back (B), a left
        // click on a screen without a menu (PRESS START, "Load successful") is A, and the A a click
        // on an item asked for (PressMenuAccept). Game reads only (`poll`); presses last a few reads.
        WORD MouseMenuButtons(bool poll)
        {
            static WORD s_out = 0;
            if (!poll)
                return s_out;
            static uint32_t s_right = g_rightClicks.load();
            static int s_holdA = 0, s_holdB = 0;
            s_out = 0;
            uint32_t right = g_rightClicks.load();
            bool backed = right != s_right;
            s_right = right;
            if (!settings::Get().controls.mouseMenus)
            {
                g_pressAccept = 0;
                return 0;
            }
            menu_mouse::Menu menu;
            // (Not on the name entry's keyboard, which takes its clicks itself: save_screens.cpp.)
            if (!g_textEntry && !menu_mouse::Live(menu) && TakeMenuClick())
            {
                s_holdA = 2;
                NoteDevice(Device::Keyboard);
            }
            if (backed)
            {
                s_holdB = 3;
                NoteDevice(Device::Keyboard);
            }
            int accept = g_pressAccept.load();
            if (accept > 0)
            {
                g_pressAccept = accept - 1;
                s_out = XINPUT_GAMEPAD_A;
            }
            else if (s_holdB > 0)
            {
                s_holdB--;
                s_out = XINPUT_GAMEPAD_B;
            }
            else if (s_holdA > 0)
            {
                s_holdA--;
                s_out = XINPUT_GAMEPAD_A;
            }
            return s_out;
        }
    }

    uint16_t MenuKeys(const std::vector<uint16_t>& scriptKeys)
    {
        WORD buttons = MenuButtons(scriptKeys);
        // The runtime's settings screen: the right button goes back (the wheel and the left button
        // are read by the screen itself).
        static uint32_t s_right = g_settingsRight.load();
        uint32_t right = g_settingsRight.load();
        if (right != s_right)
        {
            s_right = right;
            buttons |= XINPUT_GAMEPAD_B;
        }
        return buttons;
    }

    void Apply(XINPUT_STATE& state, bool poll, const std::vector<uint16_t>& scriptKeys)
    {
        XINPUT_GAMEPAD& pad = state.Gamepad;
        settings::Controls controls = Controls();
        bool menus = InMenus();
        using settings::Action;
        auto action = [&](Action a) { return ActionDown(controls, a, scriptKeys); };

        if (menus)
        {
            pad.wButtons |= MenuButtons(scriptKeys);
            if (!settings::menu::IsOpen())
                pad.wButtons |= MouseMenuButtons(poll);
            g_rotation.active = false;
        }
        else
        {
            WORD buttons = 0;
            if (action(Action::Jump)) buttons |= XINPUT_GAMEPAD_A;
            if (action(Action::LightAttack)) buttons |= XINPUT_GAMEPAD_X;
            if (action(Action::HeavyAttack)) buttons |= XINPUT_GAMEPAD_Y;
            if (action(Action::Interact)) buttons |= XINPUT_GAMEPAD_B;
            if (action(Action::Finisher)) buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
            if (action(Action::Modifier)) buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
            if (action(Action::Pause)) buttons |= XINPUT_GAMEPAD_START;
            if (action(Action::Map)) buttons |= XINPUT_GAMEPAD_BACK;
            if (action(Action::Block)) pad.bRightTrigger = 255;
            if (action(Action::LeftTrigger)) pad.bLeftTrigger = 255;

            // Left stick: WASD, normalised so diagonals are not faster; Walk scales it.
            float x = float(action(Action::MoveRight)) - float(action(Action::MoveLeft));
            float y = float(action(Action::MoveForward)) - float(action(Action::MoveBack));
            float length = std::sqrt(x * x + y * y);
            if (length > 0.0f)
            {
                bool walk = action(Action::Walk);
                float scale = (walk ? controls.walkSpeed / 100.0f : 1.0f) / length;
                x *= scale, y *= scale;
                static bool s_walking = false;
                if (walk != s_walking && poll)
                {
                    s_walking = walk;
                    Log("[input] keyboard: %s (left stick at %d%%)", walk ? "walking" : "running", walk ? controls.walkSpeed : 100);
                }
            }

            bool spin = action(Action::SpinAttack);
            if (spin || action(Action::RotateStick))
            {
                if (!g_rotation.active)
                {
                    g_rotation = { true, length > 0.0f ? std::atan2(y, x) : 1.5707963f, 0 };
                    Log("[input] keyboard: rotating the left stick%s", spin ? " (spin attack)" : "");
                }
                else if (poll)
                {
                    g_rotation.angle += ROTATION_STEP;
                    g_rotation.reads++;
                }
                // Full deflection whatever Walk says: the detector ignores the stick below 60 %.
                x = std::cos(g_rotation.angle), y = std::sin(g_rotation.angle);
                if (spin && g_rotation.reads >= SPIN_FIRST_PRESS && (g_rotation.reads - SPIN_FIRST_PRESS) % SPIN_PERIOD < 2)
                    buttons |= XINPUT_GAMEPAD_X;
            }
            else
                g_rotation.active = false;

            if (x != 0.0f || y != 0.0f)
                pad.sThumbLX = Axis(x), pad.sThumbLY = Axis(y);

            // Right stick: camera keys, plus the mouse while it is captured.
            float cx = float(action(Action::CameraRight)) - float(action(Action::CameraLeft));
            float cy = float(action(Action::CameraUp)) - float(action(Action::CameraDown));
            if (poll && controls.mouseCamera && !g_lookTaken && g_focused)
            {
                // ~15 counts per game read (450 counts/s) = full deflection at 100 %.
                float k = controls.mouseSensitivity / 100.0f / 15.0f;
                cx += g_rawX.exchange(0) * k;
                cy -= g_rawY.exchange(0) * k;
            }
            else if (poll)
                g_rawX = g_rawY = 0;
            if (controls.invertY)
                cy = -cy;
            if (cx != 0.0f || cy != 0.0f)
                pad.sThumbRX = Axis(cx), pad.sThumbRY = Axis(cy);

            pad.wButtons |= buttons;
        }
        if (poll)
            ClearPresses();
        // Scripted keys (CRASH_INPUT_SCRIPT KEY_*) stand for the keyboard, for the icons too.
        if (poll && !scriptKeys.empty())
            NoteDevice(Device::Keyboard);
    }

    // ---- devices ----

    void NoteDevice(Device device)
    {
        uint8_t previous = g_device.exchange(uint8_t(device));
        if (previous != uint8_t(device))
        {
            Log("[input] last device used: %s", DeviceName(device));
            if (settings::Get().controls.prompts == settings::PromptStyle::Auto)
                g_promptGeneration++;
        }
    }

    Device PromptDevice()
    {
        switch (settings::Get().controls.prompts)
        {
        case settings::PromptStyle::Xbox: return Device::Xbox;
        case settings::PromptStyle::PlayStation: return Device::PlayStation;
        case settings::PromptStyle::Keyboard: return Device::Keyboard;
        default: return Device(g_device.load());
        }
    }

    const char* DeviceName(Device device)
    {
        switch (device)
        {
        case Device::PlayStation: return "playstation";
        case Device::Keyboard: return "keyboard";
        default: return "xbox";
        }
    }

    uint32_t PromptGeneration()
    {
        return g_promptGeneration.load();
    }

    // ---- mouse look ----

    void TakeMouseLook(bool take)
    {
        g_lookTaken = take;
        g_lookX = g_lookY = 0;
    }

    void MouseLookDelta(float& dx, float& dy)
    {
        int x = g_lookX.exchange(0), y = g_lookY.exchange(0);
        bool live = g_focused && !InMenus() && !settings::menu::IsOpen();
        dx = live ? float(x) : 0.0f;
        dy = live ? float(y) : 0.0f;
    }
}
