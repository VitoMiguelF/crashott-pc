// The name entry's on-screen keyboard and deleting saves on the slot screen (save_screens.h).
//
// What the game does (read in the recompiled code):
// - CNameEntryScreenAction (frontend node 96 NameEntryScreen): vf04 sub_820B4A88 enters, vf05
//   sub_820B45A0 runs every frame, vf06 sub_820B43C0 leaves. this+24 = the page's layer, +28 = the
//   name's text, +20 = the page, +32 / +36 = the arrows at the sides of the wheel, +44 = the wheel's letter (0-25 =
//   A-Z, 26-35 = 0-9). The name is a global: char[32] at 0x82497328 with its length at 0x82497348
//   (at most 5). vf05 asks the input for events: 23 / 33 turn the wheel, 1 (A) plays
//   FE_SFX_NameScreen / letter_select and calls sub_820B44C0 (appends the wheel's letter with
//   "%c" / "%i", refreshes the text with sub_820B42E8), 18 (X) plays "erase" and calls
//   sub_820B4568 (drops the last one), 13 (Y) with a name plays "done" and sets bit 0x80 of byte
//   16653 of the frontend sound player (sub_821FA440(sub_821FB4F0(), 6)): the tree's condition for
//   node 96 then takes exit 97 ExitDone; with no name it plays FE_SFX / backpackNONONO. The wheel
//   is 36 page elements Char_a..Char_z, Char_0..Char_9 placed on a circle every frame, framed by
//   the sprites Divider_TopLeft / _TopRight / _BottomLeft / _BottomRight (GameStart_NameEntry.pag).
// - Here vf05 runs as before with the controller hidden from it (except B, its BACK), the wheel,
//   its arrows and the dividers get alpha 0 (pure3d::frontend Drawable alpha, sub_822EEA18), and each key of
//   the keyboard does what the wheel's button would: +44 = the letter, the same sound, the same
//   function. DONE is the Y branch.
// - CGameSlotScreenAction::vf05 (sub_820AEE90; node 154 GameSlotScreen, both LOAD GAME and NEW
//   GAME): this+24 = the slot menu, its selected item at +152 (int8) = slot - 1; slot N is the
//   package "CrashOTT GameSlot N". The game reads the slots when the screen is entered (the
//   AccessMemoryCard / ReadingCard nodes), so a deleted package would stay on the list: after a
//   delete the tree takes exit 314 ExitToGameSlotScreen (LoadFailed's way back: AccessMemoryCard >
//   ReadingCard > GameSlotScreen, or NoValidSaveFiles when none is left in LOAD GAME).
// - core::XenonSaveDrive::vf19 (sub_82296710, r3 = drive, r4 = file name) is the game's own
//   delete: it enumerates the packages and calls XamContentDelete for the one with that name. The
//   game never calls it from a menu (Xbox saves were deleted from the dashboard). The drive is the
//   `this` of its enumeration (sub_822959D8), which the slot screen runs every frame.

#include "save_screens.h"
#include "direct_boot.h"
#include "menu_art.h"
#include "menu_mouse.h"
#include "pc_frontend.h"
#include "prompt_icons.h"
#include "settings.h"
#include "../diagnostics.h"
#include "../gpu/overlay.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../kernel/pc_input.h"
#include "../kernel/xam_content.h"
#include "../platform/platform.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_820B4A88);  // CNameEntryScreenAction::vf04 (enter)
extern "C" PPC_FUNC(__imp__sub_820B45A0);  // CNameEntryScreenAction::vf05 (every frame)
extern "C" PPC_FUNC(__imp__sub_820B43C0);  // CNameEntryScreenAction::vf06 (leave)
extern "C" PPC_FUNC(__imp__sub_820AEE90);  // CGameSlotScreenAction::vf05 (every frame)
extern "C" PPC_FUNC(__imp__sub_822959D8);  // XenonSaveDrive: enumerate the save packages (r3 = the drive)
PPC_EXTERN_FUNC(sub_820B44C0);  // name entry: append the wheel's letter (this+44)
PPC_EXTERN_FUNC(sub_820B4568);  // name entry: erase the last letter
PPC_EXTERN_FUNC(sub_82296710);  // core::XenonSaveDrive::vf19: delete the package named r4
PPC_EXTERN_FUNC(sub_821FB4F0);  // the managers; sub_821FA440(them, 6) = the frontend sound player
PPC_EXTERN_FUNC(sub_821FA440);
PPC_EXTERN_FUNC(sub_821DB1D8);  // sound player: play (bank r4, sound r5, volume f1)
PPC_EXTERN_FUNC(sub_821DB260);  // the menus' "previous" move sound
PPC_EXTERN_FUNC(sub_821DB280);  // the menus' "next" move sound
PPC_EXTERN_FUNC(sub_822F1DA8);  // layer: the text named r4
PPC_EXTERN_FUNC(sub_822F1DA0);  // page: the sprite whose name hash is r4
PPC_EXTERN_FUNC(sub_822E4A90);  // the hash of the name r3 (what the find functions compare)
PPC_EXTERN_FUNC(sub_822EEA18);  // Drawable: set alpha (f1, 0-1)
PPC_EXTERN_FUNC(sub_822F2E40);  // Menu::SetSelection(menu, index)

namespace settings::save_screens
{
    namespace
    {
        using menu_art::Color;
        using menu_art::FontId;
        using menu_art::TextStyle;

        // ---- the game ----------------------------------------------------------------------------
        constexpr uint32_t NODE_NAME_ENTRY = 96, NODE_SLOTS = 154;
        constexpr int32_t EXIT_TO_SLOT_SCREEN = 314;  // LoadFailed's ExitToGameSlotScreen -> AccessMemoryCard
        constexpr int32_t KNOWN_QUIT_EXIT = 86;       // the numbering this file knows (as direct_boot.cpp)
        constexpr uint32_t NAME_BUFFER = 0x82497328, NAME_LENGTH = 0x82497348;
        constexpr int NAME_MAX = 5;
        constexpr uint32_t ACTION_PAGE_OBJECT = 20;  // the page (sprites are found from it, as vf04 finds the arrows)
        constexpr uint32_t ACTION_PAGE = 24, ACTION_ARROW_LEFT = 32, ACTION_ARROW_RIGHT = 36, ACTION_LETTER = 44;
        constexpr uint32_t PLAYER_DONE_BYTE = 16653;  // | 0x80: DONE (node 96 takes exit 97)
        constexpr uint32_t VOLUME = 0x82000DC8;       // the float the name screen plays its sounds at
        constexpr int SLOTS = 5;

