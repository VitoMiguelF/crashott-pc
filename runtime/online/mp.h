#pragma once

// Online multiplayer prototype (docs/internals/multiplayer.md). Everything is off unless CRASH_MP=1;
// with it off the game runs unchanged.
//
//   CRASH_MP=1                 turns the prototype on
//   CRASH_MP_STREAM=<mode>     vanilla | union | all: which rooms stay loaded (mp_stream.cpp)
//   CRASH_MP_ANCHOR=x,y,z;...  positions that count as players for the room choice (mp_stream.cpp)
//   CRASH_MP_TRACE=1|<ms>      rooms + players once a second (or every <ms>), and every player that
//                              moves more than 10 units in one frame ("[mp] stream ...", "[mp] actor ...")
//   CRASH_MP_ROLE=<role>       host | client: the M1 mirror over UDP (mp_net.cpp)
//   CRASH_MP_PORT=<n>          UDP port (default 47810); CRASH_MP_PEER=<ip> the host for a client
//   CRASH_MP_HZ=<n>            snapshots per second sent by the host (default 30)
//   CRASH_MP_REMOTE_USER=<n>   the game controller the peer's pad becomes (default 1 = player 2)
//   CRASH_MP_SNAP=<units>      client: a player further than this from its record is put back (1.0)
//   CRASH_MP_TELEPORT=x,y,z    host, tests: both players put there once they are on foot

#include <cstdint>
#include <string>
#include <vector>

struct PPCContext;

namespace mp
{
    enum class StreamMode { Vanilla, Union, All };

    bool Enabled();
    bool Online();  // CRASH_MP_ROLE host or client: the level starts with both players in play
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

    // mp_net.cpp
    void NetTick(PPCContext& ctx, uint8_t* base);
    // M2 (kernel/xam_input_imports.cpp): on a client, player 1's final pad state each game read
    // (sent to the host; in a level the local game then sees only START / BACK: the host moves
    // this player); on the host, the remote player's pad for controller `user` (true if one is
    // connected: it replaces whatever is there).
    // `pad` is a 12-byte XINPUT_GAMEPAD (kernel/xinput_types.h; the name clashes with <xinput.h>).
    void LocalPad(const void* pad, bool poll);
    bool RemotePad(uint32_t user, void* pad);
    // CDamageableBehaviour::AdjustHitpoints (dev/dev_cheats.cpp's hook): learns each actor's
    // damageable behaviour (the records carry its hitpoints) and returns the delta to apply: on a
    // client, 0 for the local damage to an actor the host mirrors (the host's hitpoints come instead).
    double Hitpoints(uint32_t damageable, double delta);
}
