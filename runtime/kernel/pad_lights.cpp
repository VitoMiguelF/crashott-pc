// PlayStation pad light bars by what each player controls (pad_lights.h; runtime/NOTES.md "Barra de
// luz dos controles PlayStation").
//
// Where the game keeps it:
//   - CGameDatabase 0x824A76A4: +16 / +20 the actor each player controls (GetCurrentPlayer), +24 / +28
//     the other actor of a jack while it lasts (CJackingBehaviour::StartJacking sub_82120C08 sets it,
//     EndJacking sub_82120D40 clears it): of the two, one is the player's Crash, the other the titan.
//   - The players' Crashes: mp::PlayerCrash (0x8249D778[index] for the game's two); a Crash's actor is
//     its physics behaviour's (+0xDC) +96; its drawable is its render behaviour's (+0xE0) +92.
//   - An actor's behaviours: its list (pointers at +32, count at +48; the one actor in the static copy at
//     0x82482650 has them there, sub_820904D8) and its behaviour controller's (actor +0x1C) own pointers;
//     a behaviour's actor at +28. The fight tree behaviour (CCharacterFightTreeBehaviour, vtable
//     0x82010474; its +28 is something else) keeps its sub type (the template's
//     "Ratcicle", "Spike"...) as the game's 64-bit name hash at +32 (CFightTreeBehaviour::IsSubType
//     sub_821199C0; the hash is sub_822845A0's: h = h * 65599 ^ c). The CDamageableBehaviour (vtable
//     0x8201145C) points at its hitpoints (a float) from +64.
//   - The game controller of player 0 / 1: CFrontendManager (manager 6) +16580, -1 for none
//     (GetPlayersControllerNum sub_821D8C50).

