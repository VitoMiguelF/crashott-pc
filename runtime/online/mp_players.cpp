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
// Online (host or client), player 2 joins at the level start the way the game does between two co-op
// levels: the constructor copies the co-op state the last level left (0x8245FA9C[index]: 0 = not
// joined, 1 = in the backpack, 2 = on foot; Crash::vf20 saves it) into bc +300, and 0x8245FA98 is the
// player that joins into the backpack at the start (-1 = nobody). Starting player 2 at state 2
// directly skips its model setup (only one Crash is drawn), so it is state 0 + auto-join, and its
// controller taps B off the backpack (mp_net.cpp, RemotePad) under the episode title.
// Afterwards nobody is pulled into the backpack: co-op puts the player the shared camera leaves behind
// into the other one's (Crash::EnterOtherPlayersBackpack on a Crash on foot, state 2), but each
// machine has its own camera.

#include "mp.h"
#include "../diagnostics.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"

extern "C" PPC_FUNC(__imp__sub_82153860);  // Crash::Crash
extern "C" PPC_FUNC(__imp__sub_82154D10);  // Crash::EnterOtherPlayersBackpack

namespace
{
    constexpr uint32_t CRASH_TABLE = 0x8249D778;     // Crash* [2], by player index
    constexpr uint32_t BACKPACK_CRASH = 0x8249D780;  // right after it
    constexpr uint32_t COOP_STATE = 0x8245FA9C;      // int [2], per player index
    constexpr uint32_t AUTO_JOIN = 0x8245FA98;       // byte: player index, -1 = nobody
}

PPC_FUNC(sub_82153860)
{
    uint32_t bc = ctx.r3.u32;
    uint32_t backpack = LoadBE32(BACKPACK_CRASH);
    if (mp::Online())
    {
        StoreBE32(COOP_STATE + 4, 0);
        *g_memory.Translate(AUTO_JOIN) = 1;
    }
    __imp__sub_82153860(ctx, base);
    if (!mp::Enabled())
        return;
    uint8_t index = *g_memory.Translate(bc + 292);
    if (index >= 2)
    {
        StoreBE32(BACKPACK_CRASH, backpack);
        Log("[mp] extra Crash %08X got player index %u (backpack pointer kept at %08X)", bc, index, backpack);
    }
    else
        Log("[mp] Crash %08X is player %u (table %08X), co-op state %u, flags %02X", bc, index + 1,
            LoadBE32(CRASH_TABLE + index * 4), LoadBE32(bc + 300), *g_memory.Translate(bc + 0x110));
}

PPC_FUNC(sub_82154D10)
{
    if (mp::Online() && LoadBE32(ctx.r3.u32 + 300) == 2)
    {
        static int logged;
        if (logged++ < 3)
            Log("[mp] Crash %08X kept out of the backpack (online)", ctx.r3.u32);
        return;
    }
    if (mp::Enabled())
        Log("[mp] Crash %08X into the backpack (state %u)", ctx.r3.u32, LoadBE32(ctx.r3.u32 + 300));
    __imp__sub_82154D10(ctx, base);
}
