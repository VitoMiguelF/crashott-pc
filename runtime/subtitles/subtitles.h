#pragma once

// Subtitles for the cutscenes and the in-game dialogue clips (the game has none).
//
// Text: a subtitle file per language (tools/subtitles.py, README "Legendas"):
//   [nis01]
//   sound = english sound\rsd\english\c\c268899e25ed5279.rsd
//   seconds = 103.363
//   7.26 9.62 Crash! Help me get this gizmo working!
// English: <data>/subtitles/english.txt (paths.h: dist/data or private/; transcribed from the voices, local only).
// A language pack's translation: languages\<id>\subtitles.txt, same keys and times; keys or
// fields it leaves out come from the English file. CRASH_SUBTITLE_FILE=<file> overrides.
//
// Timing: each subtitled sound is recognized when the game hands its first XMA packets to a
// decoder context. At startup a background thread reads the start of every subtitled .rsd from
// the voice archive and hashes the first packets of each stream (as the title submits them: one
// context per stereo stream, each buffer starting at its stream's first packet) and of every later
// 64 KB block; the decoder (audio/xma.cpp) hashes every newly submitted input buffer the same way.
// The title decodes ahead (cutscene audio in ~2.3 s bursts, short clips whole at once), so the
// sound's clock is the title's audio clock (frames submitted to the output, audio_output.h) from
// that moment, minus a fixed delay (CRASH_SUBTITLE_OFFSET_MS, default 280), never past what was
// decoded, and frozen while a stream stops decoding for over 3.5 s (pause, skip). A context that
// moves on to other audio ends a stream it hadn't finished (skipped scene); a clip already decoded
// whole keeps playing. Three subtitled sounds started on one context within 0.5 s are a load-time
// unpack, not playback, and are dropped. CRASH_SUBTITLE_TRACE=1 logs all of it;
// CRASH_SUBTITLE_TEST=<text> shows a fixed text (style checks).
//
// Drawing: subtitle_draw.cpp rasterizes the current lines with GDI (system font, accents, outline,
// optional box) at the output resolution and draws them with the overlay's bitmap pass.

#include <cstdint>
#include <string>

namespace subtitles
{
    // Loads the subtitle file of the startup language and starts fingerprinting (after
    // settings::Load and vfs::Init). Works whether or not subtitles are switched on, so the
    // setting can change live.
    void Init();

    // Number of sounds with subtitles for the startup language (0 = none found), and where from.
    size_t SoundCount();
    const std::string& Source();

    // XMA decoder thread (audio/xma.cpp). Cheap no-ops when nothing is loaded.
    // A buffer newly submitted to `context` (before the pass decodes it); `frames` = frames the
    // context decoded so far.
    void OnInputBuffer(uint32_t context, const uint8_t* bytes, uint32_t packets, uint32_t sampleRate, uint64_t frames);
    // After a decoding pass: total frames decoded by `context`.
    void OnFrames(uint32_t context, uint64_t frames);

    // Render thread: the text to show now (UTF-16, lines separated by '\n'), empty for none, a
    // number that changes whenever the text does, and whether a line comes from an in-game clip
    // (drawn higher, clear of the game's own hint box) rather than a cutscene.
    std::wstring CurrentText(uint64_t& version, bool& gameplay);

    // A cutscene was skipped (settings/skip_cinematics.h): its lines go away at once instead of
    // after the stream stops decoding. In-game clips (keys with a '/') stay.
    void EndCutscenes();
}
