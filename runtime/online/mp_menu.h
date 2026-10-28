#pragma once

// The main menu's MULTIPLAYER (docs/internals/multiplayer.md, "Lobby"; the item: settings/pc_frontend.cpp).
// It goes the SETTINGS way (frontend_hooks.cpp): the game plays its transition to the emptied PS2
// options screen and these screens are drawn over its cloth, like the settings (settings_menu.h):
//
//   MULTIPLAYER   HOST GAME / FIND GAME
//   HOST GAME     NEW GAME / LOAD GAME: opens the session (mp_lobby.h) and leaves through the main
//                 menu's own exit (88 / 89); a new game goes from its last screen to the HUB (exit 309, the
//                 loaded game's ExitToHUB: EndAccess + FadeIn) instead of the first level, a loaded one
//                 gets there by itself
//   FIND GAME     SERVER LIST (the LAN, every second) / ENTER CODE; a password when the host asks
//   LOBBY         a client's wait: the host, the players and their ping, until the host starts a level;
//                 then StartLevel(that level) + SkipToLevel (51) from where the client is
//
// At the HUB the host sees a panel with the code and the players; BACK opens the lobby there: kick a
// player, the password, the maximum of players, the knock-out time, close the session. The host's
// level start (InGame, node 630, from the HUB) is what the clients follow.

#include <cstdint>

struct PPCContext;

namespace mp::menu
{
    // Game thread (the marker hook, frontend_hooks.cpp): MULTIPLAYER chosen on the main menu.
    void RequestOpen();

    // The game must see a neutral controller (a screen open, or its buttons still held).
    bool BlocksGameInput();
    // Pressed for the game on top: B for a moment when the screens close back to the main menu.
    uint16_t GameButtons();
    // Hidden from the game: BACK while the HUB panel shows (it opens the lobby).
    uint16_t HiddenButtons();

    // Game thread, from the frontend's conditions (sub_820F9BA8, the screen shown): the exits these
    // screens take (host new / load game, a client's level start), the new game's way to the HUB and
    // the host's level start / return.
    int32_t OverrideExit(PPCContext& ctx, uint8_t* base, uint32_t tree, uint32_t node, int32_t exit);

    // Render thread, once per presented frame (gpu/xenos_renderer.cpp): input and drawing.
    void Draw(uint32_t width, uint32_t height);
}
