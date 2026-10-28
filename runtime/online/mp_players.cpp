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

#include "mp.h"
#include "../diagnostics.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"

extern "C" PPC_FUNC(__imp__sub_82153860);  // Crash::Crash

namespace
{
    constexpr uint32_t CRASH_TABLE = 0x8249D778;     // Crash* [2], by player index
    constexpr uint32_t BACKPACK_CRASH = 0x8249D780;  // right after it
}

PPC_FUNC(sub_82153860)
{
    uint32_t bc = ctx.r3.u32;
    uint32_t backpack = LoadBE32(BACKPACK_CRASH);
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
        Log("[mp] Crash %08X is player %u (table %08X)", bc, index + 1, LoadBE32(CRASH_TABLE + index * 4));
}
