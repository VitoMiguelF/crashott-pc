// The game's own options, the ones its pause menu's OPTIONS screen had (that screen is now covered
// by the settings screen, settings_menu.h): CO-OP mode, INVERT AXIS and AUTOSAVE. The settings
// keep them (settings.toml [game]) and this file applies them to the game on every read of the
// controller, so they hold from the main menu on and also over what a loaded save says
// (CFrontendManager::CSaveGameHandler::vf02, sub_821D9AA8, reads them from the save: three volumes,
// then a flags word with bit 0 = invert axis, bit 1 = co-op mode, bit 2 = autosave).
//
// Where they are (found with CRASH_MEMDIFF_TIMES / CRASH_WATCH, runtime/NOTES.md):
//   - co-op mode: the byte at 0x8245FA99 (set by sub_82152C60): 1 = LEAPFROG (the game's
//     default), 0 = PIGGYBACK.
//   - invert axis: bit 0x02 of the byte at +16652 of the CFrontendManager (manager 6 of the
//     managers object at 0x824A7A24, sub_821FB4F0 / sub_821FA440: word at +(index + 7) * 4); the
//     camera (sub_8221B298) reads it every frame and negates the stick.
//   - autosave: bit 0x10 of the byte at +100 of the save game manager (manager 13); the game turns
//     it on when a save slot is chosen and off when playing without saving (Frontend.bfig,
//     SaveGameManager:SetAutoSaveGame, sub_821D0750, which also sets the slot index at +99 to -1
//     when switching off). Here "off" clears the bit (leaving the slot index alone) and "on" puts
//     back a bit this file cleared, only while the slot index is valid: never autosave without a
//     slot the player chose.

#include "settings.h"
#include "../diagnostics.h"
#include "../kernel/memory.h"

namespace settings
{
    namespace
    {
        constexpr uint32_t COOP_MODE = 0x8245FA99;
        constexpr uint32_t MANAGERS = 0x824A7A24;
        constexpr uint32_t FRONTEND_MANAGER = MANAGERS + (6 + 7) * 4;
        constexpr uint32_t SAVE_GAME_MANAGER = MANAGERS + (13 + 7) * 4;
        constexpr uint32_t FRONTEND_FLAGS = 16652;
        constexpr uint8_t INVERT_AXIS_BIT = 0x02;
        constexpr uint32_t SAVE_FLAGS = 100, SAVE_SLOT = 99;
        constexpr uint8_t AUTOSAVE_BIT = 0x10;

        bool g_autosaveCleared = false;  // the bit was cleared here (setting off), not by the game

        uint8_t LoadByte(uint32_t guest) { return *g_memory.Translate(guest); }
        void StoreByte(uint32_t guest, uint8_t value) { *g_memory.Translate(guest) = value; }

        // A manager pointer that looks like one (in the title's heaps or physical memory).
        uint32_t Manager(uint32_t slot)
        {
            uint32_t pointer = LoadBE32(slot);
            return (pointer >= 0x40000000 && pointer < 0x7F000000) || pointer >= 0xA0000000 ? pointer : 0;
        }
    }

    void GameOptionsTick()
    {
        Values v = Get();

        uint8_t coop = v.coopLeapfrog ? 1 : 0;
        if (LoadByte(COOP_MODE) != coop)
        {
            Log("[settings] game option: co-op mode %s (was %u)", v.coopLeapfrog ? "leapfrog" : "piggyback", LoadByte(COOP_MODE));
            StoreByte(COOP_MODE, coop);
        }

        if (uint32_t frontend = Manager(FRONTEND_MANAGER))
        {
            uint8_t flags = LoadByte(frontend + FRONTEND_FLAGS);
            uint8_t wanted = v.invertAxis ? flags | INVERT_AXIS_BIT : flags & ~INVERT_AXIS_BIT;
            if (wanted != flags)
            {
                Log("[settings] game option: invert axis %s", v.invertAxis ? "on" : "off");
                StoreByte(frontend + FRONTEND_FLAGS, wanted);
            }
        }

        if (uint32_t save = Manager(SAVE_GAME_MANAGER))
        {
            uint8_t flags = LoadByte(save + SAVE_FLAGS);
            bool on = flags & AUTOSAVE_BIT;
            if (!v.autosave && on)
            {
                StoreByte(save + SAVE_FLAGS, flags & ~AUTOSAVE_BIT);
                g_autosaveCleared = true;
                Log("[settings] game option: autosave off");
            }
            else if (v.autosave && !on && g_autosaveCleared)
            {
                g_autosaveCleared = false;
                if (LoadByte(save + SAVE_SLOT) != 0xFF)
                {
                    StoreByte(save + SAVE_FLAGS, flags | AUTOSAVE_BIT);
                    Log("[settings] game option: autosave on again (slot %u)", LoadByte(save + SAVE_SLOT));
                }
            }
            else if (on)
            {
                g_autosaveCleared = false;  // the game turned it on itself (a slot was chosen)
            }
        }
    }
}
