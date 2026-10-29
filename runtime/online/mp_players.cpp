// Online multiplayer prototype, M0: more Crashes than the game's two players
// (docs/internals/multiplayer.md).
//
// Crash's constructor (sub_82153860) gives each new Crash the first free player index (the byte at
// bc +292: 0 while CGameDatabase +16 is empty, 1 while +20 is) and stores the Crash in the two-entry
// table at 0x8249D778[index]. A third Crash gets index 2 and its store lands on 0x8249D780, the
// "Crash in the backpack" pointer of the co-op code (EnterOtherPlayersBackpack sub_82154D10,
// IsOkayToLeaveBackpack sub_821544E0...). With the prototype on, that word is put back after the
// constructor of any Crash with index >= 2, and the extra Crash is logged; the level script
// spawns it with pObjective:DO_SpawnPlayer(3, "<name>", "<locator>").
//
// Online (or CRASH_MP_PLAYERS set) players 2-4 come up the same way: the constructor reads the co-op
// state the last level left (0x8245FA9C[index]: 0 = not joined, 1 = in the backpack, 2 = on foot;
// Crash::vf20 saves it) and 0x8245FA98 is the player that joins into the backpack at the start (-1 =
// nobody); both are set to "not joined, nobody", so player 2 is built hidden like players 3 and 4.
// PlayersTick then shows each one next to player 1 with the game's own "leave the backpack" show
// (sub_82154A58) and gives it its controller. The game's limit stays 4 players: 4 controllers (CInputManager
// devices) and 4 AI targets (CAIManager); a 5th Crash is left hidden.
// Afterwards nobody is pulled into the backpack: co-op puts the player the shared camera leaves behind
// into the other one's, but each machine has its own camera. The Crash's script asks
// IsOkayToEnterBackpack (sub_82153400) first, then SetPlayerEnteringBackpack turns the Crash's collision
// off and EnterOtherPlayersBackpack (sub_82154D10) attaches it: online the question says no for a Crash
// on foot (state 2), and an entry that still comes takes the game's own failure path (flags +0x110 bit 2:
// collision back on, DidEnterBackpackFail). Skipping the entry alone left the Crash without collision on
// the other's back: it fell through the floor and was knocked out.
// A player's death posts a CActionResetPlayer; its first run (sub_8220D830) puts player 2 of a co-op game
// into player 1's backpack, knocked out (co-op state 3, the timer at +0x114, then state 0: not joined).
// Players 3 and 4 get no such action (they just lie there). Here every player is knocked out the same way
// instead, on that action or on its hitpoints reaching 0: it lies for the game's knock-out time (the float at 0x82002328), then the actor is
// reset (sub_82090370, what a respawn does: up again, full hitpoints) and put next to the teammate on foot
// with the lowest slot (the same one on every machine).

#include "mp.h"
#include "mp_lobby.h"
#include "../diagnostics.h"
#include "../dev/dev_tools.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"

#include <ppc_config.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_82153860);  // Crash::Crash
extern "C" PPC_FUNC(__imp__sub_82154D10);  // Crash::EnterOtherPlayersBackpack
extern "C" PPC_FUNC(__imp__sub_82153400);  // Crash::IsOkayToEnterBackpack
extern "C" PPC_FUNC(__imp__sub_82153348);  // Crash::SetPlayerEnteringBackpack
extern "C" PPC_FUNC(__imp__sub_82153B08);  // Crash::~Crash
extern "C" PPC_FUNC(__imp__sub_821551B0);  // should the player's fight tree go straight to the backpack
extern "C" PPC_FUNC(__imp__sub_82098A38);  // CAIManager: adds an AI target (a player actor)
extern "C" PPC_FUNC(__imp__sub_8220D830);  // CActionResetPlayer: a player's death (vf01's first run)
PPC_EXTERN_FUNC(sub_82090370);             // CActor: reset (vtable +0x3c), what a respawn does
PPC_EXTERN_FUNC(sub_82142608);             // CActor::GetBehaviour
PPC_EXTERN_FUNC(sub_82143698);             // CPhysicsBehaviour: collision on / off
PPC_EXTERN_FUNC(sub_82098E68);             // CAIManager: removes an AI target (RemoveTarget)
PPC_EXTERN_FUNC(sub_82154A58);             // Crash: shows a player (behaviours on, collision, AI target), as leaving the backpack does

