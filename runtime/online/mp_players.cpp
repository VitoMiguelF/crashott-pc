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

#include "mp.h"
#include "../diagnostics.h"
#include "../dev/dev_tools.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"

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
extern "C" PPC_FUNC(__imp__sub_82098A38);  // CAIManager: adds an AI target (a player actor)
PPC_EXTERN_FUNC(sub_82154A58);             // Crash: shows a player (behaviours on, collision, AI target), as leaving the backpack does

namespace
{
    constexpr uint32_t CRASH_TABLE = 0x8249D778;     // Crash* [2], by player index
    constexpr uint32_t BACKPACK_CRASH = 0x8249D780;  // right after it
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
        bool onFoot = false;
    };
    std::vector<Extra> g_extras;  // players 2-4 (index 1-3) to be shown, in spawn order

    bool Managed()
    {
        static bool players = getenv("CRASH_MP_PLAYERS") != nullptr;
        return mp::Online() || (mp::Enabled() && players);
    }

    size_t ExtraCount()
    {
        return size_t(std::count_if(g_extras.begin(), g_extras.end(), [](const Extra& e) { return e.index >= 2; }));
    }

    // CInputManager (manager 7: 0x824A7A24 + (7 + 7) * 4): its CInputDevices at +8..+12, one per
    // controller (device +0x10 = the controller number, 0-3).
    constexpr uint32_t INPUT_DEVICE_VTABLE = 0x8201DC54;

    uint32_t Device(uint32_t controller)
    {
        uint32_t input = LoadBE32(0x824A7A24 + (7 + 7) * 4);
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
    __imp__sub_82153B08(ctx, base);
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
        std::string code = "dev.objective(\"mp_spawn_players\", function(o)";
        for (uint32_t n = 3; n <= mp::Players(); ++n)
            if (!mp::PlayerCrash(n - 1))
                code += " o:DO_SpawnPlayer(" + std::to_string(n) + ", \"ref_Crash_P" + std::to_string(n) +
                        "\", \"PlayerSpawnPosition\", \"\")";
        code += " end)";
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
        if (!crash || LoadBE32(crash + 300) != 2)
            return false;
        for (const Extra& e : g_extras)
            if (e.crash == crash)
                return e.onFoot && e.index < 4;
        return true;
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
    void PlayersTick(PPCContext& ctx, uint8_t* base)
    {
        if (!Managed() || !InLevel())
            return;
        SpawnPlayers(ctx, base);
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
            AddPlayer(LoadBE32(LoadBE32(e.crash + 0xDC) + 96));
            Log("[mp] player %u (Crash %08X) on foot at (%.1f, %.1f, %.1f)%s, controller %u%s", e.index + 1, e.crash, x, y,
                z, moved ? "" : ", not moved", e.index, device ? "" : " (no such device)");

        }
    }
}