        // Strings of the game (checked against the image once).
        struct GuestString
        {
            uint32_t address;
            const char* text;
        };
        constexpr GuestString BANK_NAME{ 0x820075BC, "FE_SFX_NameScreen" }, SOUND_LETTER{ 0x8200759C, "letter_select" },
                              SOUND_ERASE{ 0x82007594, "erase" }, SOUND_DONE{ 0x8200758C, "done" },
                              SOUND_NO{ 0x8200757C, "backpackNONONO" }, BANK_FE{ 0x82006DA4, "FE_SFX" },
                              SOUND_BACK{ 0x82006DAC, "back" }, SOUND_SELECT{ 0x82006DB4, "select" },
                              SOUND_SCROLL{ 0x820075AC, "letter_scroll" };

        // ---- the keyboard --------------------------------------------------------------------------
        // Virtual 1280x720 screen (the game's image, fitted into the window), where the wheel was:
        // between the name (y ~150) and the game's hints (y ~580).
        constexpr float VIRTUAL_W = 1280.0f, VIRTUAL_H = 720.0f;
        constexpr float KEY_W = 50.0f, KEY_H = 46.0f, PITCH_X = 56.0f, PITCH_Y = 52.0f, KEYS_TOP = 296.0f;
        constexpr float WIDE_W = 248.0f, WIDE_GAP = 12.0f;
        constexpr char32_t KEY_ERASE = 1, KEY_DONE = 2;

        struct Key
        {
            char32_t c;  // the character, or KEY_ERASE / KEY_DONE
            int row;
            float x, y, w, h;
        };

