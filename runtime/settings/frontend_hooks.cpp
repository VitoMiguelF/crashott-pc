// Guest hook that opens the settings screen, and closes the game, from the game's main menu.
//
// sub_822E29D8 is the generic Lua -> C++ method thunk of the game's script bindings (its upvalues
// hold the object and the member pointer; r3 = lua_State*). The patched ExitCalibration snippet
// (frontend_patches.cpp) calls Managers.FrontendManager:PlaySoundSelect(4242), and the exit of the
// EXIT GAME item (pc_frontend.cpp) PlaySoundSelect(4343): the method ignores the extra argument,
// and the thunk sees it on the Lua stack. This game's Lua 5.1 keeps L->top at +8 and L->base at
// +12, and stack slots are 8-byte TValues {u32 type, u32 value} (type 3 = number, a float).

#include "direct_boot.h"
#include "pc_frontend.h"
#include "settings.h"
#include "quit_game.h"
#include "save_screens.h"
#include "settings_menu.h"
#include "../dev/dev_console.h"
#include "../discord/discord_presence.h"
#include "../online/mp_menu.h"
#include "../kernel/kernel.h"
#include "../kernel/pc_input.h"
#include "../kernel/memory.h"

#include <atomic>
#include <cstring>

extern "C" PPC_FUNC(__imp__sub_822E29D8);

namespace
{
    constexpr uint32_t LUA_TNUMBER = 3;
    constexpr float SETTINGS_MARKER = 4242.0f;
    constexpr float QUIT_MARKER = 4343.0f;

    // The number passed after `self`, when the call has exactly one argument and it is a number.
    bool MarkerArgument(uint32_t L, float& value)
    {
        uint32_t top = LoadBE32(L + 8), base = LoadBE32(L + 12);
        if (top != base + 16)  // self + one argument
            return false;
        if (LoadBE32(base + 8) != LUA_TNUMBER)
            return false;
        uint32_t bits = LoadBE32(base + 12);
        memcpy(&value, &bits, 4);
        return true;
    }
}

// The Frontend fight tree's conditions (pc_frontend.cpp): sub_820F9BA8(tree, node, ...) returns the
// number of the exit to take from the node, or -1; its main menu case calls GetMenuIndex
// (sub_820F9A10(tree, page name, menu name)) for items 0 to 3 while A is pressed, so item 4 falls
// through to -1. Here, a -1 after GetMenuIndex said 4 on the main menu becomes the EXIT GAME exit,
// and the exits of Y (storage device) become -1.
extern "C" PPC_FUNC(__imp__sub_820F9BA8);
extern "C" PPC_FUNC(__imp__sub_820F9A10);

namespace
{
    thread_local int t_conditionDepth = 0;
    thread_local bool t_quitItemChosen = false;
    thread_local bool t_multiplayerItemChosen = false;
    // MULTIPLAYER took the SETTINGS exit: its marker opens the MULTIPLAYER screens instead.
    std::atomic<bool> g_multiplayerPending{ false };

    bool GuestStringIs(uint8_t* base, uint32_t address, const char* text)
    {
        return address && strcmp(reinterpret_cast<const char*>(base + address), text) == 0;
    }
}

// With MULTIPLAYER (index 2, pc_frontend.cpp) the main menu has six items: the game's conditions
// see the old indexes (0-1 as they are, 3-5 as 2-4) and MULTIPLAYER as SETTINGS (3).
PPC_FUNC(sub_820F9A10)
{
    uint32_t page = ctx.r4.u32, menu = ctx.r5.u32;
    __imp__sub_820F9A10(ctx, base);
    if (!GuestStringIs(base, page, "GameStart_Start_Xenon") || !GuestStringIs(base, menu, "StartMenu"))
        return;
    int32_t index = int32_t(ctx.r3.u32);
    if (settings::MultiplayerItemAdded() && index >= settings::MULTIPLAYER_MENU_INDEX)
    {
        if (index == settings::MULTIPLAYER_MENU_INDEX)
        {
            if (t_conditionDepth > 0)
                t_multiplayerItemChosen = true;
            index = settings::QUIT_MENU_INDEX - 1;
        }
        else
            index--;
        ctx.r3.s64 = index;
    }
    if (t_conditionDepth > 0 && index == settings::QUIT_MENU_INDEX && !t_multiplayerItemChosen)
        t_quitItemChosen = true;
}

PPC_FUNC(sub_820F9BA8)
{
    uint32_t tree = ctx.r3.u32, node = ctx.r4.u32;
    if (t_conditionDepth == 0)
    {
        settings::menu::NoteFrontendNode(node);  // the screen shown (the pause menu's OPTIONS opens the settings)
        input::pc::NoteFrontendNode(node);       // menus vs gameplay for the keyboard and the mouse
        settings::MenuAutopilotNode(node);       // CRASH_MENU_AUTOPILOT
        boot::NoteFrontendNode(node);            // test events (direct_boot.h)
        settings::save_screens::NoteFrontendNode(node);  // name keyboard, delete on the slot screen
        discord::NoteFrontendNode(node);                 // Discord Rich Presence: main menu / HUB
    }
    if (t_conditionDepth++ == 0)
        t_quitItemChosen = t_multiplayerItemChosen = false;
    __imp__sub_820F9BA8(ctx, base);
    int32_t exit = int32_t(ctx.r3.u32);
    if (settings::IsStorageDeviceExit(exit))
    {
        Log("[pc] frontend: Y (storage device) ignored (exit %d)", exit);
        exit = -1;
    }
    else if (exit == -1 && t_conditionDepth == 1 && t_quitItemChosen)
    {
        exit = settings::QuitExit();
        if (exit >= 0)
            Log("[pc] frontend: EXIT GAME chosen on the main menu (exit %d)", exit);
    }
    else if (exit >= 0 && t_conditionDepth == 1 && t_multiplayerItemChosen)
    {
        g_multiplayerPending = true;
        Log("[pc] frontend: MULTIPLAYER chosen on the main menu (exit %d)", exit);
    }
    if (t_conditionDepth == 1)
        exit = boot::OverrideExit(ctx, base, tree, node, exit);  // --skip-intros / CRASH_BOOT
    if (t_conditionDepth == 1)
        exit = settings::save_screens::OverrideExit(node, exit);  // the slots read again after a delete
    if (t_conditionDepth == 1)
        exit = dev::console::OverrideExit(ctx, base, tree, node, exit);  // the console's "level" from the HUB / main menu
    if (t_conditionDepth == 1)
        exit = mp::menu::OverrideExit(ctx, base, tree, node, exit);  // MULTIPLAYER: host / client flows
    ctx.r3.s64 = exit;
    t_conditionDepth--;
}

PPC_FUNC(sub_822E29D8)
{
    float marker;
    if (MarkerArgument(ctx.r3.u32, marker))
    {
        if (marker == SETTINGS_MARKER && (g_multiplayerPending.exchange(false) || mp::menu::ReopensLobby()))
        {
            Log("[mp] main menu MULTIPLAYER selected");
            mp::menu::RequestOpen();
        }
        else if (marker == SETTINGS_MARKER)
        {
            Log("[settings] main menu SETTINGS selected (lr=0x%08X)", GuestLR(ctx));
            settings::menu::RequestOpen();
        }
        else if (marker == QUIT_MARKER)
        {
            __imp__sub_822E29D8(ctx, base);  // the select sound
            settings::RequestQuit();
            return;
        }
    }
    __imp__sub_822E29D8(ctx, base);
}
