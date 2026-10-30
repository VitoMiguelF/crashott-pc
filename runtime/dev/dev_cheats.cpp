// Cheats for tests (dev_tools.h; docs/dev_features.md).
//
//   CRASH_DEV_INVINCIBLE=1   the players' hitpoints never go down.
//   CRASH_DEV_DAMAGE_DEBUG=1 logs every damage with the first 32 words of the damageable behaviour.
//
// Every change of an actor's hitpoints goes through CDamageableBehaviour's AdjustHitpoints,
// sub_82110448(this, f1 = delta, r5 = damager): enemy hits (vf09 sub_82113470), damage over time
// (vf15), TakeDamage (the scripts' method, sub_82110750 = AdjustHitpoints(-amount)) and
// SetCurrentHitpoints (sub_821108B0 = AdjustHitpoints(value - current)). Hitpoints at *(this+64),
// the maximum at *(this+68), the HUD's fraction at *(this+72); at 0 the owner dies.
// A behaviour's actor is at +28; the players' actors are at +16 / +20 of the static CGameDatabase
// 0x824A76A4 (its getter sub_821E98E0; GetCurrentPlayer sub_821E9790 reads the same slots).

#include "dev_tools.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../online/mp.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>

extern "C" PPC_FUNC(__imp__sub_82110448);  // CDamageableBehaviour::AdjustHitpoints(delta, damager)
PPC_EXTERN_FUNC(sub_82234188);             // CSettingsDatabase: the cheat flags (+5 shadow, +6 big head)
PPC_EXTERN_FUNC(sub_82142608);             // actor: behaviour by index (38 = the render behaviour)
PPC_EXTERN_FUNC(sub_822845A0);             // joint name hash
PPC_EXTERN_FUNC(sub_822EE2B0);             // drawable: joint index by hash

namespace
{
    constexpr uint32_t GAME_DATABASE = 0x824A76A4;  // static CGameDatabase (getter sub_821E98E0)
    constexpr uint32_t GAME_DATABASE_PLAYERS = 16;
    constexpr uint32_t BEHAVIOUR_ACTOR = 28;
    constexpr uint32_t DAMAGEABLE_HITPOINTS = 64;

    bool Enabled(const char* name)
    {
        const char* value = getenv(name);
        return value && *value && strcmp(value, "0") != 0;
    }

    std::atomic<bool>& InvincibleFlag()
    {
        static std::atomic<bool> value{ Enabled("CRASH_DEV_INVINCIBLE") };
        return value;
    }

    bool PlausibleObject(uint32_t pointer)
    {
        // Heap and image objects only (no MMIO windows, no null page); aligned.
        return (pointer & 3) == 0 && ((pointer >= 0x40000000 && pointer < 0x7F000000) || (pointer >= 0x82000000 && pointer < 0x83000000) ||
                                      (pointer >= 0xA0000000 && pointer < 0xC0000000));
    }

    // The behaviour's actor (+28) is one of the players (CGameDatabase +16 / +20, what
    // GetCurrentPlayer(0 / 1), sub_821E9790, returns).
    bool IsPlayerDamageable(uint32_t damageable)
    {
        uint32_t actor = LoadBE32(damageable + BEHAVIOUR_ACTOR);
        uint32_t database = GAME_DATABASE;
        if (!actor)
            return false;
        for (uint32_t slot = 0; slot < 2; slot++)
        {
            if (LoadBE32(database + GAME_DATABASE_PLAYERS + 4 * slot) == actor)
            {
                static uint32_t logged = 0;
                if (logged != damageable)
                {
                    logged = damageable;
                    Log("[dev] invincible: player %u damageable 0x%08X (actor 0x%08X)", slot + 1, damageable, actor);
                }
                return true;
            }
        }
        return false;
    }
}

namespace dev
{
    void SetInvincible(bool on)
    {
        InvincibleFlag() = on;
        Log("[dev] invincible %s", on ? "on" : "off");
    }

    bool Invincible() { return InvincibleFlag(); }