        std::vector<Key> MakeKeys()
        {
            static const char* ROWS[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
            std::vector<Key> keys;
            for (int row = 0; row < 4; row++)
            {
                int n = int(strlen(ROWS[row]));
                float left = VIRTUAL_W * 0.5f - ((n - 1) * PITCH_X + KEY_W) * 0.5f;
                for (int i = 0; i < n; i++)
                    keys.push_back({ char32_t(ROWS[row][i]), row, left + i * PITCH_X, KEYS_TOP + row * PITCH_Y, KEY_W, KEY_H });
            }
            float left = VIRTUAL_W * 0.5f - WIDE_W - WIDE_GAP * 0.5f, y = KEYS_TOP + 4 * PITCH_Y;
            keys.push_back({ KEY_ERASE, 4, left, y, WIDE_W, KEY_H });
            keys.push_back({ KEY_DONE, 4, left + WIDE_W + WIDE_GAP, y, WIDE_W, KEY_H });
            return keys;
        }
        const std::vector<Key>& Keys()
        {
            static const std::vector<Key> keys = MakeKeys();
            return keys;
        }
        int KeyOf(char32_t c)
        {
            const std::vector<Key>& keys = Keys();
            for (size_t i = 0; i < keys.size(); i++)
            {
                if (keys[i].c == c)
                    return int(i);
            }
            return -1;
        }

        // ---- shared with the render thread ---------------------------------------------------------
        enum class Mode : uint8_t { None, Keyboard, SlotPrompt, Confirm };
        struct Visual
        {
            Mode mode = Mode::None;
            int key = 0;          // keyboard: highlighted key
            int length = 0;       // keyboard: the name's length
            bool portuguese = false;
            int slot = 0;         // slot screen: 1-5
            std::string saveName, saveDate, saveTime;
            int choice = 0;       // the question: 0 = NO, 1 = YES
            bool operator==(const Visual&) const = default;
        };
        // What the mouse can point at, as last drawn (virtual units).
        enum class Zone : uint8_t { Key, Prompt, No, Yes, Back, Accept };
        struct HitZone
        {
            float x0, y0, x1, y1;
            Zone zone;
            int index;
        };
        std::mutex g_mutex;
        Visual g_visual;
        uint64_t g_visualTime = 0;  // last game frame that set it
        std::vector<HitZone> g_zones;

        // ---- game thread -------------------------------------------------------------------------
        std::atomic<uint32_t> g_node{ 0xFFFFFFFF };
        std::atomic<uint16_t> g_held{ 0 }, g_pressed{ 0 };  // controller 0 as the game reads it (FilterGameInput)
        std::atomic<bool> g_keyboardOn{ false }, g_confirmOn{ false }, g_releasing{ false };
        std::atomic<uint64_t> g_keyboardTime{ 0 };
        bool g_checked = false, g_ok = false;
        uint32_t g_nameAction = 0;
        uint32_t g_wheelAction = 0;
        // The wheel's 36 letters and the page's four scroll dividers around its centre.
        constexpr int WHEEL_ELEMENTS = 40, NAME_SLOT = 24;
        constexpr const char* DIVIDERS[4] = { "Divider_TopLeft", "Divider_TopRight", "Divider_BottomLeft", "Divider_BottomRight" };
        uint32_t g_wheelElements[WHEEL_ELEMENTS] = {};
        uint32_t g_names = 0;       // guest "Char_a" .. "Char_9", the dividers' names, NAME_SLOT bytes each
        uint32_t g_fileName = 0;    // guest buffer for the slot's file name
        int g_key = 0;
        bool g_done = false;        // DONE given: nothing more until the screen leaves
        uint16_t g_heldDir = 0;
        uint64_t g_heldSince = 0, g_lastRepeat = 0;
        uint32_t g_mouseMoves = 0, g_mouseClicks = 0;
        std::atomic<uint32_t> g_drive{ 0 };
        int g_choice = 0;
        int g_confirmSlot = 0;
        bool g_refreshPending = false, g_restorePending = false, g_leftSlots = false;
        int g_restoreIndex = 0;
        bool g_portuguese = false, g_mouseMenus = true;
        uint64_t g_optionsTime = 0;

        // The options these screens follow (the language of their texts, mouse in menus), read
        // twice a second.
        void ReadOptions()
        {
            uint64_t now = plat::TickCountMs();
            if (g_optionsTime && now - g_optionsTime < 500)
                return;
            g_optionsTime = now;
            settings::Values values = settings::Get();
            g_portuguese = values.language.rfind("pt", 0) == 0;
            g_mouseMenus = values.controls.mouseMenus;
        }

        bool Portuguese()
        {
            ReadOptions();
            return g_portuguese;
        }

        // The game's strings and the frontend numbering are the ones this file knows.
        bool Ok(uint8_t* base)
        {
            if (g_checked)
                return g_ok;
            g_checked = true;
            g_ok = true;
            for (const GuestString& s : { BANK_NAME, SOUND_LETTER, SOUND_ERASE, SOUND_DONE, SOUND_NO, BANK_FE, SOUND_BACK, SOUND_SELECT, SOUND_SCROLL })
                g_ok = g_ok && strcmp(reinterpret_cast<const char*>(base + s.address), s.text) == 0;
            if (!g_ok)
                Log("[saves] this executable's name entry is not the one the runtime knows: the wheel stays");
            g_names = GuestAlloc(WHEEL_ELEMENTS * NAME_SLOT);
            for (int i = 0; i < WHEEL_ELEMENTS; i++)
            {
                char name[NAME_SLOT];
                if (i < 26)
                    snprintf(name, sizeof(name), "Char_%c", 'a' + i);
                else if (i < 36)
                    snprintf(name, sizeof(name), "Char_%d", i - 26);
                else
                    snprintf(name, sizeof(name), "%s", DIVIDERS[i - 36]);
                memcpy(base + g_names + i * NAME_SLOT, name, strlen(name) + 1);
            }
            g_fileName = GuestAlloc(64);
            return g_ok;
        }

        // Buttons that went down since the last call, with auto-repeat of a held direction.
        uint16_t TakePressed()
        {
            constexpr uint16_t DIRECTIONS = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
            constexpr uint64_t REPEAT_DELAY_MS = 380, REPEAT_MS = 100;
            uint16_t pressed = g_pressed.exchange(0), held = g_held.load();
            uint64_t now = plat::TickCountMs();
            if (pressed & DIRECTIONS)
            {
                g_heldDir = pressed & DIRECTIONS;
                g_heldSince = g_lastRepeat = now;
            }
            else if (g_heldDir && (held & g_heldDir) == g_heldDir)
            {
                if (now - g_heldSince >= REPEAT_DELAY_MS && now - g_lastRepeat >= REPEAT_MS)
                {
                    pressed |= g_heldDir;
                    g_lastRepeat = now;
                }
            }
            else
                g_heldDir = 0;
            return pressed;
        }

        // The mouse on the image (virtual units) and what changed since the last call.
        struct MouseInput
        {
            bool on = false;      // over the image, mouse menus enabled
            bool moved = false, clicked = false;
            float x = 0, y = 0;
        };
        MouseInput ReadMouse()
        {
            MouseInput m;
            uint32_t moves = input::pc::CursorMoves(), clicks = input::pc::LeftClicks();
            m.moved = moves != g_mouseMoves;
            m.clicked = clicks != g_mouseClicks;
            g_mouseMoves = moves, g_mouseClicks = clicks;
            input::pc::TakeMenuClick();  // a click here is not the game's A
            ReadOptions();
            m.on = g_mouseMenus && input::pc::CursorOnImage(m.x, m.y);
            if (!m.on)
                m.moved = m.clicked = false;
            return m;
        }
        bool ZoneAt(float x, float y, HitZone& out)
        {
            std::lock_guard lock(g_mutex);
            for (const HitZone& zone : g_zones)
            {
                if (x >= zone.x0 && x < zone.x1 && y >= zone.y0 && y < zone.y1)
                {
                    out = zone;
                    return true;
                }
            }
            return false;
        }

        void Publish(const Visual& visual)
        {
            std::lock_guard lock(g_mutex);
            if (visual.mode != g_visual.mode)
                g_zones.clear();  // the old screen's zones
            g_visual = visual;
            g_visualTime = plat::TickCountMs();
        }

        // ---- calls into the game (the hook's context; registers restored) --------------------------
        uint32_t SoundPlayer(PPCContext& ctx, uint8_t* base)
        {
            sub_821FB4F0(ctx, base);
            ctx.r4.s64 = 6;
            sub_821FA440(ctx, base);
            return ctx.r3.u32;
        }

        void PlaySound(PPCContext& ctx, uint8_t* base, const GuestString& bank, const GuestString& sound)
        {
            PPCContext saved = ctx;
            uint32_t player = SoundPlayer(ctx, base);
            uint32_t bits = LoadBE32(VOLUME);
            float volume;
            memcpy(&volume, &bits, 4);
            ctx.r3.u64 = player;
            ctx.r4.u64 = bank.address;
            ctx.r5.u64 = sound.address;
            ctx.f1.f64 = volume;
            sub_821DB1D8(ctx, base);
            ctx = saved;
        }

        void MoveSound(PPCContext& ctx, uint8_t* base, bool forward)
        {
            PPCContext saved = ctx;
            SoundPlayer(ctx, base);
            if (forward)
                sub_821DB280(ctx, base);
            else
                sub_821DB260(ctx, base);
            ctx = saved;
        }

        void CallWithThis(PPCContext& ctx, uint8_t* base, uint32_t self, void (*function)(PPCContext&, uint8_t*))
        {
            PPCContext saved = ctx;
            ctx.r3.u64 = self;
            function(ctx, base);
            ctx = saved;
        }

        // The name as the game has it (any thread).
        std::string Name()
        {
            const char* name = reinterpret_cast<const char*>(g_memory.Translate(NAME_BUFFER));
            return std::string(name, strnlen(name, 31));
        }

        // ---- the name entry ----------------------------------------------------------------------
        // The wheel's letters, its arrows (this+32 / +36, which vf05 pulses) and the dividers
        // around its centre get alpha 0 every frame.
        void HideWheel(PPCContext& ctx, uint8_t* base, uint32_t action)
        {
            PPCContext saved = ctx;
            if (action != g_wheelAction)
            {
                g_wheelAction = action;
                uint32_t page = LoadBE32(action + ACTION_PAGE);
                for (int i = 0; i < WHEEL_ELEMENTS; i++)
                {
                    ctx.r4.u64 = g_names + i * NAME_SLOT;
                    if (i < 36)
                    {
                        ctx.r3.u64 = page;
                        sub_822F1DA8(ctx, base);  // the letters: texts of the layer, as vf05 finds them
                    }
                    else
                    {
                        ctx.r3.u64 = g_names + i * NAME_SLOT;
                        sub_822E4A90(ctx, base);
                        ctx.r4.u64 = ctx.r3.u64;
                        ctx.r3.u64 = LoadBE32(action + ACTION_PAGE_OBJECT);
                        sub_822F1DA0(ctx, base);  // the dividers: sprites of the page, as vf04 finds the arrows
                    }
                    g_wheelElements[i] = page ? ctx.r3.u32 : 0;
                }
                int found = int(std::count_if(std::begin(g_wheelElements), std::end(g_wheelElements), [](uint32_t e) { return e != 0; }));
                Log("[saves] name entry: %d of %d wheel elements hidden (letters and dividers)", found, WHEEL_ELEMENTS);
            }
            auto hide = [&](uint32_t element) {
                if (!element)
                    return;
                ctx.r3.u64 = element;
                ctx.f1.f64 = 0.0;
                sub_822EEA18(ctx, base);
            };
            for (uint32_t element : g_wheelElements)
                hide(element);
            hide(LoadBE32(action + ACTION_ARROW_LEFT));
            hide(LoadBE32(action + ACTION_ARROW_RIGHT));
            ctx = saved;
        }

        // A character, as the wheel's A would add it.
        void TypeChar(PPCContext& ctx, uint8_t* base, uint32_t action, char32_t c)
        {
            int letter = c >= 'A' && c <= 'Z' ? int(c - 'A') : c >= '0' && c <= '9' ? 26 + int(c - '0') : -1;
            if (letter < 0)
                return;
            int before = int(LoadBE32(NAME_LENGTH));
            StoreBE32(action + ACTION_LETTER, uint32_t(letter));
            PlaySound(ctx, base, BANK_NAME, SOUND_LETTER);
            CallWithThis(ctx, base, action, sub_820B44C0);
            Log("[saves] name: '%c'%s -> \"%s\"", char(c), before >= NAME_MAX ? " (the name is full: 5 characters)" : "", Name().c_str());
        }

        void Erase(PPCContext& ctx, uint8_t* base, uint32_t action)
        {
            PlaySound(ctx, base, BANK_NAME, SOUND_ERASE);
            CallWithThis(ctx, base, action, sub_820B4568);
            Log("[saves] name: erase -> \"%s\"", Name().c_str());
        }

        // The Y branch of the wheel: with a name, "done" and the flag the tree waits for.
        void Done(PPCContext& ctx, uint8_t* base)
        {
            if (!*(base + NAME_BUFFER))
            {
                PlaySound(ctx, base, BANK_FE, SOUND_NO);
                Log("[saves] name: DONE with no name (refused, as the game does)");
                return;
            }
            PlaySound(ctx, base, BANK_NAME, SOUND_DONE);
            PPCContext saved = ctx;
            uint32_t player = SoundPlayer(ctx, base);
            ctx = saved;
            base[player + PLAYER_DONE_BYTE] |= 0x80;
            g_done = true;
            Log("[saves] name: DONE -> \"%s\"", Name().c_str());
        }

        void Activate(PPCContext& ctx, uint8_t* base, uint32_t action, int key)
        {
            char32_t c = Keys()[size_t(key)].c;
            if (c == KEY_ERASE)
                Erase(ctx, base, action);
            else if (c == KEY_DONE)
                Done(ctx, base);
            else
                TypeChar(ctx, base, action, c);
        }

        // Up / down: the key in that row nearest in x; left / right: around the row.
        int Step(int key, int dx, int dy)
        {
            const std::vector<Key>& keys = Keys();
            const Key& from = keys[size_t(key)];
            if (dx)
            {
                std::vector<int> row;
                for (size_t i = 0; i < keys.size(); i++)
                {
                    if (keys[i].row == from.row)
                        row.push_back(int(i));
                }
                auto at = std::find(row.begin(), row.end(), key) - row.begin();
                return row[size_t((at + dx + int(row.size())) % int(row.size()))];
            }
            int target = (from.row + dy + 5) % 5;
            float cx = from.x + from.w * 0.5f;
            int best = key;
            float bestDistance = 1e9f;
            for (size_t i = 0; i < keys.size(); i++)
            {
                if (keys[i].row != target)
                    continue;
                float d = std::fabs(keys[i].x + keys[i].w * 0.5f - cx);
                if (d < bestDistance)
                    bestDistance = d, best = int(i);
            }
            return best;
        }

        void StartKeyboard()
        {
            g_key = KeyOf('A');
            g_done = false;
            g_heldDir = 0;
            g_pressed = 0;
            g_mouseMoves = input::pc::CursorMoves(), g_mouseClicks = input::pc::LeftClicks();
            input::pc::SetTextEntry(true);
            input::pc::TakeTypedText();
            g_keyboardOn = true;
            Log("[saves] name entry: on-screen keyboard (the wheel is hidden)");
            boot::Event("name_entry");
        }

        void StopKeyboard(const char* why)
        {
            if (!g_keyboardOn.exchange(false))
                return;
            input::pc::SetTextEntry(false);
            g_releasing = true;
            g_nameAction = 0;
            Publish({});
            Log("[saves] name entry: keyboard closed (%s), name \"%s\"", why, Name().c_str());
        }

        void NameEntryFrame(PPCContext& ctx, uint8_t* base, uint32_t action)
        {
            if (g_node.load() != NODE_NAME_ENTRY && g_node.load() != 0xFFFFFFFF)
                return;
            if (!g_keyboardOn || action != g_nameAction)
            {
                g_nameAction = action;
                StartKeyboard();
            }
            g_keyboardTime = plat::TickCountMs();
            HideWheel(ctx, base, action);

            uint16_t pressed = TakePressed();
            std::u32string typed = input::pc::TakeTypedText();
            MouseInput mouse = ReadMouse();
            if (!g_done)
            {
                HitZone zone;
                bool onKey = mouse.on && ZoneAt(mouse.x, mouse.y, zone) && zone.zone == Zone::Key;
                if (mouse.moved && onKey)
                    g_key = zone.index;
                if (mouse.clicked && onKey)
                {
                    g_key = zone.index;
                    Activate(ctx, base, action, g_key);
                }
                for (char32_t c : typed)
                {
                    if (g_done)
                        break;
                    if (c == U'\r')
                        Done(ctx, base);
                    else if (int key = KeyOf(c); key >= 0)
                    {
                        g_key = key;
                        TypeChar(ctx, base, action, c);
                    }
                }
                if (!g_done)
                {
                    if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) g_key = Step(g_key, -1, 0);
                    if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) g_key = Step(g_key, 1, 0);
                    if (pressed & XINPUT_GAMEPAD_DPAD_UP) g_key = Step(g_key, 0, -1);
                    if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) g_key = Step(g_key, 0, 1);
                    if (pressed & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN))
                        PlaySound(ctx, base, BANK_NAME, SOUND_SCROLL);
                    if (pressed & XINPUT_GAMEPAD_A)
                        Activate(ctx, base, action, g_key);
                    if (pressed & XINPUT_GAMEPAD_X)
                        Erase(ctx, base, action);
                    if (pressed & (XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_START))
                        Done(ctx, base);
                }
            }
            Visual visual;
            visual.mode = Mode::Keyboard;
            visual.key = g_key;
            visual.length = std::clamp(int(LoadBE32(NAME_LENGTH)), 0, NAME_MAX);
            visual.portuguese = Portuguese();
            Publish(visual);
        }

        // ---- the slot screen ---------------------------------------------------------------------
        std::string SlotFile(int slot)
        {
            return "CrashOTT GameSlot " + std::to_string(slot);
        }

        // "Slot: 1  Name: AA  Last Save: 10.20.26  Time: 00:20" -> the parts.
        std::string Field(const std::string& display, const char* label)
        {
            size_t at = display.find(label);
            if (at == std::string::npos)
                return {};
            at += strlen(label);
            while (at < display.size() && display[at] == ' ')
                at++;
            size_t end = display.find("  ", at);
            return display.substr(at, end == std::string::npos ? std::string::npos : end - at);
        }

        void CloseConfirm(const char* why)
        {
            if (!g_confirmOn.exchange(false))
                return;
            g_releasing = true;
            Log("[saves] delete slot %d: %s", g_confirmSlot, why);
        }

        bool DeleteSlot(PPCContext& ctx, uint8_t* base, int slot)
        {
            std::string file = SlotFile(slot);
            bool viaGame = false;
            if (uint32_t drive = g_drive.load())
            {
                memset(base + g_fileName, 0, 64);
                memcpy(base + g_fileName, file.c_str(), file.size());
                PPCContext saved = ctx;
                ctx.r3.u64 = drive;
                ctx.r4.u64 = g_fileName;
                sub_82296710(ctx, base);
                viaGame = ctx.r3.u32 & 0xFF;
                ctx = saved;
                Log("[saves] delete slot %d: XenonSaveDrive::vf19(0x%08X, \"%s\") -> %s", slot, drive, file.c_str(), viaGame ? "deleted" : "failed");
            }
            if (xam::content::SavedGameExists(file))
            {
                if (viaGame)
                    Log("[saves] delete slot %d: the package is still there after the game's delete", slot);
                xam::content::DeleteSavedGame(file);
            }
            bool gone = !xam::content::SavedGameExists(file);
            Log("[saves] delete slot %d (\"%s\"): %s", slot, file.c_str(), gone ? "deleted" : "FAILED, still there");
            if (gone)
                boot::Event("save_deleted", file.c_str());
            return gone;
        }

        void SlotFrame(PPCContext& ctx, uint8_t* base, uint32_t action)
        {
            if (g_node.load() != NODE_SLOTS)
            {
                CloseConfirm("the slot screen left");
                return;
            }
            uint32_t menu = LoadBE32(action + ACTION_PAGE);
            if (!menu)
                return;
            if (g_restorePending && g_leftSlots)
            {
                // Back from reading the slots again after a delete: the same slot.
                g_restorePending = false;
                PPCContext saved = ctx;
                ctx.r3.u64 = menu;
                ctx.r4.s64 = g_restoreIndex;
                sub_822F2E40(ctx, base);
                ctx = saved;
                Log("[saves] slot screen read again: selection back on slot %d", g_restoreIndex + 1);
            }
            int index = int(int8_t(base[menu + 152]));
            int slot = index >= 0 && index < SLOTS ? index + 1 : 0;
            std::string display;
            bool exists = slot && !g_refreshPending && xam::content::SavedGameExists(SlotFile(slot), &display);
            uint16_t pressed = TakePressed();
            MouseInput mouse = ReadMouse();
            HitZone zone{};
            bool onZone = mouse.on && ZoneAt(mouse.x, mouse.y, zone);

            if (!g_confirmOn)
            {
                if (exists && ((pressed & XINPUT_GAMEPAD_X) || (mouse.clicked && onZone && zone.zone == Zone::Prompt)))
                {
                    g_confirmSlot = slot;
                    g_choice = 0;  // NO, as the game's own questions
                    g_confirmOn = true;
                    PlaySound(ctx, base, BANK_FE, SOUND_SELECT);
                    Log("[saves] delete slot %d (%s): asking", slot, display.c_str());
                }
            }
            else
            {
                if (pressed & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT))
                {
                    g_choice ^= 1;
                    MoveSound(ctx, base, g_choice == 1);
                }
                if (mouse.moved && onZone && (zone.zone == Zone::No || zone.zone == Zone::Yes) && g_choice != (zone.zone == Zone::Yes))
                {
                    g_choice = zone.zone == Zone::Yes;
                    MoveSound(ctx, base, g_choice == 1);
                }
                bool accept = (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START)) != 0;
                bool back = (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) != 0;
                if (mouse.clicked && onZone)
                {
                    if (zone.zone == Zone::No || zone.zone == Zone::Yes)
                        g_choice = zone.zone == Zone::Yes, accept = true;
                    else if (zone.zone == Zone::Accept)
                        accept = true;
                    else if (zone.zone == Zone::Back)
                        back = true;
                }
                if (back || (accept && g_choice == 0))
                {
                    PlaySound(ctx, base, BANK_FE, SOUND_BACK);
                    CloseConfirm(back ? "cancelled (B)" : "cancelled (NO)");
                }
                else if (accept)
                {
                    int deleted = g_confirmSlot;
                    PlaySound(ctx, base, BANK_NAME, SOUND_ERASE);
                    if (DeleteSlot(ctx, base, deleted))
                    {
                        g_refreshPending = true;
                        g_restorePending = true;
                        g_leftSlots = false;
                        g_restoreIndex = deleted - 1;
                    }
                    CloseConfirm("YES");
                }
            }

            Visual visual;
            visual.portuguese = Portuguese();
            visual.slot = g_confirmOn ? g_confirmSlot : slot;
            if (g_confirmOn || exists)
            {
                if (g_confirmOn && display.empty())
                    xam::content::SavedGameExists(SlotFile(g_confirmSlot), &display);
                visual.saveName = Field(display, "Name:");
                visual.saveDate = Field(display, "Last Save:");
                visual.saveTime = Field(display, "Time:");
            }
            visual.mode = g_confirmOn ? Mode::Confirm : exists ? Mode::SlotPrompt : Mode::None;
            visual.choice = g_choice;
            Publish(visual);
        }
    }
}

