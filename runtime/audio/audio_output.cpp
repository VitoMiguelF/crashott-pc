#include "audio_output.h"
#include "wav_writer.h"
#include "xma.h"
#include "../diagnostics.h"
#include "../settings/settings.h"

#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr auto FRAME_PERIOD = std::chrono::nanoseconds(1'000'000'000ll * audio::FRAME_SAMPLES / audio::SAMPLE_RATE);

    // Device queue: frames handed to XAudio2 and not played yet. The clock aims for TARGET_QUEUED
    // (~43 ms of latency); the ring holds the buffers XAudio2 is still reading.
    constexpr uint32_t RING_FRAMES = 32;
    constexpr int TARGET_QUEUED = 8;
    constexpr auto DEVICE_STALL = std::chrono::seconds(1);

    struct VoiceCallback : IXAudio2VoiceCallback
    {
        void STDMETHODCALLTYPE OnBufferEnd(void*) override;
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnStreamEnd() override {}
        void STDMETHODCALLTYPE OnBufferStart(void*) override {}
        void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
        void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) override
        {
            Log("[audio] XAudio2 voice error 0x%08lX", error);
        }
    };

    IXAudio2* g_xaudio;
    IXAudio2MasteringVoice* g_master;
    IXAudio2SourceVoice* g_voice;
    VoiceCallback g_callback;
    std::atomic<bool> g_deviceActive;
    std::atomic<int> g_queued;
    std::atomic<int64_t> g_lastBufferEnd; // Clock ticks

    std::mutex g_submitMutex;
    float g_ring[RING_FRAMES][audio::FRAME_SAMPLES * 2];
    uint32_t g_ringNext;
    std::unique_ptr<WavWriter> g_dump;

    Clock::time_point g_nextFrame;
    std::atomic<int64_t> g_lastSubmit;
    std::atomic<uint64_t> g_callbacks;
    std::atomic<uint64_t> g_framesSubmitted, g_framesDropped, g_peakFrames;
    float g_peak;

    void VoiceCallback::OnBufferEnd(void*)
    {
        g_queued--;
        g_lastBufferEnd = Clock::now().time_since_epoch().count();
    }

    template <typename Duration>
    int64_t Ticks(Duration duration)
    {
        return std::chrono::duration_cast<Clock::duration>(duration).count();
    }

    float LoadBEFloat(const uint8_t* p)
    {
        uint32_t bits;
        memcpy(&bits, p, 4);
        bits = __builtin_bswap32(bits);
        float value;
        memcpy(&value, &bits, 4);
        return value;
    }

    bool OpenDevice()
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
            Log("[audio] CoInitializeEx failed (0x%08lX)", hr);

        if (FAILED(hr = XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)))
        {
            Log("[audio] XAudio2Create failed (0x%08lX): no sound", hr);
            return false;
        }
        if (FAILED(hr = g_xaudio->CreateMasteringVoice(&g_master)))
        {
            Log("[audio] no audio device (CreateMasteringVoice 0x%08lX): no sound", hr);
            g_xaudio->Release();
            g_xaudio = nullptr;
            return false;
        }

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2;
        format.nSamplesPerSec = audio::SAMPLE_RATE;
        format.wBitsPerSample = 32;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        if (FAILED(hr = g_xaudio->CreateSourceVoice(&g_voice, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &g_callback)))
        {
            Log("[audio] CreateSourceVoice failed (0x%08lX): no sound", hr);
            g_master->DestroyVoice();
            g_xaudio->Release();
            g_master = nullptr;
            g_xaudio = nullptr;
            return false;
        }
        g_voice->Start();

        XAUDIO2_VOICE_DETAILS details{};
        g_master->GetVoiceDetails(&details);
        Log("[audio] XAudio2 output: device %u ch at %u Hz, stream stereo float at %u Hz",
            details.InputChannels, details.InputSampleRate, audio::SAMPLE_RATE);
        g_lastBufferEnd = Clock::now().time_since_epoch().count();
        return true;
    }
}

namespace audio
{
    void StartOutput()
    {
        if (const char* path = getenv("CRASH_AUDIO_DUMP"); path && *path)
        {
            const char* seconds = getenv("CRASH_AUDIO_DUMP_SECONDS");
            uint32_t maxSeconds = seconds ? uint32_t(atoi(seconds)) : 60;
            g_dump = std::make_unique<WavWriter>(path, SAMPLE_RATE, 2, maxSeconds ? maxSeconds : 60);
            if (g_dump->IsOpen())
                Log("[audio] writing the mix to %s (up to %u s)", path, maxSeconds);
            else
                Log("[audio] could not create %s", path);
        }

        const char* noAudio = getenv("CRASH_NOAUDIO");
        if (noAudio && *noAudio && strcmp(noAudio, "0") != 0)
            Log("[audio] CRASH_NOAUDIO: no host audio device, real-time clock");
        else
            g_deviceActive = OpenDevice();

        g_nextFrame = Clock::now();
    }

