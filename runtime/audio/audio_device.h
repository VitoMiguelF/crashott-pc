#pragma once

#include <cstdint>

// The host audio device behind audio_output.cpp: a stereo float stream at 48 kHz, fed one game
// frame (256 samples) at a time. XAudio2 on Windows (audio_device_win32.cpp), SDL3 elsewhere
// (audio_device_sdl.cpp). Called under audio_output.cpp's lock, except Queued/LastPlayed.
namespace audio::device
{
    // Opens the default output; false (and logged why) when there is none.
    bool Open(uint32_t sampleRate);
    // Queues `frames` stereo frames (interleaved L/R); the data is copied. False if dropped.
    bool Queue(const float* stereo, uint32_t frames);
    // Game frames queued and not played yet.
    int Queued();
    // steady_clock ticks (time_since_epoch) when the device last finished a frame.
    int64_t LastPlayed();
    // Stops the output (the process is exiting).
    void Stop();
}
