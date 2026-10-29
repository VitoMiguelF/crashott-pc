// Each player's skin in a MULTIPLAYER session (mp.h, mp::skins; docs/internals/multiplayer.md, "Skins").
//
// The game has one skin, the template hash at 0x824A8270 (dev/dev_skins.cpp): a level load loads its
// template, every CActionSpawnPlayer keeps it (+8, its constructor) and spawns that template
// (CActionSpawnActor sub_822102B0 -> GenerateObject sub_821363E8 with +8), and Crash:CheckSkinType (the
// skin's weapon in the fight tree, HLSkinWeapon) compares with it. So every machine showed every player
// in its own player's skin. Online each player's skin (its machine's current one) goes through the
// lobby (HELLO / PING to the host, ROSTER to everybody, mp_lobby.cpp); here a level load also loads the
// other players' skin templates (released at the unload), the spawn of player n (+104, 1-based) gets the
// skin of the slot that player is on this machine, and CheckSkinType compares with the skin its Crash
// was spawned with. Only the wearable skins' templates are taken (dev::skins::Wearable).

#include "mp.h"
#include "../dev/dev_tools.h"
#include "../diagnostics.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"

#include <algorithm>
#include <mutex>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_822116C0);  // CActionSpawnPlayer: run (spawns the player)
extern "C" PPC_FUNC(__imp__sub_821534A0);  // Crash:CheckSkinType(ESkin)
PPC_EXTERN_FUNC(sub_82215730);             // ESkin -> template hash (dev_skins.cpp's hook)
PPC_EXTERN_FUNC(sub_8224CC08);             // the stream manager
PPC_EXTERN_FUNC(sub_8224D3C8);             // stream manager: load a template (by hash)
PPC_EXTERN_FUNC(sub_8224CEC8);             // stream manager: release a template (by hash)

namespace
{
    constexpr uint32_t CURRENT_SKIN = 0x824A8270;  // the skin worn: its template hash
    constexpr uint32_t SPAWN_SKIN = 8, SPAWN_PLAYER = 104;  // CActionSpawnPlayer: template hash, player (1-based)

    std::mutex g_mutex;  // the slots' skins (the lobby's I/O thread sets them)
    uint32_t g_slotSkin[mp::MAX_PLAYERS] = {};
    // Game thread: the templates this file loaded for the level, the wearable skins' templates, and the
    // skin each player (local index) was spawned with.
    std::vector<uint32_t> g_loaded, g_wearable;
    uint32_t g_spawned[mp::MAX_PLAYERS] = {};

    bool Wearable(uint32_t hash)
    {
        return hash && std::find(g_wearable.begin(), g_wearable.end(), hash) != g_wearable.end();
    }

    // Loads or releases a template by hash (the game's calls take a pointer to the hash).
    void Template(PPCContext& ctx, uint8_t* base, uint32_t hash, bool load)
    {
        static uint32_t word = GuestAlloc(4);
        StoreBE32(word, hash);
        PPCContext call = ctx;
        sub_8224CC08(call, base);
        call.r4.u64 = word;
        if (load)
            sub_8224D3C8(call, base);
        else
            sub_8224CEC8(call, base);
    }
}

namespace mp::skins
{
    uint32_t Local() { return LoadBE32(CURRENT_SKIN); }

    void SetSlotSkins(const uint32_t* skins, uint32_t count)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (uint32_t slot = 0; slot < MAX_PLAYERS; ++slot)
            g_slotSkin[slot] = slot < count ? skins[slot] : 0;
    }

    void ClearSlotSkins() { SetSlotSkins(nullptr, 0); }

    uint32_t SlotSkin(uint32_t slot)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return slot < MAX_PLAYERS ? g_slotSkin[slot] : 0;
    }

    void LevelLoad(PPCContext& ctx, uint8_t* base)
    {
        if (g_wearable.empty())
            for (int skin = 0; skin <= 24; ++skin)
                if (dev::skins::Wearable(skin))
                {
                    PPCContext call = ctx;
                    call.r3.u64 = uint32_t(skin);
                    sub_82215730(call, base);
                    g_wearable.push_back(call.r3.u32);
                }
        std::fill(std::begin(g_spawned), std::end(g_spawned), 0u);
        if (!Online())
            return;
        uint32_t current = Local();
        for (uint32_t slot = 0; slot < MAX_PLAYERS; ++slot)
        {
            uint32_t hash = SlotSkin(slot);
            if (!Wearable(hash) || hash == current || std::find(g_loaded.begin(), g_loaded.end(), hash) != g_loaded.end())
                continue;
            Template(ctx, base, hash, true);
            g_loaded.push_back(hash);
            Log("[mp] skins: slot %u's template %08X loaded with the level", slot + 1, hash);
        }
    }

    void LevelUnload(PPCContext& ctx, uint8_t* base)
    {
        for (uint32_t hash : g_loaded)
            Template(ctx, base, hash, false);
        g_loaded.clear();
        std::fill(std::begin(g_spawned), std::end(g_spawned), 0u);
    }
}

// CActionSpawnPlayer's run: player n gets the skin of the slot it is on this machine (its template loaded
// by LevelLoad, or the current one).
PPC_FUNC(sub_822116C0)
{
    uint32_t action = ctx.r3.u32;
    uint32_t index = LoadBE32(action + SPAWN_PLAYER) - 1;
    if (mp::Online() && index < mp::MAX_PLAYERS)
    {
        uint32_t hash = mp::skins::SlotSkin(mp::PlayerSlot(index));
        if (hash && hash != LoadBE32(action + SPAWN_SKIN) &&
            (hash == mp::skins::Local() || std::find(g_loaded.begin(), g_loaded.end(), hash) != g_loaded.end()))
        {
            StoreBE32(action + SPAWN_SKIN, hash);
            Log("[mp] skins: player %u (slot %u) spawned as %08X", index + 1, mp::PlayerSlot(index) + 1, hash);
        }
        g_spawned[index] = LoadBE32(action + SPAWN_SKIN);
    }
    __imp__sub_822116C0(ctx, base);
}

// Crash:CheckSkinType(skin): online, against the skin that Crash was spawned with (its weapon).
PPC_FUNC(sub_821534A0)
{
    uint32_t crash = ctx.r3.u32;
    if (mp::Online())
        for (uint32_t i = 0; i < mp::MAX_PLAYERS; ++i)
            if (g_spawned[i] && mp::PlayerCrash(i) == crash)
            {
                PPCContext call = ctx;
                call.r3.u64 = ctx.r4.u32;
                sub_82215730(call, base);
                ctx.r3.u64 = call.r3.u32 == g_spawned[i] ? 1 : 0;
                return;
            }
    __imp__sub_821534A0(ctx, base);
}
