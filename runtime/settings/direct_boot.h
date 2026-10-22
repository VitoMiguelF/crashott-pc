#pragma once

// Skipping the intros, booting straight into a level or a save, and the game's milestones as
// events for tests (direct_boot.cpp; README "Boot direto e eventos", runtime/NOTES.md).
//
//   CRASH_SKIP_INTROS=1 / --skip-intros   no logo videos, no title screen: straight to the main menu
//   CRASH_BOOT=<spec> / --boot <spec>     menu | level:<L1_E2> | load:<slot>[:continue|hub|<L1_E2>]
//   CRASH_SKIP_CINEMATICS=1 / --skip-cinematics   the in-game cutscenes (NIS) end as soon as they
//                                         play (skip_cinematics.cpp)
//
// Everything goes through the game's own code: the frontend fight tree takes its own exits
// (frontend_hooks.cpp asks OverrideExit which one), the level starts with the game's StartLevel.
// Events: "[test] event <name> (frame F, input clock S s, tick T)" in the log, for run_game.py
// anchors and the replay's EVENT entries.

#include <cstdint>
#include <string>

struct PPCContext;

namespace boot
{
    // main(), after HandleRestartArguments: --skip-intros, --boot <spec> (they win over the
    // environment). A restart from the settings screen skips the intros too.
    void HandleArguments(int argc, char** argv);
    bool SkipIntros();
    bool SkipCinematics();
    // Buttons the boot presses for the game (settings_menu GameButtons): START on the title screen.
    uint16_t Buttons();

    // The frontend's condition function (sub_820F9BA8 hook): every evaluation of `node` (events),
    // and the exit the game chose (-1 = none) -> the exit to take. `tree` is the Frontend object.
    void NoteFrontendNode(uint32_t node);
    int32_t OverrideExit(PPCContext& ctx, uint8_t* base, uint32_t tree, uint32_t node, int32_t exit);

    // A milestone: logs "[test] event <name> (...)" and keeps its input clock time.
    void Event(const char* name, const char* detail = nullptr);

    // A time relative to an event: "<event>[#n]+<offset>" (n-th occurrence, default 1), or a
    // plain number (relative to the input clock's zero).
    struct Anchored
    {
        std::string event;     // empty = the input clock
        int occurrence = 1;
        int64_t offsetMs = 0;
    };
    // Parses one at `text` (offset in units of `unitMs`: 1 for ms, 1000 for seconds) and moves
    // `text` past it. False if there is no time there.
    bool ParseAnchored(const char*& text, double unitMs, Anchored& out);
    // The input clock ms that `time` stands for, or -1 while its event hasn't happened.
    int64_t Resolve(const Anchored& time);
}
