#pragma once

// Online multiplayer prototype (docs/internals/multiplayer.md). Everything is off unless CRASH_MP=1 or
// a session is opened from the main menu's MULTIPLAYER (mp_lobby.h); with it off the game runs unchanged.
//
//   CRASH_MP=1                 turns the prototype on
//   CRASH_MP_STREAM=<mode>     vanilla | union | all: which rooms stay loaded (mp_stream.cpp; a
//                              MULTIPLAYER session: union unless set)
//   CRASH_MP_ANCHOR=x,y,z;...  positions that count as players for the room choice (mp_stream.cpp)
//   CRASH_MP_TRACE=1|<ms>      rooms + players once a second (or every <ms>), and every player that
//                              moves more than 10 units in one frame ("[mp] stream ...", "[mp] actor ...")
//   CRASH_MP_ROLE=<role>       host | client: the M1 mirror over UDP (mp_net.cpp)
//   CRASH_MP_PORT=<n>          UDP port (default 47810); CRASH_MP_PEER=<ip> the host for a client
//   CRASH_MP_HZ=<n>            snapshots per second sent by the host (default 30)
//   CRASH_MP_PLAYERS=<n>       2-8 players; with more than 2, players 3-8 are spawned on every machine
//                              (mp_players.cpp). Past 4 (the game's own limit: controllers, AI targets)
//                              it is experimental: players 5-8 get controllers of ours and take turns
//                              as the AI's 4 targets
//   CRASH_MP_SLOT=<n>          client: its player, 1-7 (default 1); the host is 0
//   CRASH_MP_SNAP=<units>      a remote player further than this from where its machine says is put back (1.0)
//   CRASH_MP_TELEPORT=x,y,z    host, tests: both players put there once they are on foot
//   CRASH_MP_LEASH=<units>     how far apart the players may go (default 0 = no limit; mp_net.cpp)

#include <cstdint>
#include <string>
#include <vector>

struct PPCContext;

namespace mp
{
    enum class StreamMode { Vanilla, Union, All };

    // Players in a session: up to 8. The game itself holds 4 (4 controllers, 4 AI targets); players 5-8
    // are experimental (mp_players.cpp: controllers 4-7 of ours, AI targets taken in turns).
    constexpr uint32_t MAX_PLAYERS = 8;
    constexpr uint32_t GAME_PLAYERS = 4;

    bool Enabled();  // CRASH_MP=1, or a MULTIPLAYER session open
    bool Online();  // host or client (CRASH_MP_ROLE or a session): the level starts with every player in play
    uint32_t Players();  // a session's player count (1-8), else CRASH_MP_PLAYERS (2-8)
    // mp_lobby.cpp: a session opened from the menu turns the prototype on (and off when it closes) and
    // sets the player count the next level starts with.
    void SetSession(bool open, uint32_t players);
    StreamMode Stream();
    bool Trace();
    uint32_t TraceIntervalMs();  // CRASH_MP_TRACE=<ms> (1 = 1000)
    uint64_t NowMs();

    // The player actors: CGameDatabase +16 / +20 (the game's two players) and the extra Crashes
    // the prototype added (AddPlayer).
    std::vector<uint32_t> PlayerActors();
    // A level is up: the stream manager has a current room and player 1 exists.
    bool InLevel();
    void AddPlayer(uint32_t actor);
    // CRASH_MP_ANCHOR positions (guest vec3 addresses): stand-ins for remote players in the room check.
    const std::vector<uint32_t>& Anchors();

    // Per-frame work on the game thread (called from the CStreamManager::Update hook).
    void Tick(PPCContext& ctx, uint8_t* base);

    // mp_stream.cpp
    uint32_t StreamManager();
    uint32_t RoomAt(PPCContext& ctx, uint8_t* base, uint32_t position);  // room holding a position
    std::string RoomLabel(uint32_t room);

