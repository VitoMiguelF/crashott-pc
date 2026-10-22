// Music / effects / voice volume of the settings screen, applied live through the game's own
// volume categories (audio/game_volumes.h).
//
// The game keeps three user volumes, the ones its pause menu's DIALOGUE / MUSIC / SFX stars set
// (saved with the game): sub_82241820 returns the object holding them (global pointer at
// 0x824A8FB0); +4, +8 and +12 point to the three category objects, each with the user volume as
// a float at +36. sub_82241968 / sub_822419A8 / sub_822419E8 clamp a value to [0, 1] and store it
// there through sub_82241838 (stfs f1,36(r4)); sub_82241840/48/50 read it back through
// sub_82241830 (lfs f1,36(r4)). The sound system multiplies it with the active mixer snapshots
// on every update, so a new value is heard right away; the gain goes with the square of the
// product (measured with the old mixers.p3d patch), so the settings' percent p is applied as
// sqrt(p): 50% is half the amplitude.
//
// The game's own stars are no longer shown (the settings screen covers its options screen), so
// the value stored is the setting's factor alone, whatever the game sets (its defaults, a loaded
// save): the settings are the only volume control. The game still reads back its own value.

#include "game_volumes.h"
#include "../diagnostics.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" PPC_FUNC(__imp__sub_82241830);
extern "C" PPC_FUNC(__imp__sub_82241838);

namespace audio::volumes
{
    namespace
    {
        constexpr uint32_t HOLDER_POINTER = 0x824A8FB0;  // sub_82241820
        constexpr uint32_t VALUE_OFFSET = 36;
        // Holder offsets of the categories, as the setters use them.
        constexpr uint32_t CATEGORY_OFFSETS[3] = { 4, 8, 12 };
        // Which setting each holder slot follows (see SlotOf).
        constexpr Category SLOT_CATEGORY[3] = { Category::Music, Category::Voice, Category::Effects };

        std::mutex g_mutex;
        float g_factor[3] = { 1.0f, 1.0f, 1.0f };  // by Category (under g_mutex)
        uint32_t g_holder = 0;
        uint32_t g_objects[3] = {};
        float g_gameValue[3] = { 1.0f, 1.0f, 1.0f };  // by slot: what the game set
        bool g_known[3] = {};

        uint32_t Load(uint32_t guest) { return LoadBE32(guest); }

        float LoadFloat(uint32_t guest)
        {
            uint32_t bits = LoadBE32(guest);
            float value;
            memcpy(&value, &bits, 4);
            return value;
        }

        void StoreFloat(uint32_t guest, float value)
        {
            uint32_t bits;
            memcpy(&bits, &value, 4);
            StoreBE32(guest, bits);
        }

        // Re-reads the holder and its category pointers (created late, or again); true once all
        // three exist. Caller holds g_mutex.
        bool Refresh()
        {
            uint32_t holder = Load(HOLDER_POINTER);
            uint32_t objects[3] = {};
            for (int i = 0; holder && i < 3; i++)
                objects[i] = Load(holder + CATEGORY_OFFSETS[i]);
            if (holder != g_holder || memcmp(objects, g_objects, sizeof(objects)) != 0)
            {
                g_holder = holder;
                memcpy(g_objects, objects, sizeof(objects));
                for (bool& known : g_known)
                    known = false;
                if (holder && objects[0] && objects[1] && objects[2])
                {
                    Log("[audio] game volume categories at 0x%08X: 0x%08X 0x%08X 0x%08X", holder, objects[0], objects[1], objects[2]);
                    if (getenv("CRASH_VOLUME_TRACE"))
                    {
                        for (int i = 0; i < 3; i++)
                        {
                            char text[256] = {};
                            size_t used = 0;
                            for (uint32_t w = 0; w < 16; w++)
                                used += snprintf(text + used, sizeof(text) - used, " %08X", Load(objects[i] + w * 4));
                            Log("[audio]   category +%u:%s", CATEGORY_OFFSETS[i], text);
                        }
                    }
                }
            }
            return holder && objects[0] && objects[1] && objects[2];
        }

        int SlotOf(uint32_t object)
        {
            for (int i = 0; i < 3; i++)
            {
                if (object && object == g_objects[i])
                    return i;
            }
            return -1;
        }

        // Writes our factor into every category (the game's own value is kept for its reads); caller holds g_mutex.
        void ApplyAll()
        {
            for (int i = 0; i < 3; i++)
            {
                if (!g_objects[i])
                    continue;
                if (!g_known[i])
                {
                    g_gameValue[i] = LoadFloat(g_objects[i] + VALUE_OFFSET);  // set by the game, not by us yet
                    g_known[i] = true;
                }
                StoreFloat(g_objects[i] + VALUE_OFFSET, g_factor[int(SLOT_CATEGORY[i])]);
            }
        }
    }

    void SetPercent(int music, int effects, int voice)
    {
        auto factor = [](int percent) { return std::sqrt(float(std::clamp(percent, 0, 100)) / 100.0f); };
        std::lock_guard lock(g_mutex);  // the factors change together with the stored values
        g_factor[int(Category::Music)] = factor(music);
        g_factor[int(Category::Effects)] = factor(effects);
        g_factor[int(Category::Voice)] = factor(voice);
        if (Refresh())
            ApplyAll();
    }

    void Tick()
    {
        if (!Load(HOLDER_POINTER))
            return;
        std::lock_guard lock(g_mutex);
        if (!Refresh())
            return;
        if (!g_known[0] || !g_known[1] || !g_known[2])
        {
            ApplyAll();  // just appeared
            return;
        }
        // A value the game wrote without its setter (e.g. while building the object): keep it as
        // the game's value and store the factor again.
        for (int i = 0; i < 3; i++)
        {
            if (!g_objects[i] || !g_known[i])
                continue;
            float expected = g_factor[int(SLOT_CATEGORY[i])];
            float current = LoadFloat(g_objects[i] + VALUE_OFFSET);
            if (current != expected)
            {
                g_gameValue[i] = current;
                StoreFloat(g_objects[i] + VALUE_OFFSET, expected);
            }
        }
    }
}

using namespace audio::volumes;

// stfs f1,36(r4): the game stores a category's user volume.
PPC_FUNC(sub_82241838)
{
    std::lock_guard lock(g_mutex);
    Refresh();
    int slot = SlotOf(ctx.r4.u32);
    if (slot < 0)
    {
        __imp__sub_82241838(ctx, base);
        return;
    }
    g_gameValue[slot] = float(ctx.f1.f64);
    g_known[slot] = true;
    StoreFloat(ctx.r4.u32 + VALUE_OFFSET, g_factor[int(SLOT_CATEGORY[slot])]);
}

// lfs f1,36(r4): the game reads it (its menu's stars, its saves): its own value, not ours.
PPC_FUNC(sub_82241830)
{
    std::lock_guard lock(g_mutex);
    int slot = SlotOf(ctx.r4.u32);
    if (slot < 0 || !g_known[slot])
    {
        __imp__sub_82241830(ctx, base);
        return;
    }
    ctx.f1.f64 = double(g_gameValue[slot]);
}
