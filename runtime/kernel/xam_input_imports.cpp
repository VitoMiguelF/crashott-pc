// Controller input (xam XamInput*): the physical pads on each game controller (pad_slots.h: XInput
// and the PlayStation pads read over HID, local co-op), plus the keyboard and the mouse on player 1's
// controller (pc_input.h: the [controls] bindings in gameplay, fixed keys in menus; F1/F2/F5/F12
// belong to the overlay, gpu/window.cpp).
// XINPUT_STATE / XINPUT_CAPABILITIES have the same layout on the console (big-endian in guest memory).

#include "kernel.h"
#include "input.h"
#include "pad_slots.h"
#include "pc_input.h"
#include "input_record.h"
#include "../audio/game_volumes.h"
#include "../settings/direct_boot.h"
#include "../settings/settings.h"
#include "../settings/save_screens.h"
#include "../settings/skip_cinematics.h"
#include "../settings/touch_controls.h"
#include "../settings/settings_menu.h"
#include "../dev/dev_console.h"
#include "../online/mp.h"
#include "../online/mp_menu.h"
#include "../platform/platform.h"
#include "../platform/crt.h"
#include "xinput_types.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED_ = 0x48F;

    // Button names used by CRASH_INPUT_SCRIPT and by the recording (run.toml `input`).
    constexpr std::pair<const char*, uint16_t> BUTTON_NAMES[] = {
        { "A", XINPUT_GAMEPAD_A }, { "B", XINPUT_GAMEPAD_B }, { "X", XINPUT_GAMEPAD_X }, { "Y", XINPUT_GAMEPAD_Y },
        { "START", XINPUT_GAMEPAD_START }, { "BACK", XINPUT_GAMEPAD_BACK }, { "UP", XINPUT_GAMEPAD_DPAD_UP },
        { "DOWN", XINPUT_GAMEPAD_DPAD_DOWN }, { "LEFT", XINPUT_GAMEPAD_DPAD_LEFT }, { "RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT },
        { "LB", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "RB", XINPUT_GAMEPAD_RIGHT_SHOULDER },
        { "L3", XINPUT_GAMEPAD_LEFT_THUMB }, { "R3", XINPUT_GAMEPAD_RIGHT_THUMB },
    };
    // Stick directions for scripts only (full deflection while held): LS_* move, RS_* the camera.
    constexpr std::pair<const char*, uint32_t> STICK_NAMES[] = {
        { "LS_LEFT", 1 }, { "LS_RIGHT", 2 }, { "LS_UP", 4 }, { "LS_DOWN", 8 },
        { "RS_LEFT", 16 }, { "RS_RIGHT", 32 }, { "RS_UP", 64 }, { "RS_DOWN", 128 },
    };
    constexpr uint64_t DEFAULT_HOLD_MS = 200;

    // The XINPUT_GAMEPAD_* bit of each settings::PadButton (0 for the triggers).
    constexpr uint16_t PAD_BITS[] = {
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
        0, 0, XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
    };
    static_assert(std::size(PAD_BITS) == settings::PAD_BUTTON_COUNT);
    constexpr uint8_t TRIGGER_AS_BUTTON = 30;  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD

    // How far `button` is pressed on `pad`: the trigger's own 0-255, or 0 / 255 for a button.
    uint8_t PadLevel(const XINPUT_GAMEPAD& pad, settings::PadButton button)
    {
        if (button == settings::PadButton::LT)
            return pad.bLeftTrigger;
        if (button == settings::PadButton::RT)
            return pad.bRightTrigger;
        return (pad.wButtons & PAD_BITS[size_t(button)]) ? 255 : 0;
    }

    // The controller mapping ([controls] pad_layout, settings/controls.h), in gameplay (frontend node
    // 638 GameRunning) and in the menus (the pause and the runtime's own screens too; not in the
    // cutscenes): with PS2, each game button takes the physical button of the PS2 layout for that
    // screen, a trigger with its analog value. The sticks stay. Applied to every physical pad and to
    // CRASH_INPUT_SCRIPT's presses, not to the keyboard.
    void RemapPad(XINPUT_GAMEPAD& pad)
    {
        settings::PadScreen screen;
        if (!settings::PadScreenOf(settings::menu::FrontendNode(), screen))
            return;
        settings::PadLayout layout = settings::Get().controls.padLayout;
        {
            // In the log once per change: "[input] controller mapping: ps2, gameplay (rb <- rt, rt <- rb)".
            static std::atomic<int> s_logged{ -1 };  // the last layout and screen logged (Xbox 360: -1)
            int key = layout == settings::PadLayout::Xbox360 ? -1 : int(screen);
            if (s_logged.exchange(key) != key && key >= 0)
            {
                std::string list;
                for (size_t b = 0; b < settings::PAD_BUTTON_COUNT; b++)
                {
                    settings::PadButton source = settings::PadLayoutSource(layout, screen, settings::PadButton(b));
                    if (source != settings::PadButton(b))
                        list += (list.empty() ? "" : ", ") + std::string(settings::PadButtonId(settings::PadButton(b))) + " <- " +
                                settings::PadButtonId(source);
                }
                Log("[input] controller mapping: %s, %s (%s), frontend node %u", settings::PadLayoutName(layout),
                    screen == settings::PadScreen::Menu ? "menus" : "gameplay", list.empty() ? "same buttons" : list.c_str(),
                    settings::menu::FrontendNode());
            }
        }
        if (layout == settings::PadLayout::Xbox360)
            return;
        XINPUT_GAMEPAD physical = pad;
        pad.wButtons = 0;
        pad.bLeftTrigger = pad.bRightTrigger = 0;
        for (size_t b = 0; b < settings::PAD_BUTTON_COUNT; b++)
        {
            uint8_t level = PadLevel(physical, settings::PadLayoutSource(layout, screen, settings::PadButton(b)));
            if (settings::PadButton(b) == settings::PadButton::LT)
                pad.bLeftTrigger = level;
            else if (settings::PadButton(b) == settings::PadButton::RT)
                pad.bRightTrigger = level;
            else if (level > TRIGGER_AS_BUTTON)
                pad.wButtons |= PAD_BITS[b];
        }
    }

    // Clock zero: the first read of controller 0 (GetTickCount64 ms), 0 = not yet.
    std::atomic<uint64_t> g_clockStart{ 0 };

    uint64_t ClockNow()
    {
        uint64_t start = g_clockStart.load();
        if (!start)
        {
            g_clockStart.compare_exchange_strong(start, plat::TickCountMs());
            start = g_clockStart.load();
        }
        return plat::TickCountMs() - start;
    }

    // CRASH_INPUT_SCRIPT=5000:A,9000:DOWN:1500,... presses controller 0 buttons at those times (ms
    // since the game first read the controller), each held for 200 ms or for the optional third
    // field (ms): unattended runs that reach menus. Besides the buttons and STICK_NAMES, a stick or
    // trigger at any position: LS=0.5/-1 (x/y, -1..1), RS=x/y, LT=0.7, RT=1 (0..1), e.g.
    // 146000:LS=0.3/1:3000. A time can be relative to a test event (settings/direct_boot.h):
    // gameplay_ready+2500:A (ms after the first gameplay_ready), level_ready#2+0:SHOT (the second
    // one); an entry waits for its event. SHOT saves a screenshot. A P2_ .. P8_ prefix on a
    // button, stick or trigger presses it on a virtual controller 1 .. 7 instead, connected from
    // boot (local co-op tests without a second pad; 4-7 only through ReadExtraController): gameplay_ready+1000:P2_START, P2_LS=1/0. The
    // replay on the game clock (CRASH_REPLAY) is in input_record.cpp.
    struct ScriptedPress
    {
        boot::Anchored at;
        uint16_t buttons;
        uint64_t hold;
        uint32_t sticks = 0;  // STICK_NAMES bits
        uint16_t key = 0;     // KEY_<name>: a keyboard key or mouse button (settings/controls.h names)
        int device = -1;      // DEVICE_XBOX / _PLAYSTATION / _KEYBOARD: that device was used last (icon tests)
        int mouse = -1;       // MOUSE=x/y (0), CLICK=x/y (1), RCLICK=x/y (2), LDOWN=x/y (3), LUP=x/y (4): the cursor
                              // on the 1280x720 image; WHEEL=n (5)
        bool shot = false;    // SHOT: a screenshot
        bool done = false;    // MOUSE / CLICK / SHOT entries act once
        char analog = 0;      // 'L'/'R' stick, 'l'/'r' trigger
        float x = 0.0f, y = 0.0f;
        uint32_t user = 0;    // P2_ etc.: the virtual controller it presses
    };

    int16_t StickValue(float value)
    {
        return int16_t(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
    }

    std::vector<ScriptedPress> ParseInputScript()
    {
        std::vector<ScriptedPress> presses;
        const char* script = getenv("CRASH_INPUT_SCRIPT");
        while (script && *script)
        {
            char* end;
            boot::Anchored time;
            const char* at = script;
            if (!boot::ParseAnchored(at, 1.0, time) || *at != ':')
                break;
            const char* name = at + 1;
            size_t length = strcspn(name, ",:");
            const char* next = name + length;
            uint32_t user = 0;
            if (length > 3 && toupper(uint8_t(name[0])) == 'P' && name[1] >= '2' && name[1] <= '8' && name[2] == '_')
            {
                user = uint32_t(name[1] - '1');
                name += 3;
                length -= 3;
            }
            size_t first = presses.size();
            uint64_t hold = DEFAULT_HOLD_MS;
            if (*next == ':')
            {
                hold = strtoull(next + 1, &end, 10);
                next = end;
            }
            for (const auto& [text, buttons] : BUTTON_NAMES)
            {
                if (strlen(text) == length && plat::CompareNoCaseN(text, name, length) == 0)
                    presses.push_back({ time, buttons, hold });
            }
            if (length == 4 && plat::CompareNoCaseN(name, "SHOT", 4) == 0)
            {
                ScriptedPress press{ time, 0, hold };
                press.shot = true;
                presses.push_back(press);
            }
            for (const auto& [text, stick] : STICK_NAMES)
            {
                if (strlen(text) == length && plat::CompareNoCaseN(text, name, length) == 0)
                    presses.push_back({ time, 0, hold, stick });
            }
            for (auto [prefix, kind] : { std::pair<const char*, int>{ "MOUSE=", 0 }, { "CLICK=", 1 }, { "RCLICK=", 2 }, { "LDOWN=", 3 },
                                         { "LUP=", 4 }, { "WHEEL=", 5 } })
            {
                size_t n = strlen(prefix);
                if (length > n && plat::CompareNoCaseN(name, prefix, n) == 0)
                {
                    ScriptedPress press{ time, 0, hold };
                    press.mouse = kind;
                    input::pc::UseScriptedMouse();
                    press.x = float(strtod(name + n, &end));
                    if (*end == '/')
                        press.y = float(strtod(end + 1, &end));
                    presses.push_back(press);
                }
            }
            if (length > 7 && plat::CompareNoCaseN(name, "DEVICE_", 7) == 0)
            {
                std::string device(name + 7, length - 7);
                for (char& c : device)
                    c = char(tolower(uint8_t(c)));
                for (int d = 0; d < 3; d++)
                {
                    if (device == input::pc::DeviceName(input::pc::Device(d)))
                    {
                        ScriptedPress press{ time, 0, hold };
                        press.device = d;
                        presses.push_back(press);
                    }
                }
            }
            if (length > 4 && plat::CompareNoCaseN(name, "KEY_", 4) == 0)
            {
                uint16_t key;
                if (settings::ParseKey(std::string(name + 4, length - 4), key) && key)
                {
                    ScriptedPress press{ time, 0, hold };
                    press.key = key;
                    presses.push_back(press);
                }
            }
            if (length > 3 && name[2] == '=')
            {
                // LS=x/y, RS=x/y, LT=v, RT=v
                ScriptedPress press{ time, 0, hold };
                char which = char(toupper(uint8_t(name[0]))), type = char(toupper(uint8_t(name[1])));
                press.x = float(strtod(name + 3, &end));
                if (type == 'S' && *end == '/')
                    press.y = float(strtod(end + 1, &end));
                if ((which == 'L' || which == 'R') && (type == 'S' || type == 'T'))
                {
                    press.analog = type == 'S' ? which : char(tolower(which));
                    presses.push_back(press);
                }
            }
            for (size_t i = first; i < presses.size(); i++)
                presses[i].user = user;
            script = *next ? next + 1 : next;
        }
        return presses;
    }

    const std::vector<ScriptedPress>& Script()
    {
        static const std::vector<ScriptedPress> presses = ParseInputScript();
        return presses;
    }

    // Whether `press` is held at `now` (input clock ms); an anchored one only after its event.
    bool Held(const ScriptedPress& press, uint64_t now)
    {
        int64_t start = boot::Resolve(press.at);
        return start >= 0 && now >= uint64_t(start) && now < uint64_t(start) + press.hold;
    }

    bool Due(const ScriptedPress& press, uint64_t now)
    {
        int64_t start = boot::Resolve(press.at);
        return start >= 0 && now >= uint64_t(start);
    }

    // The keys CRASH_INPUT_SCRIPT holds now (KEY_* entries).
    std::vector<uint16_t> ScriptedKeys(uint64_t now)
    {
        std::vector<uint16_t> keys;
        for (const ScriptedPress& press : Script())
        {
            if (press.user || (press.at.event.empty() ? now < uint64_t(press.at.offsetMs) : boot::Resolve(press.at) < 0))
                continue;  // another controller's, or not yet (the cheap test first: most entries are ahead)
            if (press.key && Held(press, now))
            {
                keys.push_back(press.key);
                if (!press.done)
                {
                    const_cast<ScriptedPress&>(press).done = true;
                    input::pc::ScriptKeyDown(press.key);
                }
            }
            if (press.device >= 0 && Held(press, now))
                input::pc::NoteDevice(input::pc::Device(press.device));
            if (press.mouse >= 0 && !press.done && Due(press, now))
            {
                const_cast<ScriptedPress&>(press).done = true;
                input::pc::ScriptMouse(press.x, press.y, press.mouse);
            }
            if (press.shot && !press.done && Due(press, now))
            {
                const_cast<ScriptedPress&>(press).done = true;
                input::RequestShot();
            }
        }
        return keys;
    }

    // Whether CRASH_INPUT_SCRIPT has entries for controller `user` (> 0): a virtual pad there.
    bool ScriptedPad(uint32_t user)
    {
        static const uint32_t users = [] {
            uint32_t mask = 0;
            for (const ScriptedPress& press : Script())
                mask |= 1u << press.user;
            return mask;
        }();
        return user && (users >> user & 1);
    }

    void ScriptedState(XINPUT_STATE& state, uint64_t now, uint32_t user = 0)
    {
        for (const ScriptedPress& press : Script())
        {
            if (press.user != user || (press.at.event.empty() ? now < uint64_t(press.at.offsetMs) : boot::Resolve(press.at) < 0))
                continue;
            if (Held(press, now))
            {
                // A scripted press stands for a controller's: the controller mapping applies to it.
                XINPUT_GAMEPAD mapped{};
                mapped.wButtons = press.buttons;
                if (press.analog == 'l')
                    mapped.bLeftTrigger = uint8_t(std::clamp(press.x, 0.0f, 1.0f) * 255.0f);
                if (press.analog == 'r')
                    mapped.bRightTrigger = uint8_t(std::clamp(press.x, 0.0f, 1.0f) * 255.0f);
                RemapPad(mapped);
                state.Gamepad.wButtons |= mapped.wButtons;
                state.Gamepad.bLeftTrigger = std::max(state.Gamepad.bLeftTrigger, mapped.bLeftTrigger);
                state.Gamepad.bRightTrigger = std::max(state.Gamepad.bRightTrigger, mapped.bRightTrigger);
                if (press.sticks & 1) state.Gamepad.sThumbLX = -32767;
                if (press.sticks & 2) state.Gamepad.sThumbLX = 32767;
                if (press.sticks & 4) state.Gamepad.sThumbLY = 32767;
                if (press.sticks & 8) state.Gamepad.sThumbLY = -32767;
                if (press.sticks & 16) state.Gamepad.sThumbRX = -32767;
                if (press.sticks & 32) state.Gamepad.sThumbRX = 32767;
                if (press.sticks & 64) state.Gamepad.sThumbRY = 32767;
                if (press.sticks & 128) state.Gamepad.sThumbRY = -32767;
                switch (press.analog)
                {
                case 'L': state.Gamepad.sThumbLX = StickValue(press.x), state.Gamepad.sThumbLY = StickValue(press.y); break;
                case 'R': state.Gamepad.sThumbRX = StickValue(press.x), state.Gamepad.sThumbRY = StickValue(press.y); break;
                }
            }
        }
    }

    // Last press for the overlay (input::LastPress).
    std::mutex g_pressMutex;
    input::Press g_lastPress{};
    bool g_hasPress = false;
    std::atomic<uint64_t> g_frame{ 0 };
    std::atomic<uint64_t> g_levelBytes{ 0 }, g_otherBytes{ 0 };

    void NotePresses(uint16_t buttons, uint16_t& previous, bool scripted, uint64_t now)
    {
        uint16_t down = buttons & ~previous;
        previous = buttons;
        if (!down)
            return;
        for (const auto& [text, bits] : BUTTON_NAMES)
        {
            if (down & bits)
            {
                std::lock_guard lock(g_pressMutex);
                g_lastPress = { text, int64_t(now), g_frame.load(), scripted };
                g_hasPress = true;
            }
        }
    }

    // Fills `state` for `user`; false if nothing is connected there. `poll`: XamInputGetState, the
    // game's read once per tick (the replay advances and the recording samples only then).
    bool ReadController(uint32_t user, XINPUT_STATE& state, bool poll = false)
    {
        state = {};
        bool connected = input::slots::Read(user, state.Gamepad);
        RemapPad(state.Gamepad);
        uint32_t playerOne = input::slots::PlayerOneUser(), keyboard = input::slots::KeyboardUser();
        if (user != playerOne)
        {
            // The keyboard and the mouse on this player's own ([controls] player2 = "keyboard").
            if (user == keyboard)
            {
                input::pc::Apply(state, poll, ScriptedKeys(ClockNow()));
                connected = true;
            }
            // A virtual pad from CRASH_INPUT_SCRIPT (P2_ entries) on top.
            if (ScriptedPad(user))
            {
                ScriptedState(state, ClockNow(), user);
                connected = true;
            }
            // Online prototype host: a client's controller (online/mp.h).
            if (mp::RemotePad(user, &state.Gamepad))
                connected = true;
            // The settings screen and the console hide every controller from the game, not only player 1's.
            if (settings::menu::BlocksGameInput() || dev::console::BlocksGameInput() || mp::menu::BlocksGameInput())
                state.Gamepad = {};
        }
        if (user == playerOne)
        {
            // Player 1's controller always exists (the keyboard stands in for it).
            uint64_t now = ClockNow();
            // The touch controls (Android) first: what the pad alone did hides them.
            touch_controls::Apply(state.Gamepad, poll);
            if (keyboard == playerOne)
                input::pc::Apply(state, poll, ScriptedKeys(now));
            static uint16_t s_lastPlayer, s_lastScript;
            NotePresses(state.Gamepad.wButtons, s_lastPlayer, false, now);
            XINPUT_STATE scripted{};
            ScriptedState(scripted, now);
            ScriptedState(state, now);
            // The replay (CRASH_REPLAY) on top; the recording takes the sum, before the settings
            // screen hides it from the game.
            uint16_t before = state.Gamepad.wButtons;
            if (poll)
            {
                MemDiffTick();             // CRASH_MEMDIFF_TIMES (debugging)
                audio::volumes::Tick();    // the game's volume categories, once they exist
                settings::GameOptionsTick();  // co-op mode, invert axis, autosave
            }
            if (poll)
                input::record::Poll(state);
            else
                input::record::Apply(state);
            NotePresses(scripted.Gamepad.wButtons | (state.Gamepad.wButtons & ~before), s_lastScript, true, now);
            connected = true;
            // The settings screen has the controller: the game sees nothing pressed.
            // The name keyboard / delete question (save_screens.h) read it here, then hide it.
            settings::save_screens::FilterGameInput(state.Gamepad, poll);
            // Hold to skip a cutscene or a video (skip_cinematics.h).
            cutscenes::FilterGameInput(state.Gamepad, poll);
            if (settings::menu::BlocksGameInput() || dev::console::BlocksGameInput() || mp::menu::BlocksGameInput())
                state.Gamepad = {};
            state.Gamepad.wButtons &= ~mp::menu::HiddenButtons();  // BACK opens the lobby at the HUB
            state.Gamepad.wButtons |= settings::menu::GameButtons() | mp::menu::GameButtons();  // B when it closes
            mp::LocalPad(&state.Gamepad, poll);  // online prototype: player 1's pad to the peer
        }
        // The packet number changes with the state, per controller.
        static uint32_t s_packet[4];
        static XINPUT_GAMEPAD s_lastPad[4];
        if (memcmp(&state.Gamepad, &s_lastPad[user], sizeof(XINPUT_GAMEPAD)) != 0)
        {
            s_packet[user]++;
            s_lastPad[user] = state.Gamepad;
        }
        state.dwPacketNumber = s_packet[user];
        return connected;
    }
}

namespace input
{
    int64_t ClockMs()
    {
        uint64_t start = g_clockStart.load();
        return start ? int64_t(plat::TickCountMs() - start) : -1;
    }

    bool LastPress(Press& press)
    {
        std::lock_guard lock(g_pressMutex);
        press = g_lastPress;
        return g_hasPress;
    }

    void SetFrame(uint64_t frame)
    {
        g_frame = frame;
    }

    uint64_t Frame()
    {
        return g_frame.load();
    }

    void NoteGameRead(uint64_t bytes, bool level)
    {
        (level ? g_levelBytes : g_otherBytes) += bytes;
    }

    bool ReadExtraController(uint32_t user, void* gamepad)
    {
        XINPUT_STATE state{};
        bool connected = false;
        if (user >= 4 && user < 8)
        {
            if (ScriptedPad(user))
            {
                ScriptedState(state, ClockNow(), user);
                connected = true;
            }
            if (mp::RemotePad(user, &state.Gamepad))
                connected = true;
            if (settings::menu::BlocksGameInput() || dev::console::BlocksGameInput() || mp::menu::BlocksGameInput())
                state.Gamepad = {};
        }
        memcpy(gamepad, &state.Gamepad, sizeof(XINPUT_GAMEPAD));
        return connected;
    }

    uint16_t MenuButtons()
    {
        XINPUT_STATE state{};
        input::slots::Read(input::slots::PlayerOneUser(), state.Gamepad);
        RemapPad(state.Gamepad);  // PS2: Y goes back on the runtime's screens too
        state.Gamepad.wButtons |= input::pc::MenuKeys(ScriptedKeys(ClockNow()));
        ScriptedState(state, ClockNow());
        input::record::Apply(state);
        constexpr int16_t STICK = 16000;
        if (state.Gamepad.sThumbLY > STICK) state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        if (state.Gamepad.sThumbLY < -STICK) state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (state.Gamepad.sThumbLX < -STICK) state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        if (state.Gamepad.sThumbLX > STICK) state.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        return state.Gamepad.wButtons;
    }
}

// u32 XamInputGetCapabilities(u32 UserIndex, u32 Flags, XINPUT_CAPABILITIES* Caps)
PPC_FUNC(__imp__XamInputGetCapabilities)
{
    KERNEL_IMPORT_ENTER(XamInputGetCapabilities);
    uint32_t user = ctx.r3.u32 == 0xFF ? 0 : ctx.r3.u32;
    uint32_t caps = ctx.r5.u32;
    XINPUT_STATE state;
    if (user >= 4 || !ReadController(user, state))
    {
        ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED_;
        return;
    }
    if (caps)
    {
        memset(base + caps, 0, 20);
        PPC_STORE_U8(caps + 0, 1);           // XINPUT_DEVTYPE_GAMEPAD
        PPC_STORE_U8(caps + 1, 1);           // XINPUT_DEVSUBTYPE_GAMEPAD
        PPC_STORE_U16(caps + 4, 0xF3FF);     // all buttons
        PPC_STORE_U8(caps + 6, 0xFF);        // triggers
        PPC_STORE_U8(caps + 7, 0xFF);
        PPC_STORE_U16(caps + 8, 0xFFC0);     // thumbsticks
        PPC_STORE_U16(caps + 10, 0xFFC0);
        PPC_STORE_U16(caps + 12, 0xFFC0);
        PPC_STORE_U16(caps + 14, 0xFFC0);
        PPC_STORE_U16(caps + 16, 0xFF);      // vibration
        PPC_STORE_U16(caps + 18, 0xFF);
    }
    ctx.r3.u64 = 0;
}

// u32 XamInputGetState(u32 UserIndex, u32 Flags, XINPUT_STATE* State)
PPC_FUNC(__imp__XamInputGetState)
{
    KERNEL_IMPORT_ENTER(XamInputGetState);
    uint32_t user = ctx.r3.u32 == 0xFF ? 0 : ctx.r3.u32;
    uint32_t statePtr = ctx.r5.u32;
    XINPUT_STATE state;
    if (user >= 4 || !ReadController(user, state, true))
    {
        ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED_;
        return;
    }
    // CRASH_INPUT_TRACE=<file.csv>: one line per game read of controller 0 (input clock ms, frame,
    // default.rcf bytes read so far, other game file bytes): how the tick clock and the loads line up.
    if (static FILE* trace = getenv("CRASH_INPUT_TRACE") ? fopen(getenv("CRASH_INPUT_TRACE"), "w") : nullptr; trace && user == 0)
    {
        fprintf(trace, "%llu,%llu,%llu,%llu\n", (unsigned long long)ClockNow(), (unsigned long long)g_frame.load(), (unsigned long long)g_levelBytes.load(), (unsigned long long)g_otherBytes.load());
        fflush(trace);
    }
    if (statePtr)
    {
        PPC_STORE_U32(statePtr + 0, state.dwPacketNumber);
        PPC_STORE_U16(statePtr + 4, state.Gamepad.wButtons);
        PPC_STORE_U8(statePtr + 6, state.Gamepad.bLeftTrigger);
        PPC_STORE_U8(statePtr + 7, state.Gamepad.bRightTrigger);
        PPC_STORE_U16(statePtr + 8, uint16_t(state.Gamepad.sThumbLX));
        PPC_STORE_U16(statePtr + 10, uint16_t(state.Gamepad.sThumbLY));
        PPC_STORE_U16(statePtr + 12, uint16_t(state.Gamepad.sThumbRX));
        PPC_STORE_U16(statePtr + 14, uint16_t(state.Gamepad.sThumbRY));
    }
    ctx.r3.u64 = 0;
}

// u32 XamInputSetState(u32 UserIndex, u32 Flags, XINPUT_VIBRATION* Vibration)
PPC_FUNC(__imp__XamInputSetState)
{
    KERNEL_IMPORT_ENTER(XamInputSetState);
    uint32_t user = ctx.r3.u32 == 0xFF ? 0 : ctx.r3.u32;
    uint32_t vibrationPtr = ctx.r5.u32;
    XINPUT_STATE state;
    if (user >= 4 || !ReadController(user, state))
    {
        ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED_;
        return;
    }
    if (vibrationPtr)
    {
        input::slots::SetVibration(user, PPC_LOAD_U16(vibrationPtr), PPC_LOAD_U16(vibrationPtr + 2));
    }
    ctx.r3.u64 = 0;
}

// u32 XGetGameRegion(): all regions, as in Xenia.
PPC_FUNC(__imp__XGetGameRegion)
{
    KERNEL_IMPORT_ENTER(XGetGameRegion);
    ctx.r3.u64 = 0xFFFF;
}

// u32 XGetLanguage(): the language of settings.toml (settings.h), English by default.
PPC_FUNC(__imp__XGetLanguage)
{
    KERNEL_IMPORT_ENTER(XGetLanguage);
    ctx.r3.u64 = settings::StartupLanguage().xconfig;
}

// NTSTATUS ExGetXConfigSetting(u16 Category, u16 Setting, void* Buffer, u16 BufferSize, u16* RequiredSize)
// User category values as in Xenia's xboxkrnl_xconfig.cc.
PPC_FUNC(__imp__ExGetXConfigSetting)
{
    KERNEL_IMPORT_ENTER(ExGetXConfigSetting);
    uint32_t category = ctx.r3.u32 & 0xFFFF, setting = ctx.r4.u32 & 0xFFFF;
    uint32_t buffer = ctx.r5.u32, bufferSize = ctx.r6.u32 & 0xFFFF, requiredPtr = ctx.r7.u32;
    uint8_t value[4] = {};
    uint32_t size = 0;
    if (category == 3)
    {
        switch (setting)
        {
        case 0x0009: size = 4; value[3] = uint8_t(settings::StartupLanguage().xconfig); break;  // language (settings.toml)
        case 0x000A: size = 4; value[1] = 0x04; break;                // video flags: widescreen (0x00040000)
        case 0x000B: size = 4; break;                                 // audio flags
        case 0x000C: size = 4; value[3] = 0x40; break;                // retail flags
        case 0x000E: size = 1; value[0] = settings::StartupLanguage().country; break;  // country: US, or the language's (NL = Dutch)
        case 0x000F: size = 1; value[0] = 0; break;                   // parental control
        default: break;
        }
    }
    uint32_t status = 0;
    if (!size)
    {
        status = 0xC0000034; // STATUS_OBJECT_NAME_NOT_FOUND
    }
    else if (buffer)
    {
        if (bufferSize < size)
            status = 0xC0000023; // STATUS_BUFFER_TOO_SMALL
        else
            memcpy(base + buffer, value, size);
    }
    if (requiredPtr)
        PPC_STORE_U16(requiredPtr, uint16_t(size));
    KERNEL_LOG("[kernel] ExGetXConfigSetting(%u, 0x%X, size %u) lr=0x%08X -> 0x%08X", category, setting, bufferSize, GuestLR(ctx), status);
    ctx.r3.u64 = status;
}
