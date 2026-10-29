#pragma once

#include <cstdint>

// Controller 0 clock, recording and replay (xam_input_imports.cpp, input_record.cpp), shared with
// the overlay, the renderer and the file I/O.
namespace input
{
    // Milliseconds since the game first read controller 0: the zero of CRASH_INPUT_SCRIPT and of
    // CRASH_SCREENSHOT_TIMES. -1 before that first read.
    int64_t ClockMs();

    // Recording (F5, or from boot with CRASH_RECORD_INPUT=1): everything the game reads from
    // controller 0 (buttons, both sticks, both triggers) on the game tick clock, written as a
    // ready-to-run profile in CRASH_RECORD_DIR (config/recorded). See input_record.cpp.
    bool Recording();
    // F5: starts a recording, or stops the current one and writes its profile. Returns the
    // message for the overlay ("GRAVANDO", "SALVO: <name>").
    const char* ToggleRecording();
    // Stops a recording in progress (the window is closing).
    void StopRecording();
    // A moment marked with F2: a screenshot in the recording.
    void RecordMark(uint64_t frame);

    // For the overlay: recording and replay (CRASH_REPLAY) progress.
    struct RecordStatus
    {
        bool recording;
        double recordSeconds;   // game seconds since F5
        bool replaying;
        double replaySeconds;   // game seconds on the replay's timeline
        double shiftSeconds;    // how far SYNC/WAIT moved the timeline (+ = later than recorded)
        const char* waiting;    // "CARREGAMENTO" / "LOG" while a SYNC/WAIT holds the timeline, else null
    };
    RecordStatus Status();
    // True once for each SHOT entry the replay (or CRASH_INPUT_SCRIPT) reaches (the renderer saves
    // a screenshot).
    bool ConsumeShotRequest();
    void RequestShot();

    // Controller 0 for the runtime's settings screen (XINPUT_GAMEPAD_* bits): the pad, the left
    // stick as a D-pad, the keyboard (Esc = B), CRASH_INPUT_SCRIPT and the replay, whether or not
    // the game currently sees them.
    uint16_t MenuButtons();

    // The last button that went down on controller 0 (for the overlay): its name (run.toml's,
    // "A", "DOWN"...), the input clock and the frame then, and whether CRASH_INPUT_SCRIPT or the
    // replay pressed it or the player (pad or keyboard). False before the first press.
    struct Press
    {
        const char* button;
        int64_t clockMs;
        uint64_t frame;
        bool scripted;
    };
    bool LastPress(Press& press);
    // The presented frame number (the overlay's), recorded with each press.
    void SetFrame(uint64_t frame);
    uint64_t Frame();
    // The game tick (reads of controller 0 so far: the replay's clock, 30 per second in gameplay).
    int64_t GameTick();
    // A test event (settings/direct_boot.h) happened: a recording writes it as an EVENT entry,
    // which its replay waits for.
    void NoteEvent(const char* name);

    // Controllers 4-7, beyond the four XInput users (online/mp_players.cpp: players 5-8 of a session, 5-8
    // is experimental): CRASH_INPUT_SCRIPT's P5_..P8_ entries and the remote player that controller plays
    // (online/mp.h RemotePad). `gamepad` is a 12-byte XINPUT_GAMEPAD; false if nothing is there.
    bool ReadExtraController(uint32_t user, void* gamepad);

    // A read the game made from its own files (kernel/file_imports.cpp): `level` = default.rcf,
    // whose read bursts mark the loads the replay synchronizes on.
    void NoteGameRead(uint64_t offset, uint64_t bytes, bool level);
}
