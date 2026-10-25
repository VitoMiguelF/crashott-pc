// XAudio2 output for audio_output.cpp (audio_device.h).
#include "audio_device.h"
#include "audio_output.h"
#include "../diagnostics.h"

#include <windows.h>
#include <xaudio2.h>

#include <atomic>
#include <chrono>
#include <cstring>

namespace
{
    using Clock = std::chrono::steady_clock;

    // The ring holds the buffers XAudio2 is still reading.
    constexpr uint32_t RING_FRAMES = 32;

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
    std::atomic<int> g_queued;
    std::atomic<int64_t> g_lastBufferEnd; // Clock ticks

    float g_ring[RING_FRAMES][audio::FRAME_SAMPLES * 2];
    uint32_t g_ringNext;

    void VoiceCallback::OnBufferEnd(void*)
    {
        g_queued--;
        g_lastBufferEnd = Clock::now().time_since_epoch().count();
    }
}

namespace audio::device
{
    bool Open(uint32_t sampleRate)
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
        format.nSamplesPerSec = sampleRate;
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
            details.InputChannels, details.InputSampleRate, sampleRate);
        g_lastBufferEnd = Clock::now().time_since_epoch().count();
        return true;
    }

    bool Queue(const float* stereo, uint32_t frames)
    {
        if (g_queued >= int(RING_FRAMES) - 1)
            return false;
        float* buffer = g_ring[g_ringNext];
        g_ringNext = (g_ringNext + 1) % RING_FRAMES;
        memcpy(buffer, stereo, size_t(frames) * 2 * sizeof(float));

        XAUDIO2_BUFFER xbuffer{};
        xbuffer.AudioBytes = UINT32(frames * 2 * sizeof(float));
        xbuffer.pAudioData = reinterpret_cast<const BYTE*>(buffer);
        g_queued++;
        if (FAILED(g_voice->SubmitSourceBuffer(&xbuffer)))
        {
            g_queued--;
            return false;
        }
        return true;
    }

    int Queued()
    {
        return g_queued;
    }

    int64_t LastPlayed()
    {
        return g_lastBufferEnd;
    }

    void Stop()
    {
        g_voice->Stop(0);
        g_voice->FlushSourceBuffers();
        g_xaudio->StopEngine();
    }
}
