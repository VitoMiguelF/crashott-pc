#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>

// 16-bit PCM WAV file for the debugging dumps (CRASH_AUDIO_DUMP, CRASH_XMA_DUMP). The header is
// rewritten every second of audio, so the file stays playable if the process is killed (the
// watchdog ends runs with ExitProcess). Writing stops after `maxSeconds`.
class WavWriter
{
public:
    WavWriter(const std::filesystem::path& path, uint32_t sampleRate, uint32_t channels, uint32_t maxSeconds);
    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    bool IsOpen() const { return file != nullptr; }
    bool IsFull() const { return frames >= maxFrames; }
    // `count` interleaved frames (count * channels samples, host byte order).
    void Write(const int16_t* samples, uint32_t count);

private:
    void UpdateHeader();

    FILE* file = nullptr;
    uint32_t sampleRate;
    uint32_t channels;
    uint64_t frames = 0;
    uint64_t maxFrames;
    uint64_t framesAtLastHeader = 0;
};