// ---- hooks --------------------------------------------------------------------------------------

PPC_FUNC(sub_820B4A88)
{
    // Entering the name screen: the keyboard starts on the next frame (vf05).
    uint32_t action = ctx.r3.u32;
    __imp__sub_820B4A88(ctx, base);
    if (action == settings::save_screens::g_wheelAction)
        settings::save_screens::g_wheelAction = 0;  // the page's elements may be new
}

PPC_FUNC(sub_820B45A0)
{
    using namespace settings::save_screens;
    uint32_t action = ctx.r3.u32;
    __imp__sub_820B45A0(ctx, base);
    if (!Ok(base))
        return;
    uint32_t result = ctx.r3.u32;
    NameEntryFrame(ctx, base, action);
    ctx.r3.u64 = result;
}

PPC_FUNC(sub_820B43C0)
{
    using namespace settings::save_screens;
    uint32_t action = ctx.r3.u32;
    if (action == g_nameAction)
        StopKeyboard(g_done ? "DONE" : "left the screen");
    if (action == g_wheelAction)
        g_wheelAction = 0;
    __imp__sub_820B43C0(ctx, base);
}

PPC_FUNC(sub_820AEE90)
{
    using namespace settings::save_screens;
    uint32_t action = ctx.r3.u32;
    __imp__sub_820AEE90(ctx, base);
    if (!Ok(base))
        return;
    uint32_t result = ctx.r3.u32;
    SlotFrame(ctx, base, action);
    ctx.r3.u64 = result;
}