    // What the cheat checker (sub_821DB680) does on RT + X X Y A: flips m_CheatBigHeadCrash, then for
    // each player's render behaviour (behaviour 38) scales the joint "Head" by 2.2 (the drawable's
    // scale at +32/+36/+40, the joint index at +44; 0xFFFF = none, scale 1).
    int SetBigHead(PPCContext& ctx, uint8_t* base, bool on)
    {
        constexpr uint32_t HEAD_NAME = 0x8200E258;  // "Head"
        constexpr float BIG = 2.2f, NORMAL = 1.0f;
        PPCContext call = ctx;
        sub_82234188(call, base);
        uint32_t cheats = call.r3.u32;
        if (cheats)
            g_memory.Translate(cheats)[6] = on ? 1 : 0;
        auto storeFloat = [](uint32_t address, float value) {
            uint32_t bits;
            memcpy(&bits, &value, 4);
            StoreBE32(address, bits);
        };
        int players = 0;
        for (uint32_t slot = 0; slot < 2; slot++)
        {
            uint32_t actor = LoadBE32(GAME_DATABASE + GAME_DATABASE_PLAYERS + 4 * slot);
            if (!PlausibleObject(actor))
                continue;
            call = ctx;
            call.r3.u64 = actor;
            call.r4.u64 = 38;
            sub_82142608(call, base);
            uint32_t behaviour = call.r3.u32;
            uint32_t drawable = PlausibleObject(behaviour) ? LoadBE32(behaviour) : 0;
            if (!PlausibleObject(drawable))
                continue;
            uint32_t joint = 0xFFFF;
            if (on)
            {
                call = ctx;
                call.r3.u64 = HEAD_NAME;
                call.r4.u64 = 0;
                sub_822845A0(call, base);
                call.r4.u64 = call.r3.u32;
                call.r3.u64 = drawable;
                sub_822EE2B0(call, base);
                joint = call.r3.u32;
            }
            for (uint32_t axis = 0; axis < 3; axis++)
                storeFloat(drawable + 32 + 4 * axis, on ? BIG : NORMAL);
            StoreBE32(drawable + 44, joint);
            players++;
        }
        Log("[dev] big head %s (%d players)", on ? "on" : "off", players);
        return players;
    }
}

PPC_FUNC(sub_82110448)
{
    ctx.f1.f64 = mp::Hitpoints(ctx.r3.u32, ctx.f1.f64, ctx.r5.u32);  // online: the host's hitpoints win
    if (getenv("CRASH_DEV_DAMAGE_DEBUG") && ctx.f1.f64 < 0.0)
    {
        uint32_t d = ctx.r3.u32;
        std::string words;
        char item[64];
        for (uint32_t o = 0; o < 128; o += 4)
        {
            uint32_t w = LoadBE32(d + o);
            uint32_t vt = PlausibleObject(w) ? LoadBE32(w) : 0;
            snprintf(item, sizeof(item), " +%u=%08X(%08X)", o, w, vt);
            words += item;
        }
        uint32_t by = ctx.r5.u32, byActor = PlausibleObject(by) ? LoadBE32(by + 28) : 0;
        Log("[dev] damage %.2f to 0x%08X by 0x%08X (vtable %08X, actor %08X):%s", -ctx.f1.f64, d, by,
            PlausibleObject(by) ? LoadBE32(by) : 0, byActor, words.c_str());
    }
    if (InvincibleFlag() && ctx.f1.f64 < 0.0 && IsPlayerDamageable(ctx.r3.u32))
    {
        static int blocked = 0;
        if (blocked++ < 20)
        {
            float hitpoints;
            uint32_t bits = LoadBE32(LoadBE32(ctx.r3.u32 + DAMAGEABLE_HITPOINTS));
            memcpy(&hitpoints, &bits, 4);
            Log("[dev] invincible: blocked %.2f damage to 0x%08X (hitpoints %.2f)", -ctx.f1.f64, ctx.r3.u32, hitpoints);
        }
        ctx.f1.f64 = 0.0;
    }
    __imp__sub_82110448(ctx, base);
}

// CActionUnlockSkin::vf02 (sub_82215FD0), the objective step DO_UnlockSkin(skin): appends the skin
// to the unlocked list at 0x824A8218 (20 ints, -1 = free; ESkin_Crash 21 isn't announced). Logged
// for tests: "[dev] unlock skin N -> unlocked: a b c".
extern "C" PPC_FUNC(__imp__sub_82215FD0);
PPC_FUNC(sub_82215FD0)
{
    constexpr uint32_t UNLOCKED_SKINS = 0x824A8218;
    uint32_t skin = LoadBE32(ctx.r3.u32 + 4);
    __imp__sub_82215FD0(ctx, base);
    std::string list;
    for (uint32_t i = 0; i < 20; i++)
    {
        int32_t value = int32_t(LoadBE32(UNLOCKED_SKINS + 4 * i));
        if (value >= 0)
            list += " " + std::to_string(value);
    }
    Log("[dev] unlock skin %u -> unlocked:%s", skin, list.c_str());
}
