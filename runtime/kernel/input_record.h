#pragma once

// Recording and replay of controller 0 on the game's own clock (input_record.cpp), used by
// xam_input_imports.cpp. The public side (overlay, renderer, file I/O) is in input.h.
//
// The clock is the game tick: one XamInputGetState(0) call. The game reads the controller once per
// simulation step (30 Hz in gameplay, a bit faster in the intro videos) and catches up after slow
// frames, so a state handed back on tick N lands on the same simulation step in every run,
// whatever the frame rate. Loading screens keep ticking, though, so a load that takes longer
// (several instances at once, a cold cache) shifts everything after it: the recording marks the
// end of each load (a burst of default.rcf reads followed by quiet) with a SYNC entry anchored
// on the last chunk read, and the replay re-aligns its timeline there.

#include <windows.h>
#include <xinput.h>

namespace input::record
{
    // Once per XamInputGetState(0), with the state the game is about to get (pad, keyboard and
    // CRASH_INPUT_SCRIPT): advances the tick, adds the replay (CRASH_REPLAY) and records the result.
    void Poll(XINPUT_STATE& state);
    // Adds the replay's current state without advancing (the runtime's settings screen, capability
    // queries).
    void Apply(XINPUT_STATE& state);
}
