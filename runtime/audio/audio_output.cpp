#include "audio_output.h"
#include "audio_device.h"
#include "wav_writer.h"
#include "xma.h"
#include "../diagnostics.h"
#include "../gpu/window.h"
#include "../settings/settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace
{
    using Clock = std::chrono::steady_clock;
    using audio::device::MAX_CHANNELS;
    constexpr auto FRAME_PERIOD = std::chrono::nanoseconds(1'000'000'000ll * audio::FRAME_SAMPLES / audio::SAMPLE_RATE);

    // Device queue: frames handed to the device (audio_device.h) and not played yet. The clock
    // aims for TARGET_QUEUED (~43 ms of latency).
    constexpr int TARGET_QUEUED = 8;
    constexpr auto DEVICE_STALL = std::chrono::seconds(1);

    std::atomic<bool> g_deviceActive;
    bool g_noDevice;      // CRASH_NOAUDIO
    bool g_deviceOpen;    // Open succeeded (Close before opening again)
    bool g_shutDown;
    std::atomic<settings::AudioOutput> g_deviceLayout;  // the audio.output the device was opened for
    std::atomic<uint32_t> g_deviceChannels;  // what it took (2, 6, 8), 0 = none
    uint32_t g_dumpChannels = 2;

    std::mutex g_submitMutex;
    std::unique_ptr<WavWriter> g_dump;

    Clock::time_point g_nextFrame;
    std::atomic<int64_t> g_lastSubmit;
    int64_t g_resumed;  // ticks: the app came back from the background (the device was paused)
    std::atomic<uint64_t> g_callbacks;
    std::atomic<uint64_t> g_framesSubmitted, g_framesDropped, g_peakFrames;
    float g_peak;

    // AUDIO TEST (StartChannelTest): one second per speaker, the sound in the first TEST_ON of it.
    constexpr uint32_t TEST_STEP = audio::SAMPLE_RATE, TEST_ON = audio::SAMPLE_RATE * 3 / 4;
    std::atomic<bool> g_testRequested;
    std::atomic<int> g_testSpeaker{ -1 };
    bool g_testing;
    uint64_t g_testSample;   // samples since the test started
    uint32_t g_testChannels; // the layout it plays in (the device's when it started)
    uint32_t g_noise = 0x12345678;
    float g_pink[3];         // pink noise filter state

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

    uint32_t ChannelsOf(settings::AudioOutput layout)
    {
        switch (layout)
        {
        case settings::AudioOutput::Surround51: return 6;
        case settings::AudioOutput::Surround71: return 8;
        default: return 2;
        }
    }

    const char* LayoutName(uint32_t channels)
    {
        return channels == 8 ? "7.1" : channels == 6 ? "5.1" : "stereo";
    }

    // The game's planar 5.1 `in` (already at the master volume) as `channels` interleaved channels
    // (audio_device.h). Stereo: ITU-style downmix, LFE dropped. 7.1: the surround pair feeds both
    // the side and the back speakers, at -3 dB each (the same power as on a 5.1 setup).
    void Mix(const float (&in)[audio::FRAME_CHANNELS][audio::FRAME_SAMPLES], uint32_t channels, float* out)
    {
        constexpr float k = 0.70710678f;
        for (uint32_t i = 0; i < audio::FRAME_SAMPLES; i++)
        {
            float* o = out + i * channels;
            if (channels == 2)
            {
                float center = in[2][i] * k;
                o[0] = in[0][i] + center + in[4][i] * k;
                o[1] = in[1][i] + center + in[5][i] * k;
                continue;
            }
            for (uint32_t c = 0; c < 4; c++)
                o[c] = in[c][i];
            if (channels == 6)
                o[4] = in[4][i], o[5] = in[5][i];
            else
                o[4] = o[6] = in[4][i] * k, o[5] = o[7] = in[5][i] * k;
        }
    }

    // AUDIO TEST order: the layout's channels (audio_device.h) clockwise from the front left, the
    // subwoofer last; and the speaker each layout channel is.
    const uint8_t TEST_ORDER_2[] = { 0, 1 }, TEST_ORDER_6[] = { 0, 2, 1, 5, 4, 3 }, TEST_ORDER_8[] = { 0, 2, 1, 7, 5, 4, 6, 3 };
    const uint8_t SPEAKERS_8[] = { audio::SPEAKER_FL, audio::SPEAKER_FR, audio::SPEAKER_C, audio::SPEAKER_LFE,
                                   audio::SPEAKER_BL, audio::SPEAKER_BR, audio::SPEAKER_SL, audio::SPEAKER_SR };

    // Adds this frame's AUDIO TEST signal to `out` (g_testChannels interleaved channels).
    void AddTest(float* out)
    {
        const uint8_t* order = g_testChannels == 8 ? TEST_ORDER_8 : g_testChannels == 6 ? TEST_ORDER_6 : TEST_ORDER_2;
        for (uint32_t i = 0; i < audio::FRAME_SAMPLES; i++, g_testSample++)
        {
            uint64_t step = g_testSample / TEST_STEP;
            if (step >= g_testChannels)
            {
                g_testing = false;
                g_testSpeaker = -1;
                Log("[audio] audio test done");
                return;
            }
            uint32_t channel = order[step], within = uint32_t(g_testSample % TEST_STEP);
            g_testSpeaker = g_testChannels == 8 ? SPEAKERS_8[channel] : int(channel);
            // White noise (xorshift) through Paul Kellet's economy pink filter; the LFE gets 50 Hz.
            g_noise ^= g_noise << 13, g_noise ^= g_noise >> 17, g_noise ^= g_noise << 5;
            float white = float(int32_t(g_noise)) / 2147483648.0f;
            g_pink[0] = 0.99765f * g_pink[0] + white * 0.0990460f;
            g_pink[1] = 0.96300f * g_pink[1] + white * 0.2965164f;
            g_pink[2] = 0.57000f * g_pink[2] + white * 1.0526913f;
            float pink = (g_pink[0] + g_pink[1] + g_pink[2] + white * 0.1848f) * 0.11f;
            if (within >= TEST_ON)
                continue;
            // 20 ms fades, so the bursts don't click.
            float fade = std::min({ 1.0f, within / 960.0f, (TEST_ON - within) / 960.0f });
            float value = g_testChannels > 2 && channel == 3 ? 0.4f * std::sin(6.2831853f * 50.0f * within / audio::SAMPLE_RATE) : pink;
            out[i * g_testChannels + channel] += value * fade;
        }
    }

    // Opens the device for audio.output = `layout` (Close first if it is open).
    void OpenDevice(settings::AudioOutput layout)
    {
        g_deviceActive = false;
        if (g_deviceOpen)
            audio::device::Close();
        g_deviceLayout = layout;
        uint32_t channels = audio::device::Open(audio::SAMPLE_RATE, ChannelsOf(layout));
        g_deviceOpen = channels != 0;
        g_deviceChannels = channels;
        g_deviceActive = g_deviceOpen;
    }
}

namespace audio
{
    void StartOutput()
    {
        const settings::AudioOutput layout = settings::AudioOutputMode();
        if (const char* path = getenv("CRASH_AUDIO_DUMP"); path && *path)
        {
            const char* seconds = getenv("CRASH_AUDIO_DUMP_SECONDS");
            uint32_t maxSeconds = seconds ? uint32_t(atoi(seconds)) : 60;
            g_dumpChannels = ChannelsOf(layout);
            g_dump = std::make_unique<WavWriter>(path, SAMPLE_RATE, g_dumpChannels, maxSeconds ? maxSeconds : 60);
            if (g_dump->IsOpen())
                Log("[audio] writing the %s mix to %s (up to %u s)", LayoutName(g_dumpChannels), path, maxSeconds);
            else
                Log("[audio] could not create %s", path);
        }

        const char* noAudio = getenv("CRASH_NOAUDIO");
        g_noDevice = noAudio && *noAudio && strcmp(noAudio, "0") != 0;
        if (g_noDevice)
            Log("[audio] CRASH_NOAUDIO: no host audio device, real-time clock");
        else
            OpenDevice(layout);

        g_nextFrame = Clock::now();
    }

    void WaitForNextFrame()
    {
        // The app in the background (gpu/window.h): no game audio frames until it's back. The
        // device was paused meanwhile, so it isn't taken for stalled at once on return.
        if (gpu::window::Suspended())
        {
            gpu::window::WaitWhileSuspended();
            g_nextFrame = Clock::now();
            g_resumed = g_nextFrame.time_since_epoch().count();
        }
        // audio.output changed in the settings: the device again, in the new layout.
        if (!g_noDevice)
        {
            settings::AudioOutput layout = settings::AudioOutputMode();
            if (layout != g_deviceLayout)
            {
                std::lock_guard lock(g_submitMutex);
                if (!g_shutDown)
                {
                    Log("[audio] output -> %s", LayoutName(ChannelsOf(layout)));
                    OpenDevice(layout);
                    g_nextFrame = Clock::now();
                }
            }
        }
        auto now = Clock::now();
        g_nextFrame += FRAME_PERIOD;

        if (g_deviceActive)
        {
            int queued = device::Queued();
            int64_t ticks = now.time_since_epoch().count();
            bool submitting = ticks - g_lastSubmit < Ticks(std::chrono::milliseconds(100));
            if (queued > 0 && ticks - std::max(device::LastPlayed(), g_resumed) > Ticks(DEVICE_STALL))
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
        // Planar input: channel c, sample i at [c * 256 + i], times the master volume of the
        // settings screen (the dump records what is heard); silent during the AUDIO TEST.
        std::lock_guard lock(g_submitMutex);
        if (g_testRequested.exchange(false))
        {
            g_testing = true;
            g_testSample = 0;
            g_testChannels = g_deviceActive ? g_deviceChannels.load() : g_dumpChannels;
            Log("[audio] audio test: %s", LayoutName(g_testChannels));
        }
        const float master = g_testing ? 0.0f : settings::MasterVolume();
        float in[FRAME_CHANNELS][FRAME_SAMPLES];
        for (uint32_t c = 0; c < FRAME_CHANNELS; c++)
            for (uint32_t i = 0; i < FRAME_SAMPLES; i++)
                in[c][i] = LoadBEFloat(samples + (c * FRAME_SAMPLES + i) * 4) * master;

        // The device's layout (the dump's without a device), then the dump's if it differs.
        const uint32_t channels = g_deviceActive ? g_deviceChannels.load() : g_dumpChannels;
        float out[FRAME_SAMPLES * MAX_CHANNELS], dumpOut[FRAME_SAMPLES * MAX_CHANNELS];
        Mix(in, channels, out);
        if (g_testing && g_testChannels == channels)
            AddTest(out);
        else if (g_testing)
            g_testing = false, g_testSpeaker = -1;  // the layout changed (or the device went away) meanwhile
        float peak = 0;
        for (uint32_t i = 0; i < FRAME_SAMPLES * channels; i++)
        {
            out[i] = std::clamp(out[i], -1.0f, 1.0f);
            peak = std::max(peak, std::abs(out[i]));
        }

        g_peak = std::max(g_peak, peak);
        if (peak > 1.0f / 32768)
            g_peakFrames++;
        g_framesSubmitted++;
        g_lastSubmit = Clock::now().time_since_epoch().count();

        if (g_dump && g_dump->IsOpen() && !g_dump->IsFull())
        {
            const float* mix = out;
            if (g_dumpChannels != channels)
            {
                Mix(in, g_dumpChannels, dumpOut);
                for (uint32_t i = 0; i < FRAME_SAMPLES * g_dumpChannels; i++)
                    dumpOut[i] = std::clamp(dumpOut[i], -1.0f, 1.0f);
                mix = dumpOut;
            }
            int16_t pcm[FRAME_SAMPLES * MAX_CHANNELS];
            for (uint32_t i = 0; i < FRAME_SAMPLES * g_dumpChannels; i++)
                pcm[i] = int16_t(mix[i] * 32767.0f);
            g_dump->Write(pcm, FRAME_SAMPLES);
        }

        if (!g_deviceActive)
            return;
        if (!device::Queue(out, FRAME_SAMPLES))
            g_framesDropped++;
    }

    uint64_t SubmittedFrames()
    {
        return g_framesSubmitted.load(std::memory_order_relaxed);
    }

    uint32_t OutputChannels()
    {
        return g_deviceActive && g_deviceLayout == settings::AudioOutputMode() ? g_deviceChannels.load() : 0;
    }

    void StartChannelTest()
    {
        g_testRequested = true;
    }

    int TestedSpeaker()
    {
        return g_testRequested ? SPEAKER_FL : g_testSpeaker.load();
    }

    void Shutdown()
    {
        std::lock_guard lock(g_submitMutex);
        g_shutDown = true;
        g_dump.reset();  // final header
        if (!g_deviceActive.exchange(false))
            return;
        device::Stop();
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
        Log("[audio] %llu frame(s) submitted (%llu with sound, peak %.3f), %llu dropped, device %s (%s), %d queued",
            (unsigned long long)g_framesSubmitted.load(), (unsigned long long)g_peakFrames.load(), g_peak,
            (unsigned long long)g_framesDropped.load(), g_deviceActive ? "active" : "off", LayoutName(g_deviceChannels),
            g_deviceActive ? device::Queued() : 0);
    }
}
