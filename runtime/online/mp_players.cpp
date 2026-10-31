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
// (sub_82154A58) and gives it its controller. The game's own limit is 4 players: 4 controllers (CInputManager
// devices) and 4 AI targets (CAIManager). Players 5-8 (experimental) get controllers 4-7 of ours
// (ExtraInput below) and take turns with the others as the AI's 4 targets (BalanceAiTargets); the game
// sees them with player index 2 / 3 (players 3 / 4's), the only ones past its two-entry tables it
// already handles: its per-player byte flags (CGameDatabase +32 + index) end at index 3, the next bytes
// are an int of its.
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
#include "../kernel/input.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../kernel/xinput_types.h"

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
extern "C" PPC_FUNC(__imp__sub_821ED5B0);  // CInputManager::vf10: updates its CInputDevices
extern "C" PPC_FUNC(__imp__sub_821ED160);  // CInputDevice: update of the frame (f1 = seconds)
PPC_EXTERN_FUNC(sub_821E9C78);             // CInputDevice: an input history entry's constructor (16 bytes)
PPC_EXTERN_FUNC(sub_821EB3F0);             // CInputDevice: a stick detector's constructor (44 bytes)
PPC_EXTERN_FUNC(sub_821ECDD0);             // CInputDevice::CInputDevice(this, gamepad, 40, controller number)
PPC_EXTERN_FUNC(sub_821EC1C8);             // CInputDevice: attaches to a controller (its inputs call the device back)
PPC_EXTERN_FUNC(sub_82289FF8);             // controller: a stick's dead zone (short* x, short* y, f1)
PPC_EXTERN_FUNC(sub_8228A110);             // controller input: its value from the pad, the device's callback on a change
PPC_EXTERN_FUNC(sub_82090370);             // CActor: reset (vtable +0x3c), what a respawn does
PPC_EXTERN_FUNC(sub_82142608);             // CActor::GetBehaviour
PPC_EXTERN_FUNC(sub_82143698);             // CPhysicsBehaviour: collision on / off
PPC_EXTERN_FUNC(sub_8220F1A0);             // the players' reset position (DO_SetPlayerResetPosition's locator): r3 = a 4x4 matrix out, true if set
PPC_EXTERN_FUNC(sub_82098E68);             // CAIManager: removes an AI target (RemoveTarget)
PPC_EXTERN_FUNC(sub_82154A58);             // Crash: shows a player (behaviours on, collision, AI target), as leaving the backpack does
extern "C" PPC_FUNC(__imp__sub_821ED6C0);  // CInputManager: registers a player's input binding with its controller's device
extern "C" PPC_FUNC(__imp__sub_82219908);  // trigger-volume condition (WAIT_TriggerVolume): true when it is met
PPC_EXTERN_FUNC(sub_8213B280);             // CIntersectionBehaviour-ish: is the actor r4 inside the volume r3

namespace
{
    constexpr uint32_t CRASH_TABLE = 0x8249D778;     // Crash* [2], by player index
    constexpr uint32_t PLAYERS_SHARED = 0x8249D78C;  // the players' shared object (~Crash releases it with the last one)
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

    // Players 3-8: native Crashes with player index 2-7 (ours; the game's byte at Crash +292 says 2 or 3, see
    // GameIndex) (the game's tables have 2 entries; index 2 lands on the backpack pointer, which the
    // constructor hook restores). Game thread only.
    struct Extra
    {
        uint32_t crash = 0;
        uint8_t index = 0;
        uint64_t since = 0;
        uint64_t shownAt = 0;  // its transform is the new place a frame later: in play 300 ms after
        bool onFoot = false;
    };
    std::vector<Extra> g_extras;  // players 2-8 (index 1-7) to be shown, in spawn order

    constexpr uint32_t KNOCKOUT_SECONDS = 0x82002328;  // float: the co-op knock-out time
    struct Knockout
    {
        uint32_t crash = 0;
        uint32_t index = 0;
        uint64_t until = 0;
    };
    std::vector<Knockout> g_knockouts;
    std::vector<uint32_t> g_departed;  // the Crashes of players who left the session (RemovePlayer)
    uint64_t g_revivedAt[mp::MAX_PLAYERS] = {};
    uint64_t g_gatherAt = 0;  // CinematicDone: when the players go next to the host's (0 = not due)

    // Player 1 sits at the parking place of the hidden Crashes for the first frames of a level, until the
    // level's spawn moves it: in play 500 ms after PlayersTick first sees it in a level.
    uint32_t g_p1 = 0;
    uint64_t g_p1Since = 0;