    // mp_players.cpp: the Crashes beyond the game's two (player index >= 2, DO_SpawnPlayer(3..8)); each
    // one is put on foot next to player 1 (PlayersTick, every frame on the game thread).
    std::vector<uint32_t> ExtraCrashes();
    uint32_t PlayerCrash(uint32_t index);  // the Crash of player index 0-7 (0 if none)
    bool PlayerOnFoot(uint32_t index);     // in play: co-op state 2 and, for players 2-8, already shown
    // Knocked out (lying down until it comes back next to a teammate) or back for less than 1.5 s: its
    // hitpoints are this machine's (mp_players.cpp).
    bool HitpointsHeld(uint32_t index);
    // mp_net.cpp: the player's slot in the game (0 = the host); offline the index itself.
    uint32_t PlayerSlot(uint32_t index);
    // The player's hitpoints (-1 while AdjustHitpoints has not shown its damageable).
    float PlayerHitpoints(uint32_t index);
    float PlayerMaxHitpoints(uint32_t index);
    void SetPlayerMaxHitpoints(uint32_t index, float value);  // and the hitpoints down to it
    void PlayersTick(PPCContext& ctx, uint8_t* base);
    // A cutscene ended (settings/skip_cinematics.cpp): online, shortly after, every player is put next to
    // the host's (the game places players 1 and 2; players 3-8 would stay where they were).
    void CinematicDone();
    // A player who left the session: hidden, without collision nor AI target, out of play (on every machine).
    void RemovePlayer(PPCContext& ctx, uint8_t* base, uint32_t index);
    // Online (or more than two players): nobody enters another player's backpack.
    bool NoBackpack();

    // mp_skins.cpp: every player in its own skin (the game has one, the current skin's template hash).
    namespace skins
    {
        uint32_t Local();  // this machine's skin (its template hash)
        // The lobby (any thread): each slot's skin hash (0 = none), from the roster.
        void SetSlotSkins(const uint32_t* skins, uint32_t count);
        void ClearSlotSkins();
        uint32_t SlotSkin(uint32_t slot);
        // dev/dev_skins.cpp's CStreamManager hooks: the other players' templates come and go with a level.
        void LevelLoad(PPCContext& ctx, uint8_t* base);
        void LevelUnload(PPCContext& ctx, uint8_t* base);
    }

    // mp_net.cpp
    void NetTick(PPCContext& ctx, uint8_t* base);
    // The UDP socket, opened by CRASH_MP_ROLE at startup or by the lobby (mp_lobby.cpp). One I/O thread
    // owns its receiving: lobby packets ("CML1") go to lobby::OnPacket on that thread, the rest is queued
    // for the game thread. Addresses are IPv4, host byte order.
    namespace net
    {
        enum class Role { None, Host, Client };
        bool Open(int port);  // 0 = any free port; false if the port is taken
        void Close();
        bool IsOpen();
        int LocalPort();
        // The game's role in the session: the slot this machine plays, the player count, and for a
        // client the host's address (a host learns its clients' from their packets, SetClientAddress).
        void SetRole(Role role, uint32_t slot, uint32_t players, uint32_t hostIp, int hostPort);
        void SetClientAddress(uint32_t slot, uint32_t ip, int port);
        void SendTo(uint32_t ip, int port, const void* data, size_t size);
        void Broadcast(int port, const void* data, size_t size);  // 255.255.255.255 (the LAN)
    }
    // Moves a Crash through its physics only (its partner stays where it is).
    bool TeleportCrash(PPCContext& ctx, uint8_t* base, uint32_t crash, float x, float y, float z);
    // M2 (kernel/xam_input_imports.cpp): LocalPad gets player 1's pad each game read (sent to the other
    // machines; with CRASH_MP_LEASH set, its stick no longer walks away from the others past it; off by default);
    // RemotePad gives controller `user` the pad of the remote player it plays (true if one is connected:
    // it replaces whatever is there).
    // `pad` is a 12-byte XINPUT_GAMEPAD (kernel/xinput_types.h; the name clashes with <xinput.h>).
    void LocalPad(void* pad, bool poll);
    bool RemotePad(uint32_t user, void* pad);
    // CDamageableBehaviour::AdjustHitpoints (dev/dev_cheats.cpp's hook): learns each actor's
    // damageable behaviour (the records carry its hitpoints) and returns the delta to apply: on a
    // client, 0 for the local damage to an actor the host mirrors (the host's hitpoints come instead).
    double Hitpoints(uint32_t damageable, double delta);
}