namespace
{
    constexpr uint32_t CRASH_TABLE = 0x8249D778;     // Crash* [2], by player index
    constexpr uint32_t BACKPACK_CRASH = 0x8249D780;  // right after it
    constexpr uint32_t MANAGERS_TABLE = 0x824A7A24;  // CUberManager: manager n at +(n + 7) * 4 (2 = AI, 7 = input)
    constexpr uint32_t COOP_STATE = 0x8245FA9C;      // int [2], per player index
    constexpr uint32_t AUTO_JOIN = 0x8245FA98;       // byte: player index, -1 = nobody

    bool GuestPointer(uint32_t p) { return (p >= 0x40000000 && p < 0x7F000000) || (p >= 0xA0000000 && p < 0xC0000000); }
    float LoadF(uint32_t guest)
    {
        uint32_t bits = LoadBE32(guest);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }

    // Players 3-5: native Crashes with player index 2-4 (the game's tables have 2 entries; index 2 lands
    // on the backpack pointer, which the constructor hook restores). Game thread only.
    struct Extra
    {
        uint32_t crash = 0;
        uint8_t index = 0;
        uint64_t since = 0;
        uint64_t shownAt = 0;  // its transform is the new place a frame later: in play 300 ms after
        bool onFoot = false;
    };
    std::vector<Extra> g_extras;  // players 2-4 (index 1-3) to be shown, in spawn order

    constexpr uint32_t KNOCKOUT_SECONDS = 0x82002328;  // float: the co-op knock-out time
    struct Knockout
    {
        uint32_t crash = 0;
        uint32_t index = 0;
        uint64_t until = 0;
    };
    std::vector<Knockout> g_knockouts;
    std::vector<uint32_t> g_departed;  // the Crashes of players who left the session (RemovePlayer)
    uint64_t g_revivedAt[4] = {};

    // Player 1 sits at the parking place of the hidden Crashes for the first frames of a level, until the
    // level's spawn moves it: in play 500 ms after PlayersTick first sees it in a level.
    uint32_t g_p1 = 0;
    uint64_t g_p1Since = 0;

    void KnockOut(uint32_t index, const char* why)
    {
        uint32_t crash = index < 4 ? mp::PlayerCrash(index) : 0;
        if (!crash || std::any_of(g_knockouts.begin(), g_knockouts.end(), [crash](const Knockout& k) { return k.crash == crash; }))
            return;
        float seconds = LoadF(KNOCKOUT_SECONDS);
        if (uint32_t chosen = mp::lobby::KnockOutSeconds())
            seconds = float(chosen);  // the session's option (mp_lobby.h)
        if (!(seconds >= 1.0f && seconds <= 10.0f))
            seconds = 3.0f;
        g_knockouts.push_back({crash, index, mp::NowMs() + uint64_t(seconds * 1000)});
        Log("[mp] player %u (Crash %08X) knocked out (%s, hitpoints %.1f), back in %.1f s", index + 1, crash, why,
            mp::PlayerHitpoints(index), seconds);
    }

    bool Managed()
    {
        static bool players = getenv("CRASH_MP_PLAYERS") != nullptr;
        return mp::Online() || (mp::Enabled() && players);
    }

    size_t ExtraCount()
    {
        return size_t(std::count_if(g_extras.begin(), g_extras.end(), [](const Extra& e) { return e.index >= 2; }));
    }

    // CInputManager (manager 7: MANAGERS_TABLE + (7 + 7) * 4): its CInputDevices at +8..+12, one per
    // controller (device +0x10 = the controller number, 0-3).
    constexpr uint32_t INPUT_DEVICE_VTABLE = 0x8201DC54;

    uint32_t Device(uint32_t controller)
    {
        uint32_t input = LoadBE32(MANAGERS_TABLE + (7 + 7) * 4);
        if (!GuestPointer(input))
            return 0;
        for (uint32_t p = LoadBE32(input + 8); GuestPointer(p) && p < LoadBE32(input + 12); p += 4)
            if (uint32_t device = LoadBE32(p); GuestPointer(device) && LoadBE32(device + 0x10) == controller)
                return device;
        return 0;
    }
}

