// .rsd (Radical "RSD6XMA ") -> WAV.
//
// Layout (all found in the game's sound archives; see tools/audio.py for the full write-up):
//   0x000  "RSD6XMA ", u32 LE channels, u32 LE bits (16), u32 LE sample rate, "****",
//          then the original source path ("d:\empire\...\sound\sounds\...\name.wav"), zero-padded
//   0x800  u32 BE fmt size F, u32 BE seek table size S, u32 BE data size D, then F bytes of XMA2
//          format (u8 version 3, u8 stream count, u16 0, u32 play begin, u32 play length, u32 rate,
//          u32 block size (0x10000), u32 samples encoded, u32 samples in source, u32 block count,
//          per stream: u8 channels, u8 0, u16 channel bits), then S bytes of seek table (u32 BE
//          cumulative samples at the end of each block)
//   0x80C+F+S  D bytes of XMA2 packets (2048 bytes each) in blocks of `block size`; in each block
//          stream s starts at packet s and continues through the packet header's skip field.
//
// Decoding follows runtime/audio/xma.cpp: frames are pulled out of the packet bit stream and each
// one is sent to FFmpeg's WMA Pro decoder in a synthetic one-frame packet, which yields 512 samples
// per frame (none for the first one). Multi-stream files are interleaved stream by stream.

