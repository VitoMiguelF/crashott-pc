// XAudio2 output for audio_output.cpp (audio_device.h).
//
// The mastering voice takes the device's own layout (the speaker setup in the Windows sound
// settings: an HDMI receiver or eARC TV set to 5.1 / 7.1 gets multichannel LPCM). A surround stream
// is mapped onto it by speaker position (SetOutputMatrix), so the channels land on the right
// speakers whatever the device's channel order; a device without the speakers asked for gets the
// stereo stream, which XAudio2 spreads by itself as before.
#include "audio_device.h"
#include "audio_output.h"
#include "../diagnostics.h"

#include <windows.h>
#include <mmreg.h>
#include <xaudio2.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>

namespace
{
    using Clock = std::chrono::steady_clock;

    // The ring holds the buffers XAudio2 is still reading.
    constexpr uint32_t RING_FRAMES = 32;

    constexpr DWORD FRONT = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY;
    constexpr DWORD SIDE = SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    constexpr DWORD BACK = SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT (ksmedia.h; MinGW has no library that defines it).
    constexpr GUID SUBTYPE_IEEE_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

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
    uint32_t g_channels;
    std::atomic<int> g_queued;
    std::atomic<int64_t> g_lastBufferEnd; // Clock ticks

    float g_ring[RING_FRAMES][audio::FRAME_SAMPLES * audio::device::MAX_CHANNELS];
    uint32_t g_ringNext;

    void VoiceCallback::OnBufferEnd(void*)
    {
        g_queued--;
        g_lastBufferEnd = Clock::now().time_since_epoch().count();
    }

    // The speakers of the stream's channels, in its order (audio_device.h), on a device with `mask`;
    // 0 when the device lacks one of them.
    DWORD StreamMask(uint32_t channels, DWORD mask)
    {
        if (channels == 8)
            return (mask & (FRONT | SIDE | BACK)) == (FRONT | SIDE | BACK) ? FRONT | SIDE | BACK : 0;
        if ((mask & FRONT) != FRONT)
            return 0;
        // 5.1: the surround pair on the side speakers (7.1 devices and Windows' usual 5.1), else the back ones.
        if ((mask & SIDE) == SIDE)
            return FRONT | SIDE;
        return (mask & BACK) == BACK ? FRONT | BACK : 0;
    }

    void Release()
    {
        if (g_voice)
            g_voice->DestroyVoice();
        if (g_master)
            g_master->DestroyVoice();
        if (g_xaudio)
            g_xaudio->Release();
        g_voice = nullptr;
        g_master = nullptr;
        g_xaudio = nullptr;
        g_queued = 0;
    }
}

namespace audio::device
{
    uint32_t Open(uint32_t sampleRate, uint32_t channels)
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
            Log("[audio] CoInitializeEx failed (0x%08lX)", hr);

        if (FAILED(hr = XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)))
        {
            Log("[audio] XAudio2Create failed (0x%08lX): no sound", hr);
            g_xaudio = nullptr;
            return 0;
        }
        if (FAILED(hr = g_xaudio->CreateMasteringVoice(&g_master)))
        {
            Log("[audio] no audio device (CreateMasteringVoice 0x%08lX): no sound", hr);
            g_master = nullptr;
            Release();
            return 0;
        }
        XAUDIO2_VOICE_DETAILS details{};
        g_master->GetVoiceDetails(&details);
        DWORD deviceMask = 0;
        g_master->GetChannelMask(&deviceMask);

        DWORD streamMask = 0;
        if (channels > 2)
        {
            streamMask = StreamMask(channels, deviceMask);
            if (!streamMask && channels == 8 && (streamMask = StreamMask(6, deviceMask)) != 0)
            {
                Log("[audio] the device has no 7.1 speakers (%u ch, mask 0x%lX): 5.1 output", details.InputChannels, deviceMask);
                channels = 6;
            }
            if (!streamMask)
            {
                Log("[audio] the device has no %s speakers (%u ch, mask 0x%lX; Windows sound settings > the device > speaker "
                    "setup): stereo output",
                    channels == 8 ? "7.1" : "5.1", details.InputChannels, deviceMask);
                channels = 2;
            }
        }
        else
            channels = 2;

        WAVEFORMATEXTENSIBLE format{};
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = WORD(channels);
        format.Format.nSamplesPerSec = sampleRate;
        format.Format.wBitsPerSample = 32;
        format.Format.nBlockAlign = WORD(channels * 4);
        format.Format.nAvgBytesPerSec = sampleRate * format.Format.nBlockAlign;
        format.Format.cbSize = sizeof(format) - sizeof(WAVEFORMATEX);
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = channels == 2 ? SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT : streamMask;
        format.SubFormat = SUBTYPE_IEEE_FLOAT;
        if (FAILED(hr = g_xaudio->CreateSourceVoice(&g_voice, &format.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &g_callback)))
        {
            Log("[audio] CreateSourceVoice failed (0x%08lX): no sound", hr);
            g_voice = nullptr;
            Release();
            return 0;
        }

        if (channels > 2)
        {
            // Stream channel i (the i-th speaker bit of streamMask, the channel order of
            // WAVEFORMATEXTENSIBLE) to the device channel of the same speaker.
            float matrix[MAX_CHANNELS * XAUDIO2_MAX_AUDIO_CHANNELS] = {};
            uint32_t source = 0;
            for (DWORD bits = streamMask; bits; bits &= bits - 1, source++)
            {
                DWORD speaker = bits & (~bits + 1);
                uint32_t target = uint32_t(std::popcount(deviceMask & (speaker - 1)));
                if (target < details.InputChannels)
                    matrix[target * channels + source] = 1.0f;
            }
            if (FAILED(hr = g_voice->SetOutputMatrix(g_master, channels, details.InputChannels, matrix)))
                Log("[audio] SetOutputMatrix failed (0x%08lX): XAudio2's own channel mapping", hr);
        }
        g_voice->Start();
        g_channels = channels;

        Log("[audio] XAudio2 output: device %u ch (mask 0x%lX) at %u Hz, stream %s float at %u Hz", details.InputChannels, deviceMask,
            details.InputSampleRate, channels == 8 ? "7.1" : channels == 6 ? "5.1" : "stereo", sampleRate);
        g_lastBufferEnd = Clock::now().time_since_epoch().count();
        return channels;
    }

    bool Queue(const float* samples, uint32_t frames)
    {
        if (g_queued >= int(RING_FRAMES) - 1)
            return false;
        float* buffer = g_ring[g_ringNext];
        g_ringNext = (g_ringNext + 1) % RING_FRAMES;
        memcpy(buffer, samples, size_t(frames) * g_channels * sizeof(float));

        XAUDIO2_BUFFER xbuffer{};
        xbuffer.AudioBytes = UINT32(frames * g_channels * sizeof(float));
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

    void Close()
    {
        Release();
    }
}