PPC_FUNC(sub_82153860)
{
    uint32_t bc = ctx.r3.u32;
    // Index 2-4 store past the 2-entry table: the backpack Crash (+8), its rider (+12), the last rider (+16).
    uint32_t after[3] = {LoadBE32(BACKPACK_CRASH), LoadBE32(BACKPACK_CRASH + 4), LoadBE32(BACKPACK_CRASH + 8)};
    uint32_t backpack = after[0];
    if (Managed())
    {
        StoreBE32(COOP_STATE + 4, 2);
        *g_memory.Translate(AUTO_JOIN) = 0xFF;
    }
    __imp__sub_82153860(ctx, base);
    if (!mp::Enabled())
        return;
    uint8_t index = *g_memory.Translate(bc + 292);
    if (index >= 2)
    {
        for (uint32_t i = 0; i < 3; ++i)
            StoreBE32(BACKPACK_CRASH + i * 4, after[i]);
        // The game gives every Crash past the second index 2: players 3, 4, 5 get 2, 3, 4.
        index = uint8_t(2 + ExtraCount());
        *g_memory.Translate(bc + 292) = index;
        // Its co-op state came from 0x8245FA9C[index], past the 2-entry table: not joined.
        uint32_t state = LoadBE32(bc + 300);
        StoreBE32(bc + 300, 0);
        Log("[mp] extra Crash %08X: co-op state read past the table %08X, now 0", bc, state);
        g_extras.push_back({bc, index, mp::NowMs(), false});
        Log("[mp] extra Crash %08X got player index %u (backpack pointer kept at %08X)", bc, index, backpack);
    }
    else
    {
        Log("[mp] Crash %08X is player %u (table %08X), co-op state %u, flags %02X", bc, index + 1,
            LoadBE32(CRASH_TABLE + index * 4), LoadBE32(bc + 300), *g_memory.Translate(bc + 0x110));
        if (index == 1 && Managed())
            g_extras.push_back({bc, index, mp::NowMs(), false});
    }
}

// CAIManager holds 4 AI targets (36-byte slots from +4, the count at +0x94, right after the 4th):
// a 5th add writes over the count and the AI update (sub_820985E0) then runs over its stack arrays.
// Each Crash spawned adds itself; with players 3-5 the 5th is refused (enemies don't chase player 5)
// and an actor already there is not added twice.
PPC_FUNC(sub_82098A38)
{
    uint32_t ai = ctx.r3.u32, actor = ctx.r4.u32, count = LoadBE32(ai + 0x94);
    for (uint32_t i = 0; i < count && i < 4; ++i)
        if (LoadBE32(ai + 4 + i * 36) == actor)
            return;
    if (count >= 4)
    {
        static int logged;
        if (logged++ < 3)
            Log("[mp] AI target %08X refused: the AI manager holds 4", actor);
        return;
    }
    __imp__sub_82098A38(ctx, base);
}

PPC_FUNC(sub_82153B08)
{
    uint32_t crash = ctx.r3.u32;
    g_extras.erase(std::remove_if(g_extras.begin(), g_extras.end(), [crash](const Extra& e) { return e.crash == crash; }),
                   g_extras.end());
    g_knockouts.erase(std::remove_if(g_knockouts.begin(), g_knockouts.end(), [crash](const Knockout& k) { return k.crash == crash; }),
                      g_knockouts.end());
    g_departed.erase(std::remove(g_departed.begin(), g_departed.end(), crash), g_departed.end());
    // When both entries of the table are empty the destructor releases the players' shared object
    // (0x8249D78C, vf1). A level's unload destroyed players 1 and 2 before the third Crash, which then
    // used it (a crash leaving any level with 3+ players): while an extra Crash is alive the other entry
    // holds it, so the last Crash to go releases the object.
    uint32_t index = *g_memory.Translate(crash + 292);
    uint32_t keep = 0;
    if (index < 2 && LoadBE32(CRASH_TABLE + index * 4) == crash && !LoadBE32(CRASH_TABLE + (index ^ 1) * 4))
        for (const Extra& e : g_extras)
            if (e.index >= 2 && GuestPointer(e.crash))
            {
                keep = e.crash;
                break;
            }
    if (keep)
    {
        StoreBE32(CRASH_TABLE + (index ^ 1) * 4, keep);
        Log("[mp] Crash %08X (player %u) goes before extra Crash %08X: the shared object stays", crash, index + 1, keep);
    }
    __imp__sub_82153B08(ctx, base);
    if (keep)
        StoreBE32(CRASH_TABLE + (index ^ 1) * 4, 0);
}

