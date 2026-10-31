// SDL3 output for audio_output.cpp (audio_device.h): one SDL_AudioStream on the default playback
// device, float. SDL converts to the device's format and rate; its channel orders for 6 and 8
// channels (FL FR FC LFE BL BR [SL SR]) match the stream's by position. A surround stream is only
// opened when the device reports that many channels (Linux: PipeWire / PulseAudio / ALSA HDMI;
// Android: AAudio), else stereo, as before.
#include "audio_device.h"
#include "audio_output.h"
#include "../diagnostics.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>

namespace
{
    using Clock = std::chrono::steady_clock;

    // Like the XAudio2 ring: past this many queued game frames a new one is dropped.
    constexpr int MAX_QUEUED = 31;

    SDL_AudioStream* g_stream;
    int g_frameBytes = int(audio::FRAME_SAMPLES * 2 * sizeof(float));
    std::atomic<int> g_lastQueued;
    std::atomic<int64_t> g_lastPlayed; // Clock ticks

    int64_t Now()
    {
        return Clock::now().time_since_epoch().count();
    }

    // Queued game frames; notes the time when the device took some since the last look.
    int Sample()
    {
        int queued = SDL_GetAudioStreamQueued(g_stream) / g_frameBytes;
        if (queued < g_lastQueued.exchange(queued))
            g_lastPlayed = Now();
        return queued;
    }
}

namespace audio::device
{
    uint32_t Open(uint32_t sampleRate, uint32_t channels)
    {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            Log("[audio] SDL audio init failed (%s): no sound", SDL_GetError());
            return 0;
        }
        if (channels > 2)
        {
            SDL_AudioSpec device{};
            int speakers = SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &device, nullptr) ? device.channels : 0;
            if (channels == 8 && speakers >= 6 && speakers < 8)
            {
                Log("[audio] the device has no 7.1 speakers (%d ch): 5.1 output", speakers);
                channels = 6;
            }
            if (speakers < int(channels))
            {
                Log("[audio] the device has no %s speakers (%d ch): stereo output", channels == 8 ? "7.1" : "5.1", speakers);
                channels = 2;
            }
        }
        else
            channels = 2;

        SDL_AudioSpec spec{ SDL_AUDIO_F32, int(channels), int(sampleRate) };
        g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!g_stream)
        {
            Log("[audio] no audio device (%s): no sound", SDL_GetError());
            return 0;
        }
        g_frameBytes = int(audio::FRAME_SAMPLES * channels * sizeof(float));
        g_lastQueued = 0;
        SDL_AudioSpec device{};
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_stream), &device, nullptr);
        SDL_ResumeAudioStreamDevice(g_stream);
        Log("[audio] SDL output (%s): device %d ch at %d Hz, stream %s float at %u Hz", SDL_GetCurrentAudioDriver(), device.channels,
            device.freq, channels == 8 ? "7.1" : channels == 6 ? "5.1" : "stereo", sampleRate);
        g_lastPlayed = Now();
        return channels;
    }

    bool Queue(const float* samples, uint32_t frames)
    {
        if (Sample() >= MAX_QUEUED)
            return false;
        if (!SDL_PutAudioStreamData(g_stream, samples, int(frames * (g_frameBytes / audio::FRAME_SAMPLES))))
            return false;
        g_lastQueued = SDL_GetAudioStreamQueued(g_stream) / g_frameBytes;
        return true;
    }

    int Queued()
    {
        return g_stream ? Sample() : 0;
    }

    int64_t LastPlayed()
    {
        return g_lastPlayed;
    }

    void Stop()
    {
        if (g_stream)
        {
            SDL_PauseAudioStreamDevice(g_stream);
            SDL_ClearAudioStream(g_stream);
        }
    }

    void Close()
    {
        if (g_stream)
            SDL_DestroyAudioStream(g_stream);
        g_stream = nullptr;
    }
}