PPC_FUNC(sub_822959D8)
{
    settings::save_screens::g_drive = ctx.r3.u32;
    __imp__sub_822959D8(ctx, base);
}

namespace settings::save_screens
{
    void NoteFrontendNode(uint32_t node)
    {
        uint32_t previous = g_node.exchange(node);
        if (node == previous)
            return;
        if (previous == NODE_NAME_ENTRY)
            StopKeyboard("left the screen");
        if (previous == NODE_SLOTS)
        {
            CloseConfirm("the slot screen left");
            if (g_restorePending)
                g_leftSlots = true;
            else
                Publish({});
        }
        if (node != NODE_SLOTS && node != NODE_NAME_ENTRY)
        {
            std::lock_guard lock(g_mutex);
            g_visual = {};
        }
    }

    int32_t OverrideExit(uint32_t node, int32_t exit)
    {
        if (node != NODE_SLOTS || !g_refreshPending || exit >= 0)
            return exit;
        g_refreshPending = false;
        if (settings::QuitExit() != KNOWN_QUIT_EXIT)
        {
            Log("[saves] slot screen: this Frontend.bfig is not the one the runtime knows; the list updates the next time it opens");
            g_restorePending = false;
            return exit;
        }
        Log("[saves] slot screen: reading the slots again (exit %d)", EXIT_TO_SLOT_SCREEN);
        return EXIT_TO_SLOT_SCREEN;
    }