PPC_FUNC(sub_8220D830)
{
    if (!Managed())
    {
        __imp__sub_8220D830(ctx, base);
        return;
    }
    uint32_t index = LoadBE32(ctx.r3.u32 + 4);
    KnockOut(index, "reset action");
    ctx.r3.u64 = 1;  // done, as the co-op path answers
}

// The first step of entering the backpack turns the Crash's collision off: online it never starts for a
// Crash on foot (EnterOtherPlayersBackpack below then fails the game's way).
PPC_FUNC(sub_82153348)
{
    if (mp::NoBackpack() && LoadBE32(ctx.r3.u32 + 300) == 2)
    {
        static int logged;
        if (logged++ < 3)
            Log("[mp] Crash %08X: entering the backpack skipped (online)", ctx.r3.u32);
        *g_memory.Translate(ctx.r3.u32 + 272) &= ~0x80;  // the original clears the cached "okay" too
        return;
    }
    __imp__sub_82153348(ctx, base);
}

PPC_FUNC(sub_82153400)
{
    uint32_t crash = ctx.r3.u32;
    __imp__sub_82153400(ctx, base);
    if (mp::NoBackpack() && ctx.r3.u32 && LoadBE32(crash + 300) == 2)
    {
        static int logged;
        if (logged++ < 3)
            Log("[mp] Crash %08X kept out of the backpack (online)", crash);
        ctx.r3.u64 = 0;
    }
}

// The fight tree sends a player straight to the backpack ("GoStraightToBackpack", sub_820A0160) while its
// Crash's cached "okay to enter the backpack" (+272 bit 0x80, set by sub_82154E78 before the Crash was
// on foot) is on; only SetPlayerEnteringBackpack, skipped above, clears it. So a player went into the
// EnterBackpack node every frame (no animation, no hits; on the host whenever it left the camera).
// Online the cache is cleared for the Crashes on foot first.
PPC_FUNC(sub_821551B0)
{
    if (mp::NoBackpack())
        for (uint32_t index = 0; index < 2; ++index)
        {
            uint32_t crash = LoadBE32(CRASH_TABLE + index * 4);
            if (GuestPointer(crash) && LoadBE32(crash + 300) == 2)
                *g_memory.Translate(crash + 272) &= ~0x80;
        }
    __imp__sub_821551B0(ctx, base);
}

PPC_FUNC(sub_82154D10)
{
    if (mp::NoBackpack() && LoadBE32(ctx.r3.u32 + 300) == 2)
    {
        static int logged;
        if (logged++ < 3)
            Log("[mp] Crash %08X: backpack entry made to fail (online)", ctx.r3.u32);
        *g_memory.Translate(ctx.r3.u32 + 0x110) |= 4;
    }
    if (mp::Enabled())
        Log("[mp] Crash %08X into the backpack (state %u)", ctx.r3.u32, LoadBE32(ctx.r3.u32 + 300));
    __imp__sub_82154D10(ctx, base);
}