    void WaitForNextFrame()
    {
        auto now = Clock::now();
        g_nextFrame += FRAME_PERIOD;

        if (g_deviceActive)
        {
            int queued = g_queued;
            int64_t ticks = now.time_since_epoch().count();
            bool submitting = ticks - g_lastSubmit < Ticks(std::chrono::milliseconds(100));
            if (queued > 0 && ticks - g_lastBufferEnd > Ticks(DEVICE_STALL))
            {
                Log("[audio] the device stopped consuming frames; continuing without sound");
                g_deviceActive = false;
            }
            else if (queued > TARGET_QUEUED + 4)
                g_nextFrame += FRAME_PERIOD / 2; // device slower than the clock
            else if (submitting && queued < TARGET_QUEUED - 2)
                g_nextFrame -= FRAME_PERIOD / 2; // device faster than the clock (or starting up)
        }

        std::this_thread::sleep_until(g_nextFrame);
        now = Clock::now();
        if (now - g_nextFrame > FRAME_PERIOD * 16)
            g_nextFrame = now; // fell behind (debugger, slow callback): don't try to catch up
    }

    void SubmitFrame(const uint8_t* samples)
    {
        // Planar input: channel c, sample i at [c * 256 + i]. ITU-style downmix, LFE dropped, then
        // the master volume of the settings screen (the dump records what is heard).
        constexpr float k = 0.70710678f;
        const float master = settings::MasterVolume();
        float stereo[FRAME_SAMPLES * 2];
        float peak = 0;
        for (uint32_t i = 0; i < FRAME_SAMPLES; i++)
        {
            auto in = [&](uint32_t channel) { return LoadBEFloat(samples + (channel * FRAME_SAMPLES + i) * 4); };
            float center = in(2) * k;
            float left = std::clamp((in(0) + center + in(4) * k) * master, -1.0f, 1.0f);
            float right = std::clamp((in(1) + center + in(5) * k) * master, -1.0f, 1.0f);
            stereo[i * 2] = left;
            stereo[i * 2 + 1] = right;
            peak = std::max({ peak, std::abs(left), std::abs(right) });
        }

        std::lock_guard lock(g_submitMutex);
        g_peak = std::max(g_peak, peak);
        if (peak > 1.0f / 32768)
            g_peakFrames++;
        g_framesSubmitted++;
        g_lastSubmit = Clock::now().time_since_epoch().count();

        if (g_dump && g_dump->IsOpen() && !g_dump->IsFull())
        {
            int16_t pcm[FRAME_SAMPLES * 2];
            for (uint32_t i = 0; i < FRAME_SAMPLES * 2; i++)
                pcm[i] = int16_t(stereo[i] * 32767.0f);
            g_dump->Write(pcm, FRAME_SAMPLES);
        }

        if (!g_deviceActive)
            return;
        if (g_queued >= int(RING_FRAMES) - 1)
        {
            g_framesDropped++;
            return;
        }
        float* buffer = g_ring[g_ringNext];
        g_ringNext = (g_ringNext + 1) % RING_FRAMES;
        memcpy(buffer, stereo, sizeof(stereo));

        XAUDIO2_BUFFER xbuffer{};
        xbuffer.AudioBytes = sizeof(stereo);
        xbuffer.pAudioData = reinterpret_cast<const BYTE*>(buffer);
        g_queued++;
        if (FAILED(g_voice->SubmitSourceBuffer(&xbuffer)))
        {
            g_queued--;
            g_framesDropped++;
        }
    }

    uint64_t SubmittedFrames()
    {
        return g_framesSubmitted.load(std::memory_order_relaxed);
    }

    void Shutdown()
    {
        std::lock_guard lock(g_submitMutex);
        g_dump.reset();  // final header
        if (!g_deviceActive.exchange(false))
            return;
        g_voice->Stop(0);
        g_voice->FlushSourceBuffers();
        g_xaudio->StopEngine();
        Log("[audio] output stopped");
    }

    void Init()
    {
        xma::Init();
        AddDiagnosticReporter(LogStats);
    }

    void CountCallback()
    {
        g_callbacks++;
    }

    void LogStats()
    {
        Log("[audio] %llu render driver callback(s)", (unsigned long long)g_callbacks.load());
        Log("[audio] %llu frame(s) submitted (%llu with sound, peak %.3f), %llu dropped, device %s, %d queued",
            (unsigned long long)g_framesSubmitted.load(), (unsigned long long)g_peakFrames.load(), g_peak,
            (unsigned long long)g_framesDropped.load(), g_deviceActive ? "active" : "off", g_queued.load());
    }
}
