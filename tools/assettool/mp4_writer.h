#pragma once

// Minimal MP4 (ISO BMFF) writer: one H.264 video track and one AAC audio track, samples written
// to a single mdat as they come, moov (with 64-bit chunk offsets) appended at the end. Enough for
// the asset tool's Bink conversion; the bundled FFmpeg build has no libavformat.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

class Mp4Writer
{
public:
    struct Track
    {
        bool video = false;
        uint32_t timescale = 0;
        // video
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> avcC;
        // audio
        uint32_t channels = 0, sampleRate = 0;
        std::vector<uint8_t> audioSpecificConfig;

        std::vector<uint64_t> offsets;
        std::vector<uint32_t> sizes;
        std::vector<uint32_t> durations;
        std::vector<uint32_t> syncSamples; // 1-based
    };

    bool Open(const std::filesystem::path& path);
    int AddTrack(const Track& track);
    void SetAvcC(int track, std::vector<uint8_t> avcC) { m_tracks[track].avcC = std::move(avcC); }
    void WriteSample(int track, const uint8_t* data, size_t size, uint32_t duration, bool sync);
    bool Close();

private:
    FILE* m_file = nullptr;
    uint64_t m_mdatStart = 0;
    uint64_t m_position = 0;
    std::vector<Track> m_tracks;
};

// Annex B (start codes) -> length-prefixed NAL units; SPS/PPS are collected when found.
std::vector<uint8_t> AnnexBToLengthPrefixed(const uint8_t* data, size_t size, std::vector<uint8_t>* sps,
                                            std::vector<uint8_t>* pps);
std::vector<uint8_t> BuildAvcC(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps);
