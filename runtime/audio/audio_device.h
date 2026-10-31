#pragma once

#include <cstdint>

// The host audio device behind audio_output.cpp: a float stream at 48 kHz of 2, 6 or 8 interleaved
// channels, fed one game frame (256 samples) at a time. XAudio2 on Windows (audio_device_win32.cpp),
// SDL3 elsewhere (audio_device_sdl.cpp). Called under audio_output.cpp's lock, except Queued/LastPlayed.
namespace audio::device
{
    // The stream's channel layouts: 2 = FL FR; 6 = FL FR C LFE SL SR (the game's own 5.1 order; the
    // surround pair goes to the device's side or back speakers); 8 = FL FR C LFE BL BR SL SR.
    constexpr uint32_t MAX_CHANNELS = 8;

    // Opens the default output for `channels` channels. Returns the channels opened: `channels`, 6
    // when 8 were asked of a 5.1 device, 2 when the device has fewer speakers than 5.1 (logged), or
    // 0 when there is no device (logged why).
    uint32_t Open(uint32_t sampleRate, uint32_t channels);
    // Queues `frames` frames of the channels Open returned (interleaved); the data is copied. False if dropped.
    bool Queue(const float* samples, uint32_t frames);
    // Game frames queued and not played yet.
    int Queued();
    // steady_clock ticks (time_since_epoch) when the device last finished a frame.
    int64_t LastPlayed();
    // Stops the output (the process is exiting).
    void Stop();
    // Releases the output so that Open can be called again (another layout chosen in the settings).
    void Close();
}
