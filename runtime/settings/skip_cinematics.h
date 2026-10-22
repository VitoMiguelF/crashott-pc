#pragma once

// Skipping the cutscenes (skip_cinematics.cpp; docs/GUIA_DO_PROJETO.md "Pular cenas e videos",
// runtime/NOTES.md "Pular as cenas"):
//
// - The player: with game.skip_cutscenes on (SETTINGS > GAME > SKIP CUTSCENES, the default), a
//   small prompt fades in at the bottom right when a cutscene (NIS) or one of the intro videos
//   (Bink: the logos and the shadow-theatre intro; the attract video keeps the game's own
//   any-button exit) starts, hides after a few seconds and comes back on any input. Holding B / START / A on a pad (Circle / Options / Cross),
//   Esc / Space / Enter on the keyboard, or the left mouse button on the prompt for HOLD_MS skips
//   it; a ring around the button fills meanwhile. Those buttons are hidden from the game while a
//   cutscene or a video plays (nothing in the game uses them then; the videos' own one-press skip
//   becomes the hold) and until they are released after a skip.
// - Tests: CRASH_SKIP_CINEMATICS=1 / --skip-cinematics (direct_boot.h) ends every cutscene as
//   soon as it plays.
//
// A cutscene ends through the game's own path for its natural end (CMasterCinematicAction state 4:
// the sound stops, the fade, the player put back, the scene marked as done, and the frontend goes
// on by itself); a video gets a START press, the game's own skip. The subtitles of the scene are
// cleared at once (subtitles::EndCutscenes).

#include <windows.h>
#include <d3d12.h>
#include <xinput.h>

#include <cstdint>

namespace cutscenes
{
    // Controller 0 as the game is about to see it (kernel/xam_input_imports.cpp, ReadController;
    // `poll` = the game's own read): the hold to skip, and the skip buttons hidden from the game.
    void FilterGameInput(XINPUT_GAMEPAD& pad, bool poll);

    // direct_boot.cpp: a video starts (CMoviePlayerBehaviour::PlayMovie, the index it really plays:
    // 0 dolby, 1 radical, 2 sierra, 3 shadow, 4 attract), and the frontend node shown (a video
    // started on another screen is over once the screen changes).
    void NoteMovie(int index);
    void NoteFrontendNode(uint32_t node);

    // Render thread, once per presented frame, over the back buffer (RENDER_TARGET state).
    void Draw(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE target, uint32_t width, uint32_t height);
}
