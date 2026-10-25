// SDL3 output for audio_output.cpp (audio_device.h): one SDL_AudioStream on the default playback
// device, stereo float. SDL converts to the device's format and rate.
#include "audio_device.h"
#include "audio_output.h"
#include "../diagnostics.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr int FRAME_BYTES = int(audio::FRAME_SAMPLES * 2 * sizeof(float));
    // Like the XAudio2 ring: past this many queued game frames a new one is dropped.
    constexpr int MAX_QUEUED = 31;

    SDL_AudioStream* g_stream;
    std::atomic<int> g_lastQueued;
    std::atomic<int64_t> g_lastPlayed; // Clock ticks

    int64_t Now()
    {
        return Clock::now().time_since_epoch().count();
    }

    // Queued game frames; notes the time when the device took some since the last look.
    int Sample()
    {
        int queued = SDL_GetAudioStreamQueued(g_stream) / FRAME_BYTES;
        if (queued < g_lastQueued.exchange(queued))
            g_lastPlayed = Now();
        return queued;
    }
}

namespace audio::device
{
    bool Open(uint32_t sampleRate)
    {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            Log("[audio] SDL audio init failed (%s): no sound", SDL_GetError());
            return false;
        }
        SDL_AudioSpec spec{ SDL_AUDIO_F32, 2, int(sampleRate) };
        g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!g_stream)
        {
            Log("[audio] no audio device (%s): no sound", SDL_GetError());
            return false;
        }
        SDL_AudioSpec device{};
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_stream), &device, nullptr);
        SDL_ResumeAudioStreamDevice(g_stream);
        Log("[audio] SDL output (%s): device %d ch at %d Hz, stream stereo float at %u Hz", SDL_GetCurrentAudioDriver(),
            device.channels, device.freq, sampleRate);
        g_lastPlayed = Now();
        return true;
    }

    bool Queue(const float* stereo, uint32_t frames)
    {
        if (Sample() >= MAX_QUEUED)
            return false;
        if (!SDL_PutAudioStreamData(g_stream, stereo, int(frames * 2 * sizeof(float))))
            return false;
        g_lastQueued = SDL_GetAudioStreamQueued(g_stream) / FRAME_BYTES;
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
}