#include "pad_lights.h"
#include "kernel.h"
#include "memory.h"
#include "pad_slots.h"
#include "pc_input.h"
#include "../diagnostics.h"
#include "../online/mp.h"
#include "../online/mp_colors.h"
#include "../platform/platform.h"
#include "../settings/settings.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace input::lights
{
    namespace
    {
        constexpr uint32_t GAME_DATABASE = 0x824A76A4;
        constexpr uint32_t MANAGERS = 0x824A7A24;  // manager i at +28 + 4i
        constexpr uint32_t FRONTEND_MANAGER = 6, FRONTEND_CONTROLLERS = 16580;
        constexpr uint32_t DAMAGEABLE_VTABLE = 0x8201145C;
        constexpr uint32_t CHARACTER_FIGHT_TREE_VTABLE = 0x82010474, FIGHT_TREE_VTABLE = 0x82011B14;
        constexpr uint32_t BEHAVIOURS_CACHE_OWNER = 0x82482650, BEHAVIOURS_CACHE_COUNT = 0x8248264C, BEHAVIOURS_CACHE = 0x82482508;
        constexpr uint64_t EVERY_MS = 30;
        constexpr uint32_t OFF_COLOUR = 0x0010C0;  // the option turned off: the PlayStation's own blue

        bool GuestPointer(uint32_t p) { return (p >= 0x40000000 && p < 0x7F000000) || (p >= 0xA0000000 && p < 0xC0000000); }
        uint64_t LoadBE64(uint32_t a) { return uint64_t(LoadBE32(a)) << 32 | LoadBE32(a + 4); }
        float LoadF(uint32_t a)
        {
            uint32_t bits = LoadBE32(a);
            float f;
            memcpy(&f, &bits, 4);
            return f;
        }

        // sub_822845A0, the game's name hash.
        constexpr uint64_t NameHash(const char* s)
        {
            uint64_t h = 0;
            for (; *s; ++s)
                h = h * 65599 ^ uint64_t(int64_t(int8_t(*s)));
            return h;
        }

        // The players' colours on the light bar, by multiplayer colour (mp_colors.h ids): the fur's
        // colours made bright enough for an LED (a dim one looks white on the pad).
        struct Named
        {
            const char* id;
            uint32_t rgb;
        };
        constexpr Named PLAYER_COLOURS[] = {
            { "orange", 0xFF4800 }, { "red", 0xFF0008 }, { "pink", 0xFF2A78 }, { "purple", 0x8C00FF },
            { "blue", 0x0020FF },   { "cyan", 0x00D8FF }, { "green", 0x10FF10 }, { "yellow", 0xFFC800 },
            { "brown", 0x8A3000 },  { "white", 0xFFFFFF }, { "gray", 0x505060 }, { "black", 0x180A30 },
        };

        // The titans by fight tree sub type (the templates' SetBehaviourData*: "Strong", "Ratcicle"...),
        // the same 15 as the stats EStatInt_*Jacked (30-44); the names the game shows in brackets.
        struct Titan
        {
            const char* subType;
            const char* name;
            uint32_t rgb;
            uint64_t hash;
        };
        constexpr Titan T(const char* subType, const char* name, uint32_t rgb) { return { subType, name, rgb, NameHash(subType) }; }
        constexpr Titan TITANS[] = {
            T("Spike", "Spike", 0x00FF30),               // green
            T("Shurtle", "Magmadon", 0xFF2800),          // lava
            T("Sludge", "Sludge", 0x80FF00),             // toxic slime
            T("Ratcicle", "Ratcicle", 0x60C8FF),         // ice blue
            T("Battler", "Battler", 0xB4B4C8),           // steel
            T("Grizzly", "Goar", 0xC00000),              // dark red
            T("Roller", "Rhinoroller", 0x5060A0),        // slate
            T("Parafox", "Snipe", 0xFF2F97),             // pink (its shots' light)
            T("Stinky", "Stench", 0xA000FF),             // purple
            T("Eelectric", "Ee-lectric", 0xFFE600),      // electric yellow
            T("Shellephant", "Shellephant", 0x9070C0),   // grey-purple
            T("Scorporilla", "Scorporilla", 0x3010FF),   // indigo
            T("Yuktopus", "Yuktopus", 0xFFB400),         // amber
            T("UkaUkaBoss", "Uka Uka", 0xFF0040),        // crimson
            T("NinaSpiderBot", "Nina's spider bot", 0x00E0FF),  // metallic cyan
        };
        constexpr uint32_t UNKNOWN_TITAN = 0xFFFFFF;

        enum class State { Menu, OnFoot, Titan, KnockedOut };
        const char* StateName(State s)
        {
            switch (s)
            {
            case State::Menu: return "no level";
            case State::OnFoot: return "on foot";
            case State::Titan: return "titan";
            default: return "knocked out";
            }
        }

        struct Look
        {
            State state = State::Menu;
            const Titan* titan = nullptr;
            uint64_t titanHash = 0;
            uint32_t rgb = 0;
            uint8_t number = 0;
        };

        // A behaviour of `actor` that `match` accepts, 0 if none. `ownActor`: only those whose actor
        // field (+28) is `actor` (the fight tree behaviour uses +28 for something else).
        template <typename F>
        uint32_t FindBehaviour(uint32_t actor, F match, bool ownActor = true)
        {
            if (!GuestPointer(actor))
                return 0;
            auto test = [&](uint32_t b) { return GuestPointer(b) && (!ownActor || LoadBE32(b + 28) == actor) && match(b); };
            // The actor's list (sub_820904D8): the pointers at +32, their count at +48 (byte); one
            // actor at a time has them in a static copy instead (its owner, count and pointers below).
            bool cached = LoadBE32(BEHAVIOURS_CACHE_OWNER) == actor;
            uint32_t count = cached ? LoadBE32(BEHAVIOURS_CACHE_COUNT) : *g_memory.Translate(actor + 48);
            uint32_t list = cached ? BEHAVIOURS_CACHE : LoadBE32(actor + 32);
            for (uint32_t i = 0; i < std::min<uint32_t>(count, 64) && (cached || GuestPointer(list)); ++i)
                if (uint32_t b = LoadBE32(list + 4 * i); test(b))
                    return b;
            // The behaviour controller's own pointers to some of them (mp_net.cpp's FindDamageable).
            uint32_t controller = LoadBE32(actor + 0x1C);
            for (uint32_t o = 0; GuestPointer(controller) && o < 0x400; o += 4)
                if (uint32_t b = LoadBE32(controller + o); test(b))
                    return b;
            return 0;
        }

        uint32_t CrashActor(uint32_t crash)
        {
            uint32_t physics = GuestPointer(crash) ? LoadBE32(crash + 0xDC) : 0;
            uint32_t actor = GuestPointer(physics) ? LoadBE32(physics + 96) : 0;
            return GuestPointer(actor) ? actor : 0;
        }

        // The titan the player whose Crash is `actor` rides, 0 if none.
        uint32_t JackedTitan(uint32_t actor)
        {
            for (uint32_t k = 0; k < 2; ++k)
            {
                uint32_t current = LoadBE32(GAME_DATABASE + 16 + 4 * k), other = LoadBE32(GAME_DATABASE + 24 + 4 * k);
                if (!GuestPointer(other) || !GuestPointer(current) || current == other)
                    continue;
                if (current == actor)
                    return other;
                if (other == actor)
                    return current;
            }
            return 0;
        }

        // The actor's fight tree sub type hash (0 if none found).
        uint64_t SubType(uint32_t actor)
        {
            uint64_t hash = 0;
            FindBehaviour(actor, [&](uint32_t b) {
                uint32_t vtable = LoadBE32(b);
                if (vtable != CHARACTER_FIGHT_TREE_VTABLE && vtable != FIGHT_TREE_VTABLE)
                    return false;
                hash = LoadBE64(b + 32);
                return true;
            }, false);
            return hash;
        }

        // The actor's hitpoints, -1 if it has no damageable behaviour.
        float Hitpoints(uint32_t actor)
        {
            uint32_t d = FindBehaviour(actor, [](uint32_t b) { return LoadBE32(b) == DAMAGEABLE_VTABLE; });
            uint32_t hp = d ? LoadBE32(d + 64) : 0;
            return GuestPointer(hp) ? LoadF(hp) : -1.0f;
        }

        uint32_t PlayerColour(uint32_t index, uint32_t crash)
        {
            uint32_t slot = mp::Online() ? mp::PlayerSlot(index) : index;
            const char* id = mp::colors::Get(mp::colors::ForSlot(slot)).id;
            // The normal orange with a skin on: the costume's colour.
            if (strcmp(id, "orange") == 0 && GuestPointer(crash))
                if (uint32_t render = LoadBE32(crash + 0xE0); GuestPointer(render))
                    if (uint8_t rgb[3]; mp::colors::SkinSwatch(LoadBE32(render + 92), rgb))
                        return uint32_t(rgb[0]) << 16 | uint32_t(rgb[1]) << 8 | rgb[2];
            for (const Named& c : PLAYER_COLOURS)
                if (strcmp(c.id, id) == 0)
                    return c.rgb;
            return PLAYER_COLOURS[0].rgb;
        }

        uint32_t Scale(uint32_t rgb, float f)
        {
            auto ch = [&](int shift) { return uint32_t(std::clamp(float((rgb >> shift) & 0xFF) * f + 0.5f, 0.0f, 255.0f)) << shift; };
            return ch(16) | ch(8) | ch(0);
        }

        bool Trace()
        {
            static const bool trace = [] {
                const char* v = getenv("CRASH_LIGHTBAR_TRACE");
                return v && *v && strcmp(v, "0") != 0;
            }();
            return trace;
        }

        Look Compute(uint32_t index, bool inLevel, bool paused, uint64_t now)
        {
            Look look;
            uint32_t crash = inLevel ? mp::PlayerCrash(index) : 0;
            look.rgb = PlayerColour(index, crash);
            look.number = uint8_t(std::min<uint32_t>((mp::Online() ? mp::PlayerSlot(index) : index) + 1, 8));
            if (uint32_t actor = CrashActor(crash))
            {
                look.state = State::OnFoot;
                if (uint32_t titan = JackedTitan(actor))
                {
                    look.state = State::Titan;
                    look.titanHash = SubType(titan);
                    look.rgb = UNKNOWN_TITAN;
                    for (const Titan& t : TITANS)
                        if (t.hash == look.titanHash)
                            look.titan = &t, look.rgb = t.rgb;
                    if (Trace())
                    {
                        static uint32_t s_logged[8];
                        if (s_logged[index & 7] != titan)
                        {
                            s_logged[index & 7] = titan;
                            Log("[lightbar] player %u: Crash actor %08X, titan %08X (database +16 %08X +20 %08X +24 %08X +28 %08X), sub type %016llX",
                                index + 1, actor, titan, LoadBE32(GAME_DATABASE + 16), LoadBE32(GAME_DATABASE + 20), LoadBE32(GAME_DATABASE + 24),
                                LoadBE32(GAME_DATABASE + 28), (unsigned long long)look.titanHash);
                        }
                    }
                }
                else
                {
                    float hp = Hitpoints(actor);
                    if (hp >= 0.0f && hp <= 0.0f)
                        look.state = State::KnockedOut;
                    static float s_hp[8] = { -2, -2, -2, -2, -2, -2, -2, -2 };
                    if (Trace() && s_hp[index & 7] != hp)
                    {
                        s_hp[index & 7] = hp;
                        Log("[lightbar] player %u: hitpoints %.1f (Crash actor %08X)", index + 1, hp, actor);
                    }
                }
            }
            if (paused)
            {
                // Breathing: down to 15 % and back every 2.4 s.
                float t = float(now % 2400) / 2400.0f;
                look.rgb = Scale(look.rgb, 0.15f + 0.85f * (0.5f + 0.5f * std::cos(t * 6.2831853f)));
            }
            else if (look.state == State::KnockedOut)
                look.rgb = (now / 160) % 2 ? 0xFF0000 : 0x200000;  // red blinking
            return look;
        }
    }

    void Tick()
    {
        static uint64_t s_last = 0;
        static bool s_wasOn = false;
        static Look s_logged[4];
        uint64_t now = plat::TickCountMs();
        if (now - s_last < EVERY_MS)
            return;
        s_last = now;
        if (!settings::Get().controls.lightbar)
        {
            // Turned off: the pads back to the PlayStation's blue, then left alone.
            if (s_wasOn)
                for (uint32_t user = 0; user < 4; ++user)
                    slots::SetLight(user, OFF_COLOUR, 0);
            s_wasOn = false;
            return;
        }
        s_wasOn = true;
        bool inLevel = GuestPointer(LoadBE32(GAME_DATABASE + 16));
        bool paused = inLevel && pc::InMenus();
        int controllerOf[2] = { -1, -1 };
        if (uint32_t frontend = LoadBE32(MANAGERS + 28 + 4 * FRONTEND_MANAGER); GuestPointer(frontend))
            for (int p = 0; p < 2; ++p)
                controllerOf[p] = int32_t(LoadBE32(frontend + FRONTEND_CONTROLLERS + 4 * p));
        Look looks[4];
        bool computed[4] = {};
        for (uint32_t user = 0; user < 4; ++user)
        {
            // The game's player on this controller; else (no level yet, a pad that has not joined) the
            // player this pad would be (pad_slots.h): its colour and number.
            int index = -1;
            for (int p = 0; p < 2; ++p)
                if (controllerOf[p] == int(user))
                    index = p;
            for (int p = 0; p < 4 && index < 0; ++p)
                if (slots::UserOfPlayer(p) == user)
                    index = p;
            if (index < 0)
                continue;
            if (!computed[index])
                looks[index] = Compute(uint32_t(index), inLevel && index < 2, paused, now), computed[index] = true;
            const Look& look = looks[index];
            slots::SetLight(user, look.rgb, look.number);
            Look& logged = s_logged[user];
            if (look.state != logged.state || look.titanHash != logged.titanHash)
            {
                if (look.state == State::Titan)
                    Log("[lightbar] controller %u (player %d): titan %s, light %06X", user, index + 1,
                        look.titan ? look.titan->name : "unknown", look.rgb);
                else
                    Log("[lightbar] controller %u (player %d): %s", user, index + 1, StateName(look.state));
                logged = look;
            }
        }
    }
}