    void KnockOut(uint32_t index, const char* why)
    {
        uint32_t crash = index < mp::MAX_PLAYERS ? mp::PlayerCrash(index) : 0;
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
    constexpr uint32_t DEVICE_ACTOR = 508;  // CInputDevice: the actor whose input maps its update runs

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

    // Every player's device (its binding's) drives that player's actor (DEVICE_ACTOR).
    void OwnDevices()
    {
        for (uint32_t i = 0; i < mp::MAX_PLAYERS; ++i)
        {
            uint32_t crash = mp::PlayerCrash(i), binding = crash ? LoadBE32(crash + 176) : 0;
            uint32_t device = GuestPointer(binding) ? LoadBE32(binding) : 0;
            if (GuestPointer(device) && LoadBE32(device) == INPUT_DEVICE_VTABLE && GuestPointer(LoadBE32(crash + 156)))
                StoreBE32(device + DEVICE_ACTOR, LoadBE32(crash + 156));
        }
    }

    // The player index the game sees for a Crash past the second: players 3 and 4 have 2 and 3; players 5-8
    // (experimental) share them (5 and 7 have 2, 6 and 8 have 3). The game's per-player byte flags
    // (CGameDatabase +32 + index: set by Crash::vf06, cleared by SignalDead, read by vf15 and others) end at
    // index 3; index 4-7 would write into the int after them. Its player-index compares only ask for 0, 1
    // or 2, and its player -> controller table of 2 (sub_821D8C50) is already read past for 2 / 3.
    uint8_t GameIndex(uint32_t index) { return uint8_t(index < 4 ? index : 2 + (index & 1)); }

    // Controllers 4-7 for players 5-8. The game polls 4 XInput users (the controller system at 0x824B46DC:
    // 4 controllers of 48 bytes from +24; +16 the user, +28 its inputs, 60 bytes each, +32 its outputs,
    // 36 bytes each, +36 the type, +40 connected) and CInputManager (manager 7) has 4 CInputDevices; a
    // controller's input (sub_8228A110, from the XINPUT_GAMEPAD the poll read) calls back the device
    // attached to it (+52 the callback, +56 the device's input) on a change, and the manager's update
    // (vf10) updates each device (sub_821ED160: the input maps of the Crash it drives).
    // Here each extra controller is a copy of controller 0 (type, inputs, outputs) with its own inputs
    // and a CInputDevice built the way CInputManager builds its own (524 bytes, constructor, vf0 = AddRef)
    // and attached to it (sub_821EC1C8: the game's own attach). Neither is in the game's
    // lists: after the manager's update, the pad of controller 4-7 (input::ReadExtraController) goes
    // through the copied inputs, then the device is updated. The Crash's input binding (+176, its +0 the
    // device) points to it, as players 3 / 4 get devices 2 / 3. Made once, kept for good (the device
    // holds a reference of ours, so a Crash releasing its binding never deletes it).
    // In a level the game's heaps are nearly full: operator new had room for one device, and the
    // constructor's own allocations (each input's history, 4 + 16 x 40 bytes at +48 + 12 i: buffer, size,
    // position; the stick detectors at +492..+504) mostly came back null. So the device is in the runtime's
    // memory and what its constructor could not allocate is made here, the way it makes them (never freed,
    // as the device never is).
    constexpr uint32_t CONTROLLER_SYSTEM = 0x824B46DC;  // -> 4 controllers from +24, 48 bytes each
    constexpr uint32_t CONTROLLER_TYPES = 0x824733B4;   // 28 bytes per type: +12 inputs, +20 outputs
    constexpr uint32_t STICK_DEAD_ZONE = 0x8200DD94;    // float: what the poll passes to sub_82289FF8
    constexpr uint32_t DEVICE_INPUTS = 18;
    struct ExtraInput
    {
        uint32_t controller = 0;  // 48 bytes + the pad (12 bytes, big-endian) at +48
        uint32_t device = 0;
        uint32_t inputs = 0;
        bool failed = false;
    };
    ExtraInput g_inputs[mp::MAX_PLAYERS - mp::GAME_PLAYERS];
    double g_inputSeconds = 1.0 / 30;  // the last update's time step (CInputManager::vf10 passes it on)

    uint32_t ZeroAlloc(uint32_t size)
    {
        uint32_t p = GuestAlloc(size);
        memset(g_memory.Translate(p), 0, size);
        return p;
    }

    // What the device's constructor could not allocate (see above); how many parts were made here.
    uint32_t BackfillDevice(PPCContext& ctx, uint8_t* base, uint32_t device)
    {
        uint32_t made = 0;
        for (uint32_t i = 0; i < DEVICE_INPUTS; ++i)
        {
            uint32_t history = device + 48 + i * 12, size = LoadBE32(history + 4);
            if (LoadBE32(history) || !size || size > 1024)
                continue;
            uint32_t buffer = ZeroAlloc(4 + 16 * size);  // sub_821EA0E0: the count, then the entries
            StoreBE32(buffer, size);
            for (uint32_t e = 0; e < size; ++e)
            {
                PPCContext call = ctx;
                call.r3.u64 = buffer + 4 + e * 16;
                sub_821E9C78(call, base);
            }
            StoreBE32(history, buffer + 4);
            ++made;
        }
        for (uint32_t at : {492u, 496u})  // sub_821EB660: two stick detectors
            if (!LoadBE32(device + at))
            {
                uint32_t detector = ZeroAlloc(44);
                PPCContext call = ctx;
                call.r3.u64 = detector;
                sub_821EB3F0(call, base);
                StoreBE32(device + at, detector);
                ++made;
            }
        for (uint32_t at : {500u, 504u})  // and two of {the float at 0x82000DC8, 3 bytes}
            if (!LoadBE32(device + at))
            {
                uint32_t part = ZeroAlloc(8);
                StoreBE32(part, LoadBE32(0x82000DC8));
                StoreBE32(device + at, part);
                ++made;
            }
        return made;
    }

    uint32_t ExtraDevice(PPCContext& ctx, uint8_t* base, uint32_t controllerNumber)
    {
        ExtraInput& in = g_inputs[controllerNumber - mp::GAME_PLAYERS];
        if (in.device || in.failed)
            return in.device;
        uint32_t system = LoadBE32(CONTROLLER_SYSTEM), first = system ? system + 24 : 0;
        if (!first || !*g_memory.Translate(first + 40))
            return 0;  // controller 0 not up yet
        uint32_t type = LoadBE32(first + 36);
        uint32_t inputs = LoadBE32(CONTROLLER_TYPES + type * 28 + 12), outputs = LoadBE32(CONTROLLER_TYPES + type * 28 + 20);
        uint32_t from = LoadBE32(first + 28), fromOutputs = LoadBE32(first + 32);
        in.failed = true;  // one try
        if (type > 16 || !inputs || inputs > 64 || outputs > 64 || !from || (outputs && !fromOutputs))
        {
            Log("[mp] controller %u (experimental): controller 0 not as expected (type %u, %u inputs), players 5-8 without it",
                controllerNumber, type, inputs);
            return 0;
        }
        uint32_t device = ZeroAlloc(524), controller = ZeroAlloc(48 + 16), copies = ZeroAlloc(inputs * 60), outputCopies = ZeroAlloc(std::max(outputs, 1u) * 36);
        memcpy(g_memory.Translate(controller), g_memory.Translate(first), 48);
        memcpy(g_memory.Translate(copies), g_memory.Translate(from), inputs * 60);
        if (outputs)
            memcpy(g_memory.Translate(outputCopies), g_memory.Translate(fromOutputs), outputs * 36);
        for (uint32_t i = 0; i < inputs; ++i)
        {
            uint32_t input = copies + i * 60;
            StoreBE32(input + 52, 0);  // no callback until the device attaches
            StoreBE32(input + 56, 0);
            float none = -1000.0f;  // so the first update reports every value to the device
            uint32_t bits;
            memcpy(&bits, &none, 4);
            StoreBE32(input + 24, bits);
        }
        StoreBE32(controller + 16, controllerNumber);
        StoreBE32(controller + 28, copies);
        StoreBE32(controller + 32, outputCopies);
        PPCContext call = ctx;
        call.r3.u64 = device;
        call.r4.u64 = 1;
        call.r5.u64 = 40;
        call.r6.u64 = controllerNumber;
        sub_821ECDD0(call, base);
        uint32_t made = BackfillDevice(ctx, base, device);
        uint32_t addRef = LoadBE32(LoadBE32(device));
        if (PPCFunc* f = addRef >= PPC_CODE_BASE && addRef < PPC_CODE_BASE + PPC_CODE_SIZE ? PPC_LOOKUP_FUNC(base, addRef) : nullptr)
        {
            call = ctx;
            call.r3.u64 = device;
            f(call, base);
        }
        call = ctx;
        call.r3.u64 = device;
        call.r4.u64 = controller;
        sub_821EC1C8(call, base);
        uint32_t attached = 0;
        for (uint32_t i = 0; i < inputs; ++i)
            attached += LoadBE32(copies + i * 60 + 52) != 0;
        in = {controller, device, inputs, false};
        Log("[mp] controller %u (experimental): device %08X on a copy of controller 0 (type %u, %u inputs, %u attached; %u parts "
            "made outside the game's heaps)", controllerNumber, device, type, inputs, attached, made);
        return device;
    }

    // After the game's devices: controllers 4-7 read their pads and update their devices.
    void UpdateExtraInputs(PPCContext& ctx, uint8_t* base)
    {
        for (uint32_t k = 0; k < mp::MAX_PLAYERS - mp::GAME_PLAYERS; ++k)
        {
            const ExtraInput& in = g_inputs[k];
            if (!in.device)
                continue;
            XINPUT_GAMEPAD pad{};
            input::ReadExtraController(mp::GAME_PLAYERS + k, &pad);
            uint32_t p = in.controller + 48;  // as the poll reads it: buttons, triggers, sticks
            StoreBE16(p, pad.wButtons);
            *g_memory.Translate(p + 2) = pad.bLeftTrigger;
            *g_memory.Translate(p + 3) = pad.bRightTrigger;
            StoreBE16(p + 4, uint16_t(pad.sThumbLX));
            StoreBE16(p + 6, uint16_t(pad.sThumbLY));
            StoreBE16(p + 8, uint16_t(pad.sThumbRX));
            StoreBE16(p + 10, uint16_t(pad.sThumbRY));
            for (uint32_t stick = 0; stick < 2; ++stick)
            {
                PPCContext call = ctx;
                call.r3.u64 = p + 4 + stick * 4;
                call.r4.u64 = p + 6 + stick * 4;
                call.f1.f64 = LoadF(STICK_DEAD_ZONE);
                sub_82289FF8(call, base);
            }
            for (uint32_t i = 0; i < in.inputs; ++i)
            {
                PPCContext call = ctx;
                call.r3.u64 = LoadBE32(in.controller + 28) + i * 60;
                call.r4.u64 = p;
                sub_8228A110(call, base);
            }
            PPCContext call = ctx;
            call.r3.u64 = in.device;
            call.f1.f64 = g_inputSeconds;
            __imp__sub_821ED160(call, base);
        }
    }
}

PPC_FUNC(sub_821ED160)
{
    g_inputSeconds = ctx.f1.f64;
    __imp__sub_821ED160(ctx, base);
}

PPC_FUNC(sub_821ED5B0)
{
    __imp__sub_821ED5B0(ctx, base);
    if (mp::Enabled())
        UpdateExtraInputs(ctx, base);
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
        // The game gives every Crash past the second index 2: players 3-8 get 2-7 here (GameIndex for the game).
        index = uint8_t(2 + ExtraCount());
        *g_memory.Translate(bc + 292) = GameIndex(index);
        // Its co-op state came from 0x8245FA9C[index], past the 2-entry table: not joined.
        uint32_t state = LoadBE32(bc + 300);
        StoreBE32(bc + 300, 0);
        Log("[mp] extra Crash %08X: co-op state read past the table %08X, now 0", bc, state);
        g_extras.push_back({bc, index, mp::NowMs(), false});
        Log("[mp] extra Crash %08X got player index %u (game index %u; backpack pointer kept at %08X)", bc, index,
            GameIndex(index), backpack);
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
// Each Crash spawned adds itself; with more than 4 players the 5th is refused (BalanceAiTargets below
// gives players 5-8 their turns) and an actor already there is not added twice.
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
    // (0x8249D78C: PrepareForDelete, vf1, then 0). A level's unload destroyed players 1 and 2 before the third
    // Crash, which then used it (a crash leaving any level with 3+ players): while another extra Crash is alive
    // an empty entry holds it, so the last Crash to go releases the object. With 4+ players the first extra
    // to go after players 1 and 2 released it and the next one released it again (PrepareForDelete of null: the
    // S20 FE closed when its session ended in a level with 4 players): once it is gone, an entry holds this
    // Crash meanwhile, so nothing is released twice.
    uint32_t index = *g_memory.Translate(crash + 292);
    bool own = index < 2 && LoadBE32(CRASH_TABLE + index * 4) == crash;  // the destructor clears that entry
    bool emptyAfter = true;
    for (uint32_t i = 0; i < 2; ++i)
        if (LoadBE32(CRASH_TABLE + i * 4) && !(own && i == index))
            emptyAfter = false;
    uint32_t keep = 0, entry = own ? index ^ 1 : 0;
    if (emptyAfter)
    {
        for (const Extra& e : g_extras)
            if (e.index >= 2 && GuestPointer(e.crash))
            {
                keep = e.crash;
                break;
            }
        if (!keep && !LoadBE32(PLAYERS_SHARED))
            keep = crash;  // already released
    }
    if (keep)
    {
        StoreBE32(CRASH_TABLE + entry * 4, keep);
        Log("[mp] Crash %08X (player %u) goes before %s: the shared object %s", crash, index + 1,
            keep == crash ? "nobody" : "an extra Crash", keep == crash ? "is not released again" : "stays");
    }
    __imp__sub_82153B08(ctx, base);
    if (keep)
        StoreBE32(CRASH_TABLE + entry * 4, 0);
}

PPC_FUNC(sub_8220D830)
{
    if (!Managed())
    {
        __imp__sub_8220D830(ctx, base);
        return;
    }
    uint32_t index = LoadBE32(ctx.r3.u32 + 4);
    // Players 4-8 share the game's index 3 (and 3 / 5 / 7 its 2: GameIndex): the one of them out of hitpoints.
    if (index >= 2 && index < 4)
        for (uint32_t i = index; i < mp::MAX_PLAYERS; i += 2)
            if (mp::PlayerCrash(i) && !mp::HitpointsHeld(i) && mp::PlayerHitpoints(i) > -0.5f && mp::PlayerHitpoints(i) <= 0.0f)
            {
                index = i;
                break;
            }
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
    // Players 3-8 (CRASH_MP_PLAYERS > 2) are spawned the way the levels spawn players 1 and 2
    // (objectives.lua: pObjective:DO_SpawnPlayer(n, name, locator)), by one objective of the game's own
    // system, once per level, when player 1 is on foot; every machine names them the same
    // (ref_Crash_P3 .. ref_Crash_P8: the network id).
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
                return e.onFoot && e.index < MAX_PLAYERS && NowMs() - e.shownAt >= 300;
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

    // Players 2-8 are built like an unjoined player 2: hidden, parked, its behaviours off. Once player 1
    // is on foot each is shown the way leaving the backpack shows a player (sub_82154A58 detaches it from
    // the backpack Crash's attacher, so that pointer is player 1 for the call), set on foot (co-op
    // state 2) and put next to player 1.
    bool HitpointsHeld(uint32_t index)
    {
        for (const Knockout& k : g_knockouts)
            if (k.index == index)
                return true;
        return index < MAX_PLAYERS && g_revivedAt[index] && NowMs() - g_revivedAt[index] < 1500;
    }

    // A knocked-out player whose time is up: reset (the respawn's reset keeps two of the actor's values,
    // behaviours 0xF / 0x11, when the first is above the threshold, as sub_8220D830 does) and put at the
    // level's reset position (the last checkpoint the players reached, where the game resets a lone player:
    // sub_8220D6F8), 1.2 apart per slot; before the first one, next to the teammate on foot with the lowest
    // slot (itself, where it lies, when nobody is). Next to a teammate, the host came back at the level's start
    // beside a player who had stayed there.
    void Revive(PPCContext& ctx, uint8_t* base, const Knockout& k)
    {
        uint32_t physics = LoadBE32(k.crash + 0xDC), actor = GuestPointer(physics) ? LoadBE32(physics + 96) : 0;
        if (!GuestPointer(actor))
            return;
        uint32_t target = 0, targetSlot = ~0u;
        for (uint32_t i = 0; i < MAX_PLAYERS; ++i)
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
        static uint32_t reset = GuestAlloc(64);
        call = ctx;
        call.r3.u64 = reset;
        if (reset)
            sub_8220F1A0(call, base);
        if (reset && call.r3.u8)
        {
            float x = LoadF(reset + 48) + 1.2f * PlayerSlot(k.index), y = LoadF(reset + 52) + 0.5f, z = LoadF(reset + 56);
            bool moved = TeleportCrash(ctx, base, k.crash, x, y, z);
            Log("[mp] player %u back at the reset position (%.1f, %.1f, %.1f)%s, hitpoints %.1f of %.1f", k.index + 1, x, y, z,
                moved ? "" : ", not moved", PlayerHitpoints(k.index), PlayerMaxHitpoints(k.index));
            return;
        }
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

    // More than 4 players (experimental): the CAIManager holds 4 targets (36-byte slots from +4: +0 the
    // actor, then two lists of the AI after it, +4..+8 and +20..+24; the count at +0x94) and its update
    // (sub_820985E0) keeps stack arrays per slot, so it cannot grow. Every 0.5 s a target not in play
    // (knocked out, gone) gives its place to a player waiting, a free place goes to the one waiting
    // longest, and every 8 s the target nobody is after (both lists empty) that has been one longest makes
    // room for the one waiting longest: enemies go after every player in turn, not only the first 4.
    void BalanceAiTargets(PPCContext& ctx, uint8_t* base)
    {
        static uint64_t next, lastTurn;
        static uint64_t since[MAX_PLAYERS];  // when the player became a target, or stopped being one
        static bool wasTarget[MAX_PLAYERS];
        uint64_t now = NowMs();
        if (Players() <= GAME_PLAYERS || now < next)
            return;
        next = now + 500;
        uint32_t ai = LoadBE32(MANAGERS_TABLE + (2 + 7) * 4);
        if (!GuestPointer(ai))
            return;
        auto place = [&](uint32_t actor) {
            uint32_t count = std::min<uint32_t>(LoadBE32(ai + 0x94), 4);
            for (uint32_t i = 0; i < count; ++i)
                if (LoadBE32(ai + 4 + i * 36) == actor)
                    return int(i);
            return -1;
        };
        auto change = [&](uint32_t actor, bool add) {
            PPCContext call = ctx;
            call.r3.u64 = ai;
            call.r4.u64 = actor;
            if (add)
                sub_82098A38(call, base);
            else
                sub_82098E68(call, base);
        };
        uint32_t actors[MAX_PLAYERS] = {};
        bool ready[MAX_PLAYERS] = {};
        std::vector<uint32_t> waiting;
        for (uint32_t i = 0; i < MAX_PLAYERS; ++i)
        {
            uint32_t crash = PlayerCrash(i);
            actors[i] = crash ? LoadBE32(crash + 156) : 0;
            ready[i] = GuestPointer(actors[i]) && PlayerOnFoot(i) && !HitpointsHeld(i);
            bool target = actors[i] && place(actors[i]) >= 0;
            if (target != wasTarget[i])
                wasTarget[i] = target, since[i] = now;
            if (ready[i] && !target)
                waiting.push_back(i);
        }
        std::stable_sort(waiting.begin(), waiting.end(), [](uint32_t a, uint32_t b) { return since[a] < since[b]; });
        size_t turn = 0;
        auto give = [&](int out, const char* why) {
            uint32_t in = waiting[turn++];
            if (out >= 0)
                change(actors[out], false);
            change(actors[in], true);
            if (place(actors[in]) < 0)
                return;
            if (out >= 0)
                wasTarget[out] = false, since[out] = now;
            wasTarget[in] = true, since[in] = now;
            Log("[mp] AI target: player %u in%s%s (%s)", in + 1, out >= 0 ? ", player " : "", out >= 0 ? std::to_string(out + 1).c_str() : "", why);
        };
        for (uint32_t i = 0; i < MAX_PLAYERS && turn < waiting.size(); ++i)
            if (wasTarget[i] && !ready[i] && actors[i])
                give(int(i), "out of play");
        while (turn < waiting.size() && LoadBE32(ai + 0x94) < 4)
            give(-1, "a free place");
        if (turn >= waiting.size() || now - lastTurn < 8000)
            return;
        int longest = -1;
        for (uint32_t i = 0; i < MAX_PLAYERS; ++i)
        {
            int at = wasTarget[i] && ready[i] ? place(actors[i]) : -1;
            uint32_t slot = ai + 4 + uint32_t(at) * 36;
            if (at >= 0 && LoadBE32(slot + 4) == LoadBE32(slot + 8) && LoadBE32(slot + 20) == LoadBE32(slot + 24) &&
                (longest < 0 || since[i] < since[longest]))
                longest = int(i);
        }
        if (longest >= 0)
        {
            lastTurn = now;
            give(longest, "its turn");
        }
    }

    void CinematicDone()
    {
        if (Managed())
            g_gatherAt = NowMs() + 500;  // after the game's own placing of players 1 and 2
    }

    // After a cutscene the game puts players 1 and 2 where the level goes on; players 3-8 stayed where
    // they were (the parking place, when they came up during the level's opening one: out of everybody's
    // sight, and their room kept current). Every player in play goes to the host's place, 1.2 apart per
    // slot behind its line, the same place on every machine. The host's place is where this machine's
    // player 1 was put (the level's player 1 spot, the same everywhere): on a client the host is player 2,
    // which the game puts at the player 2 spot (20 units off in L1_E1), and gathering there had everybody
    // corrected back across the level (a player "born" far ahead).
    void Gather(PPCContext& ctx, uint8_t* base)
    {
        g_gatherAt = 0;
        uint32_t p1 = PlayerCrash(0);
        uint32_t physics = p1 ? LoadBE32(p1 + 0xDC) : 0, actor = GuestPointer(physics) ? LoadBE32(physics + 96) : 0;
        uint32_t xform = GuestPointer(actor) ? LoadBE32(actor + 44) : 0;
        if (!GuestPointer(xform) || !PlayerOnFoot(0))
            return;
        float spot[3] = {LoadF(xform + 48), LoadF(xform + 52), LoadF(xform + 56)};
        for (uint32_t i = 0; i < MAX_PLAYERS; ++i)
        {
            uint32_t slot = PlayerSlot(i);
            if (!PlayerCrash(i) || !PlayerOnFoot(i) || HitpointsHeld(i) || (i == 0 && slot == 0))
                continue;
            float x = spot[0] + 1.2f * slot, y = spot[1] + 0.5f, z = spot[2] - (slot ? 1.5f : 0.0f);
            bool moved = TeleportCrash(ctx, base, PlayerCrash(i), x, y, z);
            Log("[mp] after the cutscene: player %u (slot %u) at the host's place (%.1f, %.1f, %.1f)%s", i + 1, slot + 1, x, y, z,
                moved ? "" : ", not moved");
        }
    }

    void PlayersTick(PPCContext& ctx, uint8_t* base)
    {
        if (!Managed() || !InLevel())
            return;
        if (uint32_t p1 = PlayerCrash(0); p1 != g_p1)
            g_p1 = p1, g_p1Since = NowMs();
        SpawnPlayers(ctx, base);
        // Players 3-8 come with the template's maximum (90), players 1 and 2 with the game's (30 at the
        // start, more with the upgrades): they get player 1's.
        float p1Max = PlayerMaxHitpoints(0);
        for (uint32_t i = 2; i < MAX_PLAYERS; ++i)
            if (p1Max > 0 && PlayerOnFoot(i) && PlayerMaxHitpoints(i) > 0 && PlayerMaxHitpoints(i) != p1Max)
            {
                Log("[mp] player %u: maximum hitpoints %.0f -> %.0f (player 1's)", i + 1, PlayerMaxHitpoints(i), p1Max);
                SetPlayerMaxHitpoints(i, p1Max);
            }
        for (uint32_t i = 0; i < MAX_PLAYERS; ++i)
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
        if (g_gatherAt && NowMs() >= g_gatherAt)
            Gather(ctx, base);
        BalanceAiTargets(ctx, base);
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
            if (e.index >= MAX_PLAYERS)
            {
                e.onFoot = true;  // never shown
                Log("[mp] player %u (Crash %08X) left hidden: at most %u players", e.index + 1, e.crash, MAX_PLAYERS);
                continue;
            }
            // Players 5-8: controllers 4-7 of ours (made once controller 0 is up).
            uint32_t extraDevice = e.index >= GAME_PLAYERS ? ExtraDevice(ctx, base, e.index) : 0;
            if (e.index >= GAME_PLAYERS && !extraDevice)
                continue;
            uint32_t saved = LoadBE32(BACKPACK_CRASH);
            StoreBE32(BACKPACK_CRASH, p1);
            PPCContext call = ctx;
            call.r3.u64 = e.crash;
            sub_82154A58(call, base);
            StoreBE32(BACKPACK_CRASH, saved);
            StoreBE32(e.crash + 300, 2);
            // Its controller: the Crash's input binding (+176 -> +0 the CInputDevice); players 3-8 start on
            // player 2's. Players 5-8 take a reference to theirs (the Crash releases its binding when it goes).
            // The device also names the actor it drives (+508, what Crash::vf06's registration sub_821ED6C0
            // writes): its update runs that actor's input maps, so with only the binding moved the stick
            // (read through the binding) worked and no button did (no attack, no jump). Every player's device
            // gets its own actor again (the registration with the wrong controller number wrote P3's into
            // another player's device).
            uint32_t binding = LoadBE32(e.crash + 176), device = extraDevice ? extraDevice : Device(e.index);
            if (GuestPointer(binding) && LoadBE32(LoadBE32(binding)) == INPUT_DEVICE_VTABLE && device)
            {
                bool registered = LoadBE32(binding) == device;  // by the sub_821ED6C0 hook below
                StoreBE32(binding, device);
                StoreBE32(device + DEVICE_ACTOR, LoadBE32(e.crash + 156));
                OwnDevices();
                uint32_t addRef = extraDevice && !registered ? LoadBE32(LoadBE32(device)) : 0;
                if (PPCFunc* f = addRef >= PPC_CODE_BASE && addRef < PPC_CODE_BASE + PPC_CODE_SIZE ? PPC_LOOKUP_FUNC(base, addRef) : nullptr)
                {
                    PPCContext ref = ctx;
                    ref.r3.u64 = device;
                    f(ref, base);
                }
            }
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

// CInputManager: Crash::vf06 registers a player's input binding (r5, the Crash's +176) for its actor (r6) with
// the device of the player's controller, asked of CFrontendManager::GetPlayersControllerNum (sub_821D8C50, a
// table of 2) with the player index (r4): the device names the actor (+508), takes a reference and goes in
// the binding. The input maps the Crash then makes go into that device's list (+412), which its update
// (sub_821ED160) runs with its own buttons. For players 3-8 the index read past the table gave player 2's
// device: they ran on player 2's buttons and, with the binding moved to their own device afterwards, only
// the stick (read through the binding) was theirs (no jump, no attack on the other machines). Each of them
// is registered with its own device here: 2 / 3 for players 3 / 4, ours (ExtraDevice) for players 5-8.
PPC_FUNC(sub_821ED6C0)
{
    uint32_t binding = ctx.r5.u32, actor = ctx.r6.u32;
    if (Managed() && GuestPointer(binding) && actor)
        for (const Extra& e : g_extras)
        {
            // The Crash is its actor's part at +0x40 (its +156, the actor, is set after vf06)
            if (e.index < 2 || e.index >= mp::MAX_PLAYERS || (e.crash - 0x40 != actor && LoadBE32(e.crash + 156) != actor))
                continue;
            uint32_t device = e.index < mp::GAME_PLAYERS ? Device(e.index) : ExtraDevice(ctx, base, e.index);
            if (!GuestPointer(device))
                break;
            StoreBE32(device + DEVICE_ACTOR, actor);
            auto call = [&](uint32_t object, uint32_t slot) {
                uint32_t f = LoadBE32(LoadBE32(object) + slot * 4);
                if (PPCFunc* fn = f >= PPC_CODE_BASE && f < PPC_CODE_BASE + PPC_CODE_SIZE ? PPC_LOOKUP_FUNC(base, f) : nullptr)
                {
                    PPCContext c = ctx;
                    c.r3.u64 = object;
                    fn(c, base);
                }
            };
            call(device, 0);  // AddRef
            if (uint32_t old = LoadBE32(binding); GuestPointer(old))
                call(old, 1);  // Release
            StoreBE32(binding, device);
            Log("[mp] player %u's input registered with controller %u's device (not player 2's)", e.index + 1, e.index);
            return;
        }
    __imp__sub_821ED6C0(ctx, base);
}

// The objectives' trigger volumes (pObjective:WAIT_TriggerVolume(name, by)): the condition (+8 its volume, +20 by)
// with by = -1 (ETriggeredBy_MID_POINT_OF_ALL_ACTIVE_PLAYERS, most of them: the combat volumes that spawn the
// enemies, the reset points) tests the point between the players, -2 (EITHER_PLAYER) players 1 and 2 only. Online,
// where every player has a screen and goes anywhere, the enemies came only once every Crash stood near
// their volume: there both are met by any player in play (1-8) inside it. -3 (BOTH_PLAYERS) and 1 / 2 stay the game's.
PPC_FUNC(sub_82219908)
{
    uint32_t condition = ctx.r3.u32, volume = GuestPointer(condition) ? LoadBE32(condition + 8) : 0;
    int32_t by = volume ? int32_t(LoadBE32(condition + 20)) : 0;
    if (mp::Online() && (by == -1 || by == -2))
    {
        for (uint32_t i = 0; i < mp::MAX_PLAYERS; ++i)
        {
            uint32_t crash = mp::PlayerOnFoot(i) ? mp::PlayerCrash(i) : 0;
            uint32_t physics = crash ? LoadBE32(crash + 0xDC) : 0, actor = GuestPointer(physics) ? LoadBE32(physics + 96) : 0;
            if (!GuestPointer(actor))
                continue;
            PPCContext call = ctx;
            call.r3.u64 = volume;
            call.r4.u64 = actor;
            sub_8213B280(call, base);
            if (call.r3.u8)
            {
                if (mp::Trace())
                    Log("[mp] trigger volume %08X met by player %u (by %d)", volume, i + 1, by);
                ctx.r3.u64 = 1;
                return;
            }
        }
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_82219908(ctx, base);
}
