#include "wav_writer.h"
#include "../platform/crt.h"

#include <cstring>

WavWriter::WavWriter(const std::filesystem::path& path, uint32_t sampleRate, uint32_t channels, uint32_t maxSeconds)
    : sampleRate(sampleRate), channels(channels), maxFrames(uint64_t(sampleRate) * maxSeconds)
{
    file = plat::OpenCFile(path, "wb");
    if (file)
        UpdateHeader();
}

WavWriter::~WavWriter()
{
    if (file)
    {
        UpdateHeader();
        fclose(file);
    }
}

void WavWriter::Write(const int16_t* samples, uint32_t count)
{
    if (!file || frames >= maxFrames)
        return;
    if (frames + count > maxFrames)
        count = uint32_t(maxFrames - frames);

    fwrite(samples, sizeof(int16_t) * channels, count, file);
    frames += count;
    if (frames - framesAtLastHeader >= sampleRate || frames >= maxFrames)
        UpdateHeader();
}

void WavWriter::UpdateHeader()
{
    uint32_t dataBytes = uint32_t(frames * channels * 2);
    uint32_t byteRate = sampleRate * channels * 2;
    uint16_t blockAlign = uint16_t(channels * 2);

    uint8_t header[44];
    auto put32 = [&](int offset, uint32_t value) { memcpy(header + offset, &value, 4); };
    auto put16 = [&](int offset, uint16_t value) { memcpy(header + offset, &value, 2); };
    memcpy(header + 0, "RIFF", 4);
    put32(4, 36 + dataBytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1); // PCM
    put16(22, uint16_t(channels));
    put32(24, sampleRate);
    put32(28, byteRate);
    put16(32, blockAlign);
    put16(34, 16);
    memcpy(header + 36, "data", 4);
    put32(40, dataBytes);

    long position = ftell(file);
    fseek(file, 0, SEEK_SET);
    fwrite(header, 1, sizeof(header), file);
    if (position > long(sizeof(header)))
        fseek(file, position, SEEK_SET);
    fflush(file);
    framesAtLastHeader = frames;
}
