// Skipping the in-game cutscenes (NIS) for tests: CRASH_SKIP_CINEMATICS=1 / --skip-cinematics
// (direct_boot.h; README "Boot direto e eventos", runtime/NOTES.md "Pular as cenas").
//
// A cutscene is a CMasterCinematicAction (the action of a fight tree MasterCinematicTrack, e.g. the
// Frontend.bfig node 756 R1_NIS01 of L1_E1) with a state machine at +412, changed only by
// SetState (sub_821B92E8(action, state)) and run by its update (vf05, sub_821BA100):
//
//   0 prepare -> 1 or 2      1 fade in (preCinematicFade)    2 start (load the camera/sounds)
//   3 playing: the camera action (+340) and the sound sequence (+336) drive the time, in
//     frames at [action+12]+124 (the sound stream's position while it plays, so the clock of the
//     game can't make it faster); when both are done (or the managers' "profile changed" bit,
//     +352 & 0x80, is set) -> 4
//   4 end -> 5 or 6          5 fade out (postCinematicFade)  6 post tracks (+236), player shown
//   7 done: action+418 |= 0x80 and [managers 12]+452 |= 0x80, which the frontend's ExitWhenDone
//     condition waits for (forcing that exit instead leaves the level on LOADING forever)
//
// Skipping calls SetState(4) itself once the cutscene is playing: the same path its end takes
// (sound stopped, fade, post tracks, done flag), so the frontend leaves the NIS node by itself.

#include "direct_boot.h"
#include "../kernel/kernel.h"
#include "../kernel/input.h"
#include "../kernel/memory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

PPC_EXTERN_FUNC(sub_821B92E8);
extern "C" PPC_FUNC(__imp__sub_821B92E8);  // CMasterCinematicAction::SetState(state)
extern "C" PPC_FUNC(__imp__sub_821BA100);  // CMasterCinematicAction::vf05 (update)

namespace
{
    constexpr uint32_t ACTION_STATE = 412, ACTION_FLAGS = 418, ACTION_TRACK = 12, TRACK_TIME = 124;
    constexpr uint32_t STATE_PLAYING = 3, STATE_END = 4, STATE_DONE = 7;

    float CinematicFrames(uint32_t action)
    {
        uint32_t track = LoadBE32(action + ACTION_TRACK);
        if (!track)
            return 0.0f;
        uint32_t bits = LoadBE32(track + TRACK_TIME);
        float value;
        memcpy(&value, &bits, 4);
        return value;
    }

    // Updates of the cutscene in state 3 before it is skipped (the first ones start its tracks).
    int SkipAfterUpdates()
    {
        static int value = [] {
            const char* text = getenv("CRASH_SKIP_CINEMATICS_AFTER");
            return text && *text ? std::max(1, atoi(text)) : 1;
        }();
        return value;
    }

    uint32_t g_playingAction = 0;
    int g_playingUpdates = 0;
}

// SetState: the transitions in the log, the events of the start and the end.
PPC_FUNC(sub_821B92E8)
{
    uint32_t action = ctx.r3.u32;
    uint32_t from = LoadBE32(action + ACTION_STATE), to = ctx.r4.u32;
    __imp__sub_821B92E8(ctx, base);
    // SetState goes on through states 0 and 4 by itself (they pick the next one): the final state.
    uint32_t now = LoadBE32(action + ACTION_STATE);
    // (the first call, from the start action, finds the state not set yet)
    Log("[cinematic] 0x%08X state %s -> %u%s (frame %.1f, tick %lld)", action, from <= STATE_DONE ? std::to_string(from).c_str() : "new", to,
        now != to ? (" -> " + std::to_string(now)).c_str() : "", CinematicFrames(action), (long long)input::GameTick());
    if (now == STATE_DONE)
        boot::Event("cinematic_done");
}

// vf05 (update): once the cutscene plays, its end when skipping.
PPC_FUNC(sub_821BA100)
{
    uint32_t action = ctx.r3.u32;
    if (boot::SkipCinematics() && LoadBE32(action + ACTION_STATE) == STATE_PLAYING)
    {
        if (action != g_playingAction)
            g_playingAction = action, g_playingUpdates = 0;
        if (++g_playingUpdates >= SkipAfterUpdates())
        {
            char detail[64];
            snprintf(detail, sizeof(detail), "frame %.1f", CinematicFrames(action));
            boot::Event("cinematic_skipped", detail);
            g_playingAction = 0;
            PPCContext call = ctx;
            call.r3.u64 = action;
            call.r4.u64 = STATE_END;
            sub_821B92E8(call, base);
        }
    }
    __imp__sub_821BA100(ctx, base);
}