    void FilterGameInput(XINPUT_GAMEPAD& pad, bool poll)
    {
        constexpr int16_t STICK = 16000;
        uint16_t buttons = pad.wButtons;
        if (pad.sThumbLY > STICK) buttons |= XINPUT_GAMEPAD_DPAD_UP;
        if (pad.sThumbLY < -STICK) buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (pad.sThumbLX < -STICK) buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
        if (pad.sThumbLX > STICK) buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (poll)
        {
            uint16_t before = g_held.exchange(buttons);
            g_pressed |= uint16_t(buttons & ~before);
        }
        // The keyboard closes by itself if the game stops running its screen.
        if (g_keyboardOn && plat::TickCountMs() - g_keyboardTime.load() > 400)
            StopKeyboard("the screen is gone");
        if (g_confirmOn)
        {
            pad = {};
            return;
        }
        if (g_keyboardOn)
        {
            uint16_t back = pad.wButtons & XINPUT_GAMEPAD_B;  // the game's BACK (to the main menu)
            pad = {};
            pad.wButtons = back;
            return;
        }
        if (g_releasing)
        {
            if (buttons)
            {
                pad = {};
                return;
            }
            g_releasing = false;
        }
        if (g_node.load() == NODE_SLOTS)
            pad.wButtons &= ~XINPUT_GAMEPAD_X;  // our DELETE there
    }

    bool BlocksMenuMouse()
    {
        return g_confirmOn || g_keyboardOn;
    }
}

// ---- drawing (render thread) -------------------------------------------------------------------

namespace settings::save_screens
{
    namespace
    {
        struct View
        {
            float s, ox, oy;
            float X(float x) const { return ox + x * s; }
            float Y(float y) const { return oy + y * s; }
            int IX(float x) const { return int(std::lround(X(x))); }
            int IY(float y) const { return int(std::lround(Y(y))); }
        };

        // The frontend's colours on its cloth (settings_menu.cpp, CLOTH) and over a message frame
        // (DIMMED, the pause menu's).
        constexpr TextStyle KEY_NORMAL{ { 214, 120, 14, 255 }, { 70, 28, 0, 255 } };
        constexpr TextStyle KEY_SELECTED{ { 150, 218, 212, 255 }, { 30, 104, 110, 255 } };
        constexpr TextStyle KEY_OFF{ { 132, 84, 26, 255 }, { 84, 42, 2, 110 } };
        constexpr TextStyle MSG_TEXT{ { 238, 226, 196, 255 }, { 52, 28, 4, 255 } };
        constexpr TextStyle MSG_ITEM{ { 232, 160, 0, 255 }, { 104, 48, 8, 255 } };
        constexpr TextStyle MSG_SELECTED{ { 144, 216, 232, 255 }, { 24, 104, 128, 255 } };
        constexpr TextStyle MSG_WARNING{ { 255, 128, 72, 255 }, { 96, 24, 0, 255 } };
        constexpr Color PLATE{ 92, 44, 4, 70 }, PLATE_EDGE{ 70, 28, 0, 120 };
        constexpr Color PLATE_SELECTED{ 250, 236, 200, 120 }, PLATE_SELECTED_EDGE{ 30, 104, 110, 220 };
        constexpr Color FRAME{ 236, 222, 190, 190 }, DIM{ 14, 8, 0, 150 };

        menu_art::Canvas g_canvas;
        std::vector<uint32_t> g_pixels, g_faded;
        Visual g_drawn;
        uint32_t g_drawnStyle = 0;
        int g_drawnW = 0, g_drawnH = 0;
        uint64_t g_version = 0, g_fadedVersion = 0;
        uint64_t g_shownAt = 0;
        Mode g_shownMode = Mode::None;
        std::vector<HitZone> g_newZones;

        const char* T(bool pt, const char* en, const char* ptText)
        {
            return pt ? ptText : en;
        }

        float Width(FontId font, const std::string& text, float cell)
        {
            return menu_art::TextWidth(font, menu_art::Utf32(text), cell);
        }

        float Text(const View& v, FontId font, const std::string& text, float x, float y, float cell, const TextStyle& style, float align = 0.0f)
        {
            std::u32string u = menu_art::Utf32(text);
            float width = menu_art::TextWidth(font, u, cell);
            float left = x - width * align;
            menu_art::DrawGameText(g_canvas, font, u, v.X(left), v.Y(y), cell * v.s, style);
            return left + width;
        }