namespace
{
    // Players 3 and 4 (CRASH_MP_PLAYERS > 2) are spawned the way the levels spawn players 1 and 2
    // (objectives.lua: pObjective:DO_SpawnPlayer(n, name, locator)), by one objective of the game's own
    // system, once per level, when player 1 is on foot; every machine names them the same
    // (ref_Crash_P3 / ref_Crash_P4: the network id).
    void SpawnPlayers(PPCContext& ctx, uint8_t* base)
    {
        static uint32_t spawnedFor;  // player 1's Crash of the level they were spawned in
        static uint64_t p1Since;
        uint32_t p1 = LoadBE32(CRASH_TABLE);
        if (mp::Players() <= 2 || !GuestPointer(p1) || LoadBE32(p1 + 300) != 2 || spawnedFor == p1)
        {
            if (!GuestPointer(p1) || LoadBE32(p1 + 300) != 2)
                p1Since = 0;
            return;
        }
        if (!p1Since)
            p1Since = mp::NowMs();
        if (mp::NowMs() - p1Since < 500)
            return;
        spawnedFor = p1;
        // Not resetable: the level's end resets the objectives, and this one (made outside the level's
        // script, so not unloaded with it) would spawn the players again from names already freed.
        std::string code = "local o = NewObjective(\"mp_spawn_players\", \"\", true)";
        for (uint32_t n = 3; n <= mp::Players(); ++n)
            if (!mp::PlayerCrash(n - 1))
                code += " o:DO_SpawnPlayer(" + std::to_string(n) + ", \"ref_Crash_P" + std::to_string(n) +
                        "\", \"PlayerSpawnPosition\", \"\")";
        code += " o:Done()";
        std::string error;
        bool ok = dev::RunLua(ctx, base, code.c_str(), "mp_spawn_players", &error);
        Log("[mp] spawning players 3-%u: %s", mp::Players(), ok ? "ok" : error.c_str());
    }
}

namespace mp
{
    bool NoBackpack() { return Managed(); }

    bool PlayerOnFoot(uint32_t index)
    {
        uint32_t crash = PlayerCrash(index);
        if (!crash || LoadBE32(crash + 300) != 2 || std::find(g_departed.begin(), g_departed.end(), crash) != g_departed.end())
            return false;
        for (const Extra& e : g_extras)
            if (e.crash == crash)
                return e.onFoot && e.index < 4 && NowMs() - e.shownAt >= 300;
        return !Managed() || (crash == g_p1 && NowMs() - g_p1Since >= 500);
    }

    uint32_t PlayerCrash(uint32_t index)
    {
        if (index < 2)
        {
            uint32_t crash = LoadBE32(CRASH_TABLE + index * 4);
            return GuestPointer(crash) ? crash : 0;
        }
        for (const Extra& e : g_extras)
            if (e.index == index)
                return e.crash;
        return 0;
    }

    std::vector<uint32_t> ExtraCrashes()
    {
        std::vector<uint32_t> out;
        for (const Extra& e : g_extras)
            if (e.onFoot && e.index >= 2)
                out.push_back(e.crash);
        return out;
    }

    // Players 2-4 are built like an unjoined player 2: hidden, parked, its behaviours off. Once player 1
    // is on foot each is shown the way leaving the backpack shows a player (sub_82154A58 detaches it from
    // the backpack Crash's attacher, so that pointer is player 1 for the call), set on foot (co-op
    // state 2) and put next to player 1.
    bool HitpointsHeld(uint32_t index)
    {
        for (const Knockout& k : g_knockouts)
            if (k.index == index)
                return true;
        return index < 4 && g_revivedAt[index] && NowMs() - g_revivedAt[index] < 1500;
    }

