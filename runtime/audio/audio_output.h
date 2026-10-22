#pragma once

#include <cstdint>

// Host side of the XAudio render driver (kernel/audio_imports.cpp): the title hands over frames of
// 256 samples x 6 channels (planar, big-endian floats: FL, FR, C, LFE, SL, SR) at 48 kHz. They are
// downmixed to stereo and played through XAudio2, and optionally written to a WAV file.
//
// Environment: CRASH_NOAUDIO=1 skips the host device (frames are still mixed and dumped, and the
// title is clocked in real time); CRASH_AUDIO_DUMP=<file.wav> writes the stereo mix (16-bit, 48 kHz,
// at most CRASH_AUDIO_DUMP_SECONDS seconds, default 60).
namespace audio
{
    constexpr uint32_t FRAME_SAMPLES = 256;
    constexpr uint32_t FRAME_CHANNELS = 6;
    constexpr uint32_t SAMPLE_RATE = 48000;

    // Opens the output. Call on the thread that will call WaitForNextFrame.
    void StartOutput();

    // Blocks until the title should be asked for the next frame: every 256 samples of real time,
    // nudged to keep the device's queue at a steady depth.
    void WaitForNextFrame();

    // XAudioSubmitRenderDriverFrame (any thread). `samples` is the guest frame.
    void SubmitFrame(const uint8_t* samples);

    // Frames submitted so far: the title's audio clock (also the frame index in the dump).
    uint64_t SubmittedFrames();

    // Stops the device output and closes the dump before the process exits (EXIT GAME, see
    // settings/quit_game.h); later frames are only counted.
    void Shutdown();

    // Counts a render driver callback (for the stats).
    void CountCallback();

    // Also registered as a diagnostic reporter by Init.
    void LogStats();

    // Registers the audio diagnostics (XMA decoder + output) and starts the XMA hardware. Call
    // before the guest runs.
    void Init();
}