        // A rounded plate (the keys), antialiased; cached by size and colour.
        const menu_art::Image& Plate(int w, int h, int radius, Color fill, Color edge, int edgeWidth)
        {
            static std::map<std::tuple<int, int, int, uint32_t, uint32_t, int>, menu_art::Image> cache;
            auto pack = [](Color c) { return uint32_t(c.r) | uint32_t(c.g) << 8 | uint32_t(c.b) << 16 | uint32_t(c.a) << 24; };
            auto [it, inserted] = cache.try_emplace({ w, h, radius, pack(fill), pack(edge), edgeWidth });
            menu_art::Image& image = it->second;
            if (!inserted)
                return image;
            image.w = w, image.h = h;
            image.px.assign(size_t(w) * h, 0);
            auto distance = [&](float x, float y) {
                // Signed distance to the rounded rectangle's border (negative inside).
                float qx = std::fabs(x - w * 0.5f) - (w * 0.5f - radius), qy = std::fabs(y - h * 0.5f) - (h * 0.5f - radius);
                float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
                return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
            };
            for (int y = 0; y < h; y++)
            {
                for (int x = 0; x < w; x++)
                {
                    float d = distance(x + 0.5f, y + 0.5f);
                    float cover = std::clamp(0.5f - d, 0.0f, 1.0f);
                    if (cover <= 0.0f)
                        continue;
                    float t = std::clamp(-d - edgeWidth + 0.5f, 0.0f, 1.0f);  // 0 on the edge, 1 inside
                    float a = (edge.a * (1.0f - t) + fill.a * t) / 255.0f * cover;
                    float r = edge.r * (1.0f - t) + fill.r * t, g = edge.g * (1.0f - t) + fill.g * t, b = edge.b * (1.0f - t) + fill.b * t;
                    auto p = [&](float c) { return uint32_t(std::lround(c * a)); };
                    image.px[size_t(y) * w + x] = p(r) | p(g) << 8 | p(b) << 16 | uint32_t(std::lround(a * 255.0f)) << 24;
                }
            }
            return image;
        }

        void AddZone(float x0, float y0, float x1, float y1, Zone zone, int index = 0)
        {
            g_newZones.push_back({ x0, y0, x1, y1, zone, index });
        }

        void DrawKeyboard(const View& v, const Visual& vis)
        {
            const std::vector<Key>& keys = Keys();
            bool full = vis.length >= NAME_MAX, empty = vis.length == 0;
            for (size_t i = 0; i < keys.size(); i++)
            {
                const Key& key = keys[i];
                bool selected = int(i) == vis.key;
                int px = v.IX(key.x), py = v.IY(key.y), pw = v.IX(key.x + key.w) - px, ph = v.IY(key.y + key.h) - py;
                int radius = std::max(2, int(std::lround(9.0f * v.s))), edge = std::max(1, int(std::lround(2.0f * v.s)));
                g_canvas.Blit(Plate(pw, ph, radius, selected ? PLATE_SELECTED : PLATE, selected ? PLATE_SELECTED_EDGE : PLATE_EDGE, edge), px, py);
                AddZone(key.x - 3.0f, key.y - 3.0f, key.x + key.w + 3.0f, key.y + key.h + 3.0f, Zone::Key, int(i));
                bool off = key.c == KEY_ERASE ? empty : key.c == KEY_DONE ? empty : full;
                const TextStyle& style = selected ? KEY_SELECTED : off ? KEY_OFF : KEY_NORMAL;
                std::string label;
                float cell;
                if (key.c == KEY_ERASE || key.c == KEY_DONE)
                {
                    // The game's own words for X and Y on this screen (ERASE / DONE).
                    label = key.c == KEY_ERASE ? T(vis.portuguese, "ERASE", "APAGAR") : T(vis.portuguese, "DONE", "PRONTO");
                    cell = selected ? 34.0f : 30.0f;
                }
                else
                {
                    label = std::string(1, char(key.c));
                    cell = selected ? 44.0f : 38.0f;
                }
                Text(v, FontId::Large, label, key.x + key.w * 0.5f, key.y + (key.h - cell) * 0.5f + 1.0f, cell, style, 0.5f);
            }
        }

        // The prompt on the slot screen: X DELETE, on the line of the game's B BACK / SELECT A.
        void DrawPrompt(const View& v, const Visual& vis)
        {
            // The game's B BACK / SELECT A here are ~46 high, centred on y 643.
            constexpr float Y = 620.0f, CELL = 46.0f;
            std::string text = std::string("\xC2\xB3 ") + T(vis.portuguese, "DELETE", "APAGAR");
            float w = Width(FontId::Large, text, CELL);
            Text(v, FontId::Large, text, VIRTUAL_W * 0.5f, Y, CELL, KEY_NORMAL, 0.5f);
            AddZone(VIRTUAL_W * 0.5f - w * 0.5f - 10.0f, Y - 8.0f, VIRTUAL_W * 0.5f + w * 0.5f + 10.0f, Y + CELL + 8.0f, Zone::Prompt);
        }