    // A knocked-out player whose time is up: reset (the respawn's reset keeps two of the actor's values,
    // behaviours 0xF / 0x11, when the first is above the threshold, as sub_8220D830 does) and put next to
    // the teammate on foot with the lowest slot (itself, where it lies, when nobody is).
    void Revive(PPCContext& ctx, uint8_t* base, const Knockout& k)
    {
        uint32_t physics = LoadBE32(k.crash + 0xDC), actor = GuestPointer(physics) ? LoadBE32(physics + 96) : 0;
        if (!GuestPointer(actor))
            return;
        uint32_t target = 0, targetSlot = ~0u;
        for (uint32_t i = 0; i < 4; ++i)
        {
            uint32_t crash = PlayerCrash(i);
            if (!crash || crash == k.crash || !PlayerOnFoot(i) || HitpointsHeld(i))
                continue;
            if (PlayerSlot(i) < targetSlot)
                targetSlot = PlayerSlot(i), target = crash;
        }
        uint32_t targetPhysics = LoadBE32((target ? target : k.crash) + 0xDC);
        uint32_t targetActor = GuestPointer(targetPhysics) ? LoadBE32(targetPhysics + 96) : 0;
        uint32_t xform = GuestPointer(targetActor) ? LoadBE32(targetActor + 44) : 0;
        PPCContext call = ctx;
        call.r3.u64 = actor;
        call.r4.u64 = 0xF;
        sub_82142608(call, base);
        uint32_t kept = call.r3.u32;
        call = ctx;
        call.r3.u64 = actor;
        call.r4.u64 = 0x11;
        sub_82142608(call, base);
        uint32_t kept2 = call.r3.u32;
        bool keep = GuestPointer(kept) && GuestPointer(kept2) && LoadF(kept) > LoadF(0x82000DC8);
        uint32_t values[2] = {keep ? LoadBE32(kept) : 0, keep ? LoadBE32(kept2) : 0};
        call = ctx;
        call.r3.u64 = actor;
        call.r4.u64 = 0;
        sub_82090370(call, base);
        if (keep)
        {
            StoreBE32(kept, values[0]);
            StoreBE32(kept2, values[1]);
        }
        g_revivedAt[k.index] = NowMs();
        if (!GuestPointer(xform))
            return;
        // Behind the teammate's line, 1.2 apart per slot (not on the players who came up beside player 1, nor
        // on another one back at the same time).
        float x = LoadF(xform + 48) + (target ? 1.2f * (PlayerSlot(k.index) + 1) : 0.0f), y = LoadF(xform + 52) + 0.5f,
              z = LoadF(xform + 56) - (target ? 1.5f : 0.0f);
        bool moved = TeleportCrash(ctx, base, k.crash, x, y, z);
        Log("[mp] player %u back at (%.1f, %.1f, %.1f)%s%s, hitpoints %.1f of %.1f", k.index + 1, x, y, z,
            target ? "" : " (nobody on foot)", moved ? "" : ", not moved", PlayerHitpoints(k.index), PlayerMaxHitpoints(k.index));
    }

    // A player who left the session goes out of the level, on every machine: what showing it turned on
    // (sub_82154A58: the Crash's behaviours, its physics' collision, the AI target) is turned off again, so
    // nobody sees, hits or chases it, and it is out of play (PlayerOnFoot: no knock-out, snap or colour).
    void RemovePlayer(PPCContext& ctx, uint8_t* base, uint32_t index)
    {
        uint32_t crash = PlayerCrash(index);
        if (!crash || std::find(g_departed.begin(), g_departed.end(), crash) != g_departed.end())
            return;
        g_departed.push_back(crash);
        g_knockouts.erase(std::remove_if(g_knockouts.begin(), g_knockouts.end(), [crash](const Knockout& k) { return k.crash == crash; }),
                          g_knockouts.end());
        for (uint32_t offset : {224u, 180u, 220u, 208u, 216u, 212u, 244u, 240u, 228u, 236u})
        {
            uint32_t behaviour = LoadBE32(crash + offset);
            uint32_t vtable = GuestPointer(behaviour) ? LoadBE32(behaviour) : 0;
            uint32_t function = vtable >= 0x82000000 && vtable < 0x82600000 ? LoadBE32(vtable + 44) : 0;  // vf11
            PPCFunc* setEnabled = function >= PPC_CODE_BASE && function < PPC_CODE_BASE + PPC_CODE_SIZE ? PPC_LOOKUP_FUNC(base, function) : nullptr;
            if (!setEnabled)
                continue;
            PPCContext call = ctx;
            call.r3.u64 = behaviour;
            call.r4.u64 = 0;
            setEnabled(call, base);
            if (offset == 220)
            {
                call = ctx;
                call.r3.u64 = behaviour;
                call.r4.u64 = 0;
                sub_82143698(call, base);  // the physics' collision
            }
        }
        *g_memory.Translate(crash + 272) &= ~0x10;
        uint32_t ai = LoadBE32(MANAGERS_TABLE + (2 + 7) * 4), actor = LoadBE32(crash + 156);
        if (GuestPointer(ai) && GuestPointer(actor))
        {
            PPCContext call = ctx;
            call.r3.u64 = ai;
            call.r4.u64 = actor;
            sub_82098E68(call, base);
        }
        Log("[mp] player %u (Crash %08X) left the session: out of the level", index + 1, crash);
    }

