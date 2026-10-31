#pragma once

// Discord Rich Presence (docs/GUIA_DO_PROJETO.md "Discord"): the player's Discord profile shows the
// game, where the player is (main menu, the HUB, the episode by its name), the mode (solo, local co-op, an
// online MULTIPLAYER session with its player count) and the time played since the game started; friends can
// join an online session from Discord (discord_presence.cpp). No SDK: the local IPC of
// the Discord client (discord_ipc.h), its handshake and SET_ACTIVITY frames in JSON, on a thread of its
// own. Without a Discord client running it does nothing (it looks again every 15 s); off with
// SETTINGS > GAME > DISCORD STATUS (game.discord = false), live. Not on Android (no IPC in the mobile app).
//
//   CRASH_DISCORD_APP_ID=<id>   another Discord application's id (discord.com/developers) than
//                               APPLICATION_ID in discord_presence.cpp
//   CRASH_DISCORD_ASSET=<name>  the application's art asset shown as the large image ("crashrecomp")
//   CRASH_DISCORD_JOIN=<code>   tests: as a friend's "Join" with that invite code (the main menu joins it)

#include <cstdint>

namespace discord
{
    // Starts the presence thread (main.cpp, after the language pack: the episode titles).
    void Init();
    // The Frontend tree node shown now (settings/frontend_hooks.cpp, game thread): the main menu / the HUB.
    void NoteFrontendNode(uint32_t node);
}