        // MSG_Background (the top-left corner of the game's message boxes) in the four corners.
        void DrawFrame(const View& v, float x0, float y0, float x1, float y1)
        {
            const menu_art::Image* corner = menu_art::GetArt(menu_art::Art::MessageFrame);
            int left = v.IX(x0), top = v.IY(y0), right = v.IX(x1), bottom = v.IY(y1);
            if (!corner)
            {
                g_canvas.Fill(left, top, right, bottom, { 0, 0, 0, 190 });
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
                const menu_art::Image& c = menu_art::Scaled(*corner, 0, 0, curl, corner->h, curlW, halfH, flipX, flipY);
                const menu_art::Image& edge = menu_art::Scaled(*corner, curl, 0, corner->w - curl, corner->h, halfW - curlW, halfH, flipX, flipY);
                g_canvas.Blit(c, flipX ? qx + halfW - curlW : qx, qy, FRAME);
                g_canvas.Blit(edge, flipX ? qx : qx + curlW, qy, FRAME);
            }
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

        // "Delete this saved game?" in a message frame, NO / YES like the game's own questions.
        void DrawConfirm(const View& v, const Visual& vis, int width, int height)
        {
            g_canvas.Fill(0, 0, width, height, DIM);
            constexpr float X0 = 260.0f, X1 = 1020.0f, CELL = 30.0f, LINE = 36.0f;
            std::vector<std::string> question = Wrap(T(vis.portuguese, "Are you sure you want to delete this saved game?",
                                                       "Tem certeza de que deseja apagar este jogo salvo?"),
                                                     FontId::Small, CELL, X1 - X0 - 100.0f);
            // The frame fits the lines: question, the save, the warning, NO / YES and the hints,
            // centred a little above the middle of the screen (over the slots' pictures).
            const float contentH = 40.0f + LINE * question.size() + 4.0f + 44.0f + 40.0f + 2 * 44.0f + 16.0f + 28.0f + 52.0f;
            const float Y0 = std::round(350.0f - contentH * 0.5f), Y1 = Y0 + contentH;
            DrawFrame(v, X0, Y0, X1, Y1);
            const float cx = VIRTUAL_W * 0.5f;
            float y = Y0 + 40.0f;
            for (const std::string& line : question)
            {
                Text(v, FontId::Small, line, cx, y, CELL, MSG_TEXT, 0.5f);
                y += LINE;
            }
            // The save: slot, name, date and play time, as the slot screen shows them.
            std::string save = std::string(T(vis.portuguese, "SLOT ", "ESPAÇO ")) + std::to_string(vis.slot);
            for (const std::string* part : { &vis.saveName, &vis.saveDate, &vis.saveTime })
            {
                if (!part->empty())
                    save += "   " + *part;
            }
            y += 4.0f;
            Text(v, FontId::Large, save, cx, y, 34.0f, MSG_SELECTED, 0.5f);
            y += 44.0f;
            Text(v, FontId::Small, T(vis.portuguese, "It cannot be recovered.", "Não será possível recuperá-lo."), cx, y, 26.0f, MSG_WARNING, 0.5f);

            // NO / YES (NÃO / SIM), the highlighted one in the selected colours.
            const float itemsY = y + 40.0f, itemCell = 40.0f, itemPitch = 44.0f;
            const char* items[2] = { T(vis.portuguese, "NO", "NÃO"), T(vis.portuguese, "YES", "SIM") };
            for (int i = 0; i < 2; i++)
            {
                bool selected = vis.choice == i;
                float cell = selected ? itemCell : itemCell - 4.0f;
                float iy = itemsY + i * itemPitch + (itemCell - cell) * 0.5f;
                float w = Width(FontId::Large, items[i], cell);
                Text(v, FontId::Large, items[i], cx, iy, cell, selected ? MSG_SELECTED : MSG_ITEM, 0.5f);
                AddZone(cx - std::max(w, 90.0f) * 0.5f - 16.0f, itemsY + i * itemPitch - 4.0f, cx + std::max(w, 90.0f) * 0.5f + 16.0f,
                      itemsY + i * itemPitch + itemPitch - 4.0f, i ? Zone::Yes : Zone::No);
            }

            // (B) BACK ... SELECT (A), as the game's hints (with the device's icons).
            const float hintY = itemsY + 2 * itemPitch + 16.0f, hintCell = 28.0f;
            std::string back = std::string("\xC2\xB2 ") + T(vis.portuguese, "BACK", "VOLTAR");
            std::string select = std::string(T(vis.portuguese, "SELECT", "SELECIONAR")) + " \xC2\xB1";
            // Inside the frame's bottom curls.
            float backEnd = Text(v, FontId::Large, back, X0 + 96.0f, hintY, hintCell, KEY_NORMAL);
            float selectW = Width(FontId::Large, select, hintCell);
            Text(v, FontId::Large, select, X1 - 96.0f, hintY, hintCell, KEY_NORMAL, 1.0f);
            AddZone(X0 + 86.0f, hintY - 6.0f, backEnd + 10.0f, hintY + hintCell + 6.0f, Zone::Back);
            AddZone(X1 - 106.0f - selectW, hintY - 6.0f, X1 - 86.0f, hintY + hintCell + 6.0f, Zone::Accept);
        }

        void Rasterize(const Visual& vis, int width, int height)
        {
            View v;
            v.s = std::min(width / VIRTUAL_W, height / VIRTUAL_H);
            v.ox = (width - VIRTUAL_W * v.s) * 0.5f, v.oy = (height - VIRTUAL_H * v.s) * 0.5f;
            g_canvas.Reset(width, height);
            g_newZones.clear();
            switch (vis.mode)
            {
            case Mode::Keyboard: DrawKeyboard(v, vis); break;
            case Mode::SlotPrompt: DrawPrompt(v, vis); break;
            case Mode::Confirm: DrawConfirm(v, vis, width, height); break;
            case Mode::None: break;
            }
            std::lock_guard lock(g_mutex);
            if (g_visual.mode == vis.mode)
                g_zones = g_newZones;
        }
    }

    void Draw(uint32_t width, uint32_t height)
    {
        Visual vis;
        {
            std::lock_guard lock(g_mutex);
            // A screen the game stopped running (a cut to another screen) is not drawn.
            if (g_visual.mode != Mode::None && plat::TickCountMs() - g_visualTime > 500)
                g_visual = {};
            vis = g_visual;
        }
        uint64_t now = plat::TickCountMs();
        if (vis.mode != g_shownMode)
        {
            // Fade in when the keyboard or the question appears (the prompt follows the selection).
            bool fade = vis.mode == Mode::Keyboard || vis.mode == Mode::Confirm || g_shownMode == Mode::None;
            g_shownMode = vis.mode;
            g_shownAt = fade ? now : 0;
        }
        if (vis.mode == Mode::None || !width || !height || !menu_art::Ready(0))
            return;
        uint32_t style = prompts::Style();
        if (int(width) != g_drawnW || int(height) != g_drawnH)
        {
            g_drawnW = int(width), g_drawnH = int(height);
            g_drawn = {};
        }
        if (!(vis == g_drawn) || style != g_drawnStyle)
        {
            Rasterize(vis, int(width), int(height));
            g_canvas.Export(g_pixels);
            g_drawn = vis;
            g_drawnStyle = style;
            g_version++;
        }
        constexpr float FADE_MS = 160.0f;
        float t = g_shownAt ? std::clamp(float(now - g_shownAt) / FADE_MS, 0.0f, 1.0f) : 1.0f;
        const uint32_t* pixels = g_pixels.data();
        uint64_t version = g_version;
        if (t < 1.0f)
        {
            uint32_t k = uint32_t(std::lround(t * 256.0f));
            g_faded.resize(g_pixels.size());
            for (size_t i = 0; i < g_pixels.size(); i++)
                g_faded[i] = (g_pixels[i] & 0x00FFFFFFu) | (((g_pixels[i] >> 24) * k >> 8) << 24);
            pixels = g_faded.data();
            version = (1ull << 63) | ++g_fadedVersion;
        }
        gpu::overlay::DrawBitmap(width, height, pixels, int(width), int(height), 0, 0, version, gpu::overlay::BITMAP_SAVE_SCREENS);
    }
}
