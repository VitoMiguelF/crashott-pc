#pragma once

// The main menu's MULTIPLAYER (docs/internals/multiplayer.md, "Lobby"; the item: settings/pc_frontend.cpp).
// It goes the SETTINGS way (frontend_hooks.cpp): the game plays its transition to the emptied PS2
// options screen and these screens are drawn over its cloth, like the settings (settings_menu.h):
//
//   MULTIPLAYER   NAME / HOST GAME / FIND GAME
//   YOUR NAME     the player's name online, saved in settings.toml ([multiplayer] name); shown first the
//                 first time (no saved name, no CRASH_MP_NAME)
//   HOST GAME     NEW GAME / LOAD GAME: opens the session (mp_lobby.h) and leaves through the main
//                 menu's own exit (88 / 89); a new game goes from its last screen to the HUB (exit 309, the
//                 loaded game's ExitToHUB: EndAccess + FadeIn) instead of the first level, a loaded one
//                 gets there by itself
//   FIND GAME     SERVER LIST (the LAN, every second) / ENTER CODE; a password when the host asks
//   LOBBY         a client's wait: the host, the players and their ping, until the host starts a level;
//                 then StartLevel(that level) + SkipToLevel (51) from where the client is
//
// In a level the game never pauses in a session (PauseGame(true) skipped): the host's pause shows over
// the running level, and a client's START opens its own menu instead (QUIT TO THE MAIN MENU only).
// Leaving a level: DROP OUT is EXIT LEVEL online. A client never saves (the save screens' exits go
// to the HUB without saving), and from the HUB goes on to the main menu: its own EXIT LEVEL (or a
// closed session) ends there, the host's EXIT LEVEL (or the host back at its HUB) opens the lobby again.
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
    // The SETTINGS marker opens these screens (at the lobby): a client back from the host's level.
    bool ReopensLobby();

    // The game must see a neutral controller (a screen open, or its buttons still held).
    bool BlocksGameInput();
    // A screen of these is open (also over a level: a client's START menu): the touch is the mouse there,
    // not the on-screen controls (settings/touch_controls.cpp).
    bool TakesTouch();
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

    // SKIN AND COLOR (the host's lobby, a client's wait): the game's own skin screen (SkinSelectionScreen,
    // node 1058, with its 3D preview) opened from the lobby. While it shows, every wearable skin but
    // Crash_Hulk is there (dev/dev_skins.cpp's next / previous and unlocked hooks), and the preview wears
    // this player's colour (mp_colors.cpp): PreviewColour, -1 when it does not show.
    bool PickerOpen();
    int PreviewColour();
}
