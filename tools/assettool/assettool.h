#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct RsdInfo
{
    uint32_t channels = 0;          // header channel count (after decoding: total of all streams)
    uint32_t sampleRate = 0;
    uint32_t streams = 0;
    std::vector<uint32_t> streamChannels;
    uint32_t playBegin = 0, playLength = 0;
    uint32_t blockSize = 0, blockCount = 0;
    uint32_t samplesEncoded = 0, samplesInSource = 0;
    uint32_t dataOffset = 0, dataSize = 0;
    uint32_t decodedSamples = 0;
    uint32_t emptyFrames = 0;      // frames (after the first) that decoded to nothing: decoder errors
    std::string sourcePath;         // original .wav path stored in the header
};

RsdInfo ParseRsd(const std::vector<uint8_t>& file);
// 16-bit interleaved PCM of all streams, trimmed to the source length.
std::vector<int16_t> DecodeRsd(const std::vector<uint8_t>& file, RsdInfo& info);

std::vector<uint8_t> ReadFile(const std::filesystem::path& path);
bool WriteFile(const std::filesystem::path& path, const void* data, size_t size);
bool WriteWav(const std::filesystem::path& path, const int16_t* pcm, size_t frames, uint32_t channels, uint32_t sampleRate);

// .bik -> .mp4 (H.264 + AAC) and one .wav per audio track. Returns 0 on success.
int ConvertBink(const std::filesystem::path& input, const std::filesystem::path& output, int bitrateKbps);