    void PlayersTick(PPCContext& ctx, uint8_t* base)
    {
        if (!Managed() || !InLevel())
            return;
        if (uint32_t p1 = PlayerCrash(0); p1 != g_p1)
            g_p1 = p1, g_p1Since = NowMs();
        SpawnPlayers(ctx, base);
        // Players 3 and 4 come with the template's maximum (90), players 1 and 2 with the game's (30 at the
        // start, more with the upgrades): they get player 1's.
        float p1Max = PlayerMaxHitpoints(0);
        for (uint32_t i = 2; i < 4; ++i)
            if (p1Max > 0 && PlayerOnFoot(i) && PlayerMaxHitpoints(i) > 0 && PlayerMaxHitpoints(i) != p1Max)
            {
                Log("[mp] player %u: maximum hitpoints %.0f -> %.0f (player 1's)", i + 1, PlayerMaxHitpoints(i), p1Max);
                SetPlayerMaxHitpoints(i, p1Max);
            }
        for (uint32_t i = 0; i < 4; ++i)
            if (PlayerOnFoot(i) && !HitpointsHeld(i) && PlayerHitpoints(i) > -0.5f && PlayerHitpoints(i) <= 0.0f)
                KnockOut(i, "no hitpoints");
        for (size_t i = 0; i < g_knockouts.size();)
            if (NowMs() >= g_knockouts[i].until)
            {
                Knockout k = g_knockouts[i];
                g_knockouts.erase(g_knockouts.begin() + i);
                Revive(ctx, base, k);
            }
            else
                ++i;
        if (g_extras.empty())
            return;
        uint32_t p1 = LoadBE32(CRASH_TABLE);
        uint32_t p1Physics = GuestPointer(p1) ? LoadBE32(p1 + 0xDC) : 0;
        uint32_t p1Actor = GuestPointer(p1Physics) ? LoadBE32(p1Physics + 96) : 0;
        uint32_t p1Xform = GuestPointer(p1Actor) ? LoadBE32(p1Actor + 44) : 0;
        if (!GuestPointer(p1Xform) || LoadBE32(p1 + 300) != 2)
            return;
        for (Extra& e : g_extras)
        {
            if (e.onFoot || NowMs() - e.since < 1500 || !GuestPointer(LoadBE32(e.crash + 0xDC)))
                continue;
            if (e.index >= 4)
            {
                e.onFoot = true;  // never shown
                Log("[mp] player %u (Crash %08X) left hidden: the game holds 4 players", e.index + 1, e.crash);
                continue;
            }
            uint32_t saved = LoadBE32(BACKPACK_CRASH);
            StoreBE32(BACKPACK_CRASH, p1);
            PPCContext call = ctx;
            call.r3.u64 = e.crash;
            sub_82154A58(call, base);
            StoreBE32(BACKPACK_CRASH, saved);
            StoreBE32(e.crash + 300, 2);
            // Its controller: the Crash's input binding (+176 -> +0 the CInputDevice); players 3 and 4 start on
            // player 2's.
            uint32_t binding = LoadBE32(e.crash + 176), device = Device(e.index);
            if (GuestPointer(binding) && LoadBE32(LoadBE32(binding)) == INPUT_DEVICE_VTABLE && device)
                StoreBE32(binding, device);
            else
                device = 0;
            float x = LoadF(p1Xform + 48) + 1.5f * e.index, y = LoadF(p1Xform + 52) + 0.5f, z = LoadF(p1Xform + 56);
            bool moved = TeleportCrash(ctx, base, e.crash, x, y, z);
            e.onFoot = true;
            e.shownAt = NowMs();
            AddPlayer(LoadBE32(LoadBE32(e.crash + 0xDC) + 96));
            Log("[mp] player %u (Crash %08X) on foot at (%.1f, %.1f, %.1f)%s, controller %u%s", e.index + 1, e.crash, x, y,
                z, moved ? "" : ", not moved", e.index, device ? "" : " (no such device)");

        }
    }
}
