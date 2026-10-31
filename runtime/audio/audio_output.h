#pragma once

#include <cstdint>

// Host side of the XAudio render driver (kernel/audio_imports.cpp): the title hands over frames of
// 256 samples x 6 channels (planar, big-endian floats: FL, FR, C, LFE, SL, SR) at 48 kHz. Its
// whole mix comes this way, the Bink videos' 5.1 soundtracks too. The frames are played through
// the host device (audio_device.h: XAudio2 / SDL3) as audio.output says (settings.h AudioOutput):
// downmixed to stereo, or as 5.1 / 7.1 multichannel PCM when the device has those speakers
// (else stereo, logged); the setting applies live (the device is reopened). Optionally the mix is
// also written to a WAV file.
//
// Environment: CRASH_NOAUDIO=1 skips the host device (frames are still mixed and dumped, and the
// title is clocked in real time); CRASH_AUDIO_DUMP=<file.wav> writes the mix (16-bit, 48 kHz, at
// most CRASH_AUDIO_DUMP_SECONDS seconds, default 60) in the layout audio.output had at startup:
// 2 channels, or 6 / 8 in the device's order (audio_device.h), whatever the device took.
namespace audio
{
    constexpr uint32_t FRAME_SAMPLES = 256;
    constexpr uint32_t FRAME_CHANNELS = 6;
    constexpr uint32_t SAMPLE_RATE = 48000;

    // Opens the output. Call on the thread that will call WaitForNextFrame.
    void StartOutput();

    // Blocks until the title should be asked for the next frame: every 256 samples of real time,
    // nudged to keep the device's queue at a steady depth. Reopens the device when audio.output
    // changed.
    void WaitForNextFrame();

    // XAudioSubmitRenderDriverFrame (any thread). `samples` is the guest frame.
    void SubmitFrame(const uint8_t* samples);

    // Frames submitted so far: the title's audio clock (also the frame index in the dump).
    uint64_t SubmittedFrames();

    // Channels the device plays now (2, 6 or 8); 0 without a device, or until it was reopened for a
    // new audio.output.
    uint32_t OutputChannels();

    // The SETTINGS screen's AUDIO TEST: the game goes quiet and each speaker of the layout being
    // played gets a second in turn (noise; a low tone on the subwoofer): front left, center, front
    // right, surround right, (back right, back left,) surround left, subwoofer. Pressed again: from
    // the start. It is also in the dump when the dump has the device's layout.
    void StartChannelTest();
    // The speaker being tested (TestSpeaker), or -1.
    enum TestSpeaker { SPEAKER_FL, SPEAKER_FR, SPEAKER_C, SPEAKER_LFE, SPEAKER_SL, SPEAKER_SR, SPEAKER_BL, SPEAKER_BR };
    int TestedSpeaker();

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