#include "assettool.h"
#include "ffmpeg_api.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace
{
    constexpr uint32_t PACKET_BYTES = 2048;
    constexpr uint32_t PACKET_BITS = PACKET_BYTES * 8;
    constexpr uint32_t FRAME_LENGTH_BITS = 15;
    constexpr uint32_t WMAPRO_HEADER_BITS = 4 + 2 + FRAME_LENGTH_BITS;
    constexpr uint16_t XMA_DECODE_FLAGS = 0x10D6;

    uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
    uint32_t LE32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

    struct BitWriter
    {
        uint8_t* data;
        uint32_t bit = 0;
        void Put(uint32_t value, uint32_t count)
        {
            for (uint32_t i = count; i-- > 0; bit++)
            {
                uint8_t mask = uint8_t(0x80 >> (bit & 7));
                if ((value >> i) & 1) data[bit >> 3] |= mask; else data[bit >> 3] &= ~mask;
            }
        }
        void Copy(const uint8_t* source, uint32_t sourceBit, uint32_t count)
        {
            for (uint32_t i = 0; i < count; i++, sourceBit++)
                Put((source[sourceBit >> 3] >> (7 - (sourceBit & 7))) & 1, 1);
        }
    };

    uint32_t PeekBits(const uint8_t* data, uint32_t bit, uint32_t count)
    {
        uint32_t value = 0;
        for (uint32_t i = 0; i < count; i++, bit++)
            value = (value << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1);
        return value;
    }

    // The packets of one stream, in order, and a reader over their bit stream (headers skipped).
    struct StreamReader
    {
        std::vector<const uint8_t*> packets;
        uint32_t packet = 0;   // index into packets
        uint32_t bit = 0;      // bit inside the packet (>= 32)
        uint32_t startedHere = 0;

        static uint32_t FrameCount(const uint8_t* p) { return BE32(p) >> 26; }
        static uint32_t FrameOffset(const uint8_t* p) { return (BE32(p) >> 11) & 0x7FFF; }

        bool AtEnd() const { return packet >= packets.size(); }

        void StartPacket(uint32_t index)
        {
            packet = index;
            startedHere = 0;
            if (!AtEnd())
                bit = 32 + FrameOffset(packets[packet]);
        }

        // Copies `count` bits (crossing packets) to `out`; false if the stream ends first.
        bool Read(uint32_t count, BitWriter* out, uint32_t& p, uint32_t& b) const
        {
            while (count > 0)
            {
                if (p >= packets.size())
                    return false;
                uint32_t n = std::min(count, PACKET_BITS - b);
                if (out)
                    out->Copy(packets[p], b, n);
                b += n;
                count -= n;
                if (b == PACKET_BITS)
                {
                    p++;
                    b = 32;
                }
            }
            return true;
        }

        // Next frame's bits (with its length field); false at the end of the stream.
        bool NextFrame(std::vector<uint8_t>& frame, uint32_t& length)
        {
            for (int guard = 0; guard < 100000; guard++)
            {
                if (AtEnd())
                    return false;
                const uint8_t* p = packets[packet];
                if (FrameCount(p) == 0 || startedHere >= FrameCount(p))
                {
                    StartPacket(packet + 1);
                    continue;
                }
                uint8_t lengthBits[4] = {};
                BitWriter lw{ lengthBits };
                uint32_t pp = packet, bb = bit;
                if (!Read(FRAME_LENGTH_BITS, &lw, pp, bb))
                    return false;
                length = PeekBits(lengthBits, 0, FRAME_LENGTH_BITS);
                if (length <= FRAME_LENGTH_BITS || length == 0x7FFF)
                {
                    StartPacket(packet + 1);
                    continue;
                }
                frame.assign((length + 7) / 8 + 1, 0);
                BitWriter fw{ frame.data() };
                pp = packet;
                bb = bit;
                if (!Read(length, &fw, pp, bb))
                    return false;
                if (pp != packet)
                {
                    packet = pp;
                    startedHere = 0;
                }
                else
                {
                    startedHere++;
                }
                bit = bb;
                return true;
            }
            return false;
        }
    };

    struct WmaDecoder
    {
        AVCodecContext* codec = nullptr;
        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
        uint8_t sequence = 0;
        uint32_t channels;
        std::vector<float> pcm; // interleaved

        WmaDecoder(uint32_t channels, uint32_t sampleRate) : channels(channels)
        {
            const ff::Api& f = ff::Get();
            const AVCodec* wma = f.avcodec_find_decoder(AV_CODEC_ID_WMAPRO);
            if (!wma)
                throw std::runtime_error("FFmpeg has no WMA Pro decoder");
            codec = f.avcodec_alloc_context3(wma);
            packet = f.av_packet_alloc();
            frame = f.av_frame_alloc();
            uint32_t channelMask = channels == 2 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
            auto* extradata = static_cast<uint8_t*>(f.av_mallocz(18 + AV_INPUT_BUFFER_PADDING_SIZE));
            extradata[0] = 16;
            memcpy(extradata + 2, &channelMask, 4);
            memcpy(extradata + 14, &XMA_DECODE_FLAGS, 2);
            codec->extradata = extradata;
            codec->extradata_size = 18;
            codec->sample_rate = int(sampleRate);
            codec->block_align = PACKET_BYTES;
            f.av_channel_layout_from_mask(&codec->ch_layout, channelMask);
            if (f.avcodec_open2(codec, wma, nullptr) < 0)
                throw std::runtime_error("could not open the WMA Pro decoder");
        }

        ~WmaDecoder()
        {
            const ff::Api& f = ff::Get();
            f.avcodec_free_context(&codec);
            f.av_packet_free(&packet);
            f.av_frame_free(&frame);
        }

        void Send(uint32_t previousBits, const uint8_t* source, uint32_t sourceBit, uint32_t count)
        {
            const ff::Api& f = ff::Get();
            alignas(16) uint8_t buffer[PACKET_BYTES + AV_INPUT_BUFFER_PADDING_SIZE];
            memset(buffer, 0xFF, PACKET_BYTES);
            memset(buffer + PACKET_BYTES, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            BitWriter writer{ buffer };
            writer.Put(sequence++ & 0xF, 4);
            writer.Put(0, 2);
            writer.Put(previousBits, FRAME_LENGTH_BITS);
            writer.Copy(source, sourceBit, count);
            packet->data = buffer;
            packet->size = PACKET_BYTES;
            f.avcodec_send_packet(codec, packet);
            while (f.avcodec_receive_frame(codec, frame) >= 0)
            {
                int n = frame->ch_layout.nb_channels;
                if (frame->format != AV_SAMPLE_FMT_FLTP || n <= 0)
                    continue;
                for (int i = 0; i < frame->nb_samples; i++)
                    for (uint32_t c = 0; c < channels; c++)
                        pcm.push_back(reinterpret_cast<const float*>(frame->extended_data[std::min<int>(c, n - 1)])[i]);
            }
            packet->data = nullptr;
            packet->size = 0;
        }

        void DecodeFrame(const uint8_t* bits, uint32_t length)
        {
            uint32_t first = std::min(length, PACKET_BITS - WMAPRO_HEADER_BITS);
            Send(0, bits, 0, first);
            if (first < length)
                Send(length - first, bits, first, length - first);
        }
    };
}

RsdInfo ParseRsd(const std::vector<uint8_t>& file)
{
    if (file.size() < 0x80C + 0x24 || memcmp(file.data(), "RSD6XMA ", 8) != 0)
        throw std::runtime_error("not an RSD6XMA file");
    RsdInfo info;
    info.channels = LE32(&file[8]);
    info.sampleRate = LE32(&file[0x10]);
    const uint8_t* h = &file[0x800];
    uint32_t fmtSize = BE32(h), seekSize = BE32(h + 4), dataSize = BE32(h + 8);
    const uint8_t* fmt = h + 12;
    info.streams = fmt[1];
    info.playBegin = BE32(fmt + 4);
    info.playLength = BE32(fmt + 8);
    info.blockSize = BE32(fmt + 16);
    info.samplesEncoded = BE32(fmt + 20);
    info.samplesInSource = BE32(fmt + 24);
    info.blockCount = BE32(fmt + 28);
    for (uint32_t s = 0; s < info.streams; s++)
        info.streamChannels.push_back(fmt[32 + s * 4]);
    info.dataOffset = 0x80C + fmtSize + seekSize;
    info.dataSize = dataSize;
    if (info.dataOffset + dataSize > file.size() || info.blockSize % PACKET_BYTES || !info.blockSize || !info.streams)
        throw std::runtime_error("bad RSD6XMA header");
    const char* path = reinterpret_cast<const char*>(&file[0x18]);
    info.sourcePath.assign(path, strnlen(path, 0x800 - 0x18));
    return info;
}

std::vector<int16_t> DecodeRsd(const std::vector<uint8_t>& file, RsdInfo& info)
{
    info = ParseRsd(file);
    const uint8_t* data = &file[info.dataOffset];
    uint32_t packetCount = info.dataSize / PACKET_BYTES;
    uint32_t packetsPerBlock = info.blockSize / PACKET_BYTES;

    std::vector<std::vector<float>> streamPcm(info.streams);
    uint32_t totalChannels = 0;
    for (uint32_t s = 0; s < info.streams; s++)
    {
        StreamReader reader;
        for (uint32_t block = 0; block * packetsPerBlock < packetCount; block++)
        {
            uint32_t first = block * packetsPerBlock, end = std::min(packetCount, first + packetsPerBlock);
            for (uint32_t p = first + s; p < end; p += 1 + (BE32(data + p * PACKET_BYTES) & 0xFF))
                reader.packets.push_back(data + p * PACKET_BYTES);
        }
        uint32_t channels = info.streamChannels[s];
        totalChannels += channels;
        // XMA frames are always 512 samples, but FFmpeg derives the WMA Pro frame size (and the band
        // layout) from the rate. The hardware only runs at 24000/32000/44100/48000 Hz; a file is
        // decoded at the lowest of those not below its header rate (11025 Hz effects: 24000), and if
        // some frames fail to decode (a few files were encoded at another rate than their header
        // says) the other rates are tried and the best result kept.
        std::vector<uint32_t> rates;
        for (uint32_t rate : { 24000u, 32000u, 44100u, 48000u })
            if (info.sampleRate <= rate || rate == 48000)
                rates.push_back(rate);
        for (uint32_t rate : { 44100u, 32000u, 24000u })
            if (std::find(rates.begin(), rates.end(), rate) == rates.end())
                rates.push_back(rate);
        uint32_t bestEmpty = UINT32_MAX;
        for (uint32_t rate : rates)
        {
            WmaDecoder decoder(channels, rate);
            reader.StartPacket(0);
            std::vector<uint8_t> frame;
            uint32_t length, decodedFrames = 0, empty = 0;
            while (reader.NextFrame(frame, length))
            {
                size_t before = decoder.pcm.size();
                decoder.DecodeFrame(frame.data(), length);
                size_t produced = (decoder.pcm.size() - before) / channels;
                if (produced == 0 && decodedFrames > 0)
                    empty++;
                decodedFrames++;
                if (getenv("ASSETTOOL_DEBUG"))
                    fprintf(stderr, "stream %u at %u Hz: frame of %u bits -> %zu samples\n", s, rate, length, produced);
            }
            // The last frame may legitimately be trimmed to nothing.
            if (empty < bestEmpty)
            {
                bestEmpty = empty;
                streamPcm[s] = std::move(decoder.pcm);
            }
            if (empty <= 1)
                break;
        }
        info.emptyFrames += bestEmpty > 1 ? bestEmpty : 0;
    }

    // Interleave the streams and trim to the source length.
    size_t samples = SIZE_MAX;
    for (uint32_t s = 0; s < info.streams; s++)
        samples = std::min(samples, streamPcm[s].size() / info.streamChannels[s]);
    if (info.samplesInSource && info.samplesInSource < samples)
        samples = info.samplesInSource;
    info.channels = totalChannels;
    std::vector<int16_t> pcm(samples * totalChannels);
    for (size_t i = 0; i < samples; i++)
    {
        uint32_t out = 0;
        for (uint32_t s = 0; s < info.streams; s++)
            for (uint32_t c = 0; c < info.streamChannels[s]; c++)
            {
                float v = streamPcm[s][i * info.streamChannels[s] + c];
                pcm[i * totalChannels + out++] = int16_t(std::clamp(v * 32768.0f, -32768.0f, 32767.0f));
            }
    }
    info.decodedSamples = uint32_t(samples);
    return pcm;
}
