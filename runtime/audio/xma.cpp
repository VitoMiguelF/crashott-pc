// XMA decoder hardware.
//
// How the title drives it (the XDK's XMA playback library, sub_823EDF50 & co.; Radical's streamer
// on top of it, e.g. sub_822CC530):
//  - XMACreateContext returns a context; the title reads the context array's physical address from
//    register 0x600 once (sub_823ED260) and derives each context's index from it.
//  - Registers are little-endian (lwbrx/stwbrx). Writing a bitmask to 0x650+n kicks (enables)
//    contexts 32n..32n+31, 0x690+n locks (disables) them and 0x6A0+n clears them. Register 0x606
//    reads "busy" as index | 0x200; it stays 0 here, because a lock waits for the decoder instead.
//  - To change a context the title locks it, copies the 64 bytes to a shadow, edits the shadow
//    (submits input buffers, consumes output by moving the read offset), copies it back and kicks.
//
// Context layout (big-endian dwords, bitfields from bit 0 of each dword; as in Xenia's
// XMA_CONTEXT_DATA, BSD): 0: input buffer 0 packet count :12, loop count :8, input 0 valid :1,
// input 1 valid :1, output block count :5, output write offset :5. 1: input buffer 1 packet count
// :12, loop subframes :8, subframe decode count :4, padding :3, sample rate :2, stereo :1, ?:1,
// output valid :1. 2: input read offset (bits from the start of the current buffer) :26, error :6.
// 3: loop start :26. 4: loop end :26, packet metadata :5, current buffer :1. 5/6: input buffer 0/1
// (physical). 7: output buffer (physical). 8: work buffer. 9: output read offset :5.
//
// Decoding: input buffers are XMA2 packets of 2048 bytes (32-bit header: frame count :6, bit offset
// of the first frame that starts in the packet :15, metadata :3, packets to skip to reach this
// stream's next one :8). Frames are a bit stream that continues across packets (skipping headers)
// and across the two input buffers; each frame starts with its length in bits (15 bits, 0x7FFF =
// padding to the end of the packet) and decodes to 512 samples per channel. The parser here follows
// the stream exactly as the hardware state describes it (read offset, buffer switch, loops), and
// each extracted frame goes to FFmpeg's WMA Pro decoder (XMA frames are WMA Pro frames with decode
// flags 0x10D6) wrapped in a synthetic one-frame WMA Pro packet. That keeps the output at exactly
// 512 samples per frame, which FFmpeg's own XMA2 wrapper doesn't (it holds back 4096 samples).
// Output is 16-bit big-endian PCM, interleaved for stereo, written in 256-byte blocks to the output
// ring; when the write offset catches up with the read offset the hardware clears "output valid"
// (the title then treats the whole ring as full, see sub_823ED760).

#include "xma.h"
#include "audio_override.h"
#include "../subtitles/subtitles.h"
#include "ffmpeg.h"
#include "wav_writer.h"
#include "../diagnostics.h"
#include "../kernel/memory.h"

#include "../platform/platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    constexpr uint32_t CONTEXT_COUNT = 320;
    constexpr uint32_t CONTEXT_SIZE = 64;
    constexpr uint32_t CONTEXT_DWORDS = 10; // dwords 10-15 are unused here

    constexpr uint32_t PACKET_BYTES = 2048;
    constexpr uint32_t PACKET_BITS = PACKET_BYTES * 8;
    constexpr uint32_t PACKET_HEADER_BITS = 32;
    constexpr uint32_t FRAME_LENGTH_BITS = 15;
    constexpr uint32_t SAMPLES_PER_FRAME = 512;
    constexpr uint32_t OUTPUT_BLOCK_BYTES = 256;
    constexpr uint32_t OUTPUT_BLOCK_SAMPLES = OUTPUT_BLOCK_BYTES / 2; // int16 values per block
    constexpr uint32_t MAX_FRAMES_PER_PASS = 64;

    // Registers (Xenia's xma_register_table.inc); the trapped page holds registers 0x400-0x7FF.
    constexpr uint32_t REG_CONTEXT_ARRAY_ADDRESS = 0x600;
    constexpr uint32_t REG_CONTEXT_KICK = 0x650;
    constexpr uint32_t REG_CONTEXT_LOCK = 0x690;
    constexpr uint32_t REG_CONTEXT_CLEAR = 0x6A0;
    constexpr uint32_t REG_GROUPS = CONTEXT_COUNT / 32;
    constexpr uint32_t TRAPPED_PAGE = XMA_MMIO_BEGIN + 0x1000;
    constexpr uint32_t LOGGED_REGISTER_WRITES = 16;
    constexpr uint32_t LOGGED_CONTEXT_EVENTS = 24;

    constexpr uint32_t SAMPLE_RATES[4] = { 24000, 32000, 44100, 48000 };

    // Synthetic WMA Pro packet: sequence number :4, unused :2, bits of the previous frame :15,
    // then frame data, padded with 1 bits (a frame length of 0x7FFF ends the packet).
    constexpr uint32_t WMAPRO_HEADER_BITS = 4 + 2 + FRAME_LENGTH_BITS;
    constexpr uint16_t XMA_DECODE_FLAGS = 0x10D6;

    uint32_t Bits(uint32_t value, uint32_t shift, uint32_t count)
    {
        return (value >> shift) & ((1u << count) - 1);
    }

    void SetBits(uint32_t& value, uint32_t shift, uint32_t count, uint32_t field)
    {
        uint32_t mask = ((1u << count) - 1) << shift;
        value = (value & ~mask) | ((field << shift) & mask);
    }

    struct ContextData
    {
        uint32_t d[CONTEXT_DWORDS];

        uint32_t PacketCount(uint32_t buffer) const { return Bits(d[buffer], 0, 12); } // dword 0 / dword 1
        uint32_t LoopCount() const { return Bits(d[0], 12, 8); }
        void SetLoopCount(uint32_t value) { SetBits(d[0], 12, 8, value); }
        bool InputValid(uint32_t buffer) const { return Bits(d[0], 20 + buffer, 1); }
        void SetInputValid(uint32_t buffer, bool valid) { SetBits(d[0], 20 + buffer, 1, valid); }
        uint32_t OutputBlockCount() const { return Bits(d[0], 22, 5); }
        uint32_t OutputWriteOffset() const { return Bits(d[0], 27, 5); }
        void SetOutputWriteOffset(uint32_t value) { SetBits(d[0], 27, 5, value); }
        uint32_t SampleRate() const { return SAMPLE_RATES[Bits(d[1], 27, 2)]; }
        uint32_t Channels() const { return Bits(d[1], 29, 1) ? 2 : 1; }
        bool OutputValid() const { return Bits(d[1], 31, 1); }
        void SetOutputValid(bool valid) { SetBits(d[1], 31, 1, valid); }
        uint32_t ReadOffset() const { return Bits(d[2], 0, 26); }
        void SetReadOffset(uint32_t value) { SetBits(d[2], 0, 26, value); }
        uint32_t LoopStart() const { return Bits(d[3], 0, 26); }
        uint32_t LoopEnd() const { return Bits(d[4], 0, 26); }
        uint32_t CurrentBuffer() const { return Bits(d[4], 31, 1); }
        void SetCurrentBuffer(uint32_t buffer) { SetBits(d[4], 31, 1, buffer); }
        uint32_t InputBuffer(uint32_t buffer) const { return d[5 + buffer]; }
        uint32_t OutputBuffer() const { return d[7]; }
        uint32_t OutputReadOffset() const { return Bits(d[9], 0, 5); }
    };

    uint8_t* PhysicalToHost(uint32_t physical)
    {
        return g_memory.Translate(PHYSICAL_VIEW_A + (physical & (PHYSICAL_SIZE - 1)));
    }

    uint32_t ReadBE32(const uint8_t* p)
    {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }

    uint32_t PeekBits(const uint8_t* data, uint32_t bit, uint32_t count)
    {
        uint32_t value = 0;
        for (uint32_t i = 0; i < count; i++, bit++)
            value = (value << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1);
        return value;
    }

    // MSB-first bit writer over a byte buffer.
    struct BitWriter
    {
        uint8_t* data;
        uint32_t bit = 0;

        void Put(uint32_t value, uint32_t count)
        {
            for (uint32_t i = count; i-- > 0; bit++)
            {
                uint8_t mask = uint8_t(0x80 >> (bit & 7));
                if ((value >> i) & 1)
                    data[bit >> 3] |= mask;
                else
                    data[bit >> 3] &= ~mask;
            }
        }

        void Copy(const uint8_t* source, uint32_t sourceBit, uint32_t count)
        {
            for (uint32_t i = 0; i < count; i++, sourceBit++)
                Put((source[sourceBit >> 3] >> (7 - (sourceBit & 7))) & 1, 1);
        }
    };

    uint32_t HeaderFrameCount(uint32_t header) { return header >> 26; }
    uint32_t HeaderFrameOffset(uint32_t header) { return Bits(header, 11, 15); }
    uint32_t HeaderSkip(uint32_t header) { return header & 0xFF; }

    struct HostContext
    {
        std::mutex mutex;            // held while the decoder works on the context
        std::atomic<bool> enabled;   // kicked and not locked
        bool allocated = false;
        uint32_t guest = 0;

        // Decoder state.
        uint32_t channels = 0;
        uint32_t sampleRate = 0;
#if CRASH_HAVE_FFMPEG
        AVCodecContext* codec = nullptr;
        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
#endif
        uint8_t sequence = 0;
        std::vector<int16_t> pcm;    // decoded, not yet written to the output ring (interleaved)
        size_t pcmRead = 0;
        uint32_t lastWriteOffset = 0; // output write offset as last stored, to spot stream resets
        uint64_t frames = 0;
        uint64_t streamFrames = 0;   // frames since the last reset
        uint32_t streamNumber = 0;
        std::unique_ptr<WavWriter> dump;
        uint32_t tracedBuffers[2] = {}; // CRASH_XMA_TRACE: input buffers already logged
        bool inputValid[2] = {};        // valid bits as last stored: a buffer valid again is a new submission
    };

    HostContext g_contexts[CONTEXT_COUNT];
    std::mutex g_allocationMutex;
    uint32_t g_contextArray;

#if CRASH_HAVE_FFMPEG
    const ffmpeg::Api* g_ffmpeg;
#endif
    std::filesystem::path g_dumpDirectory;
    bool g_trace;

    std::mutex g_workMutex;
    std::condition_variable g_workCondition;
    bool g_workPending;

    std::atomic<uint64_t> g_kicks, g_locks, g_clears, g_framesDecoded, g_decodeErrors, g_paddedFrames, g_blocksWritten;
    std::atomic<uint32_t> g_loggedRegisterWrites, g_loggedContextEvents;
    std::atomic<uint32_t> g_emulatedStores, g_steppedStores;

    thread_local uint32_t t_pendingWrite; // guest address of a store being single-stepped

    bool LogContextEvent()
    {
        return g_loggedContextEvents.fetch_add(1, std::memory_order_relaxed) < LOGGED_CONTEXT_EVENTS;
    }

    void LoadContext(uint32_t guest, ContextData& data)
    {
        for (uint32_t i = 0; i < CONTEXT_DWORDS; i++)
            data.d[i] = LoadBE32(guest + i * 4);
    }

    void FreeDecoder(HostContext& h)
    {
#if CRASH_HAVE_FFMPEG
        if (h.codec)
            g_ffmpeg->avcodec_free_context(&h.codec);
        if (h.packet)
            g_ffmpeg->av_packet_free(&h.packet);
        if (h.frame)
            g_ffmpeg->av_frame_free(&h.frame);
#endif
        h.channels = h.sampleRate = 0;
    }

    // Starts a new stream: drops pending PCM and the decoder's overlap state.
    void ResetStream(HostContext& h)
    {
        h.pcm.clear();
        h.pcmRead = 0;
        h.lastWriteOffset = 0;
        h.sequence = 0;
        h.streamFrames = 0;
        h.dump.reset();
#if CRASH_HAVE_FFMPEG
        if (h.codec)
            g_ffmpeg->avcodec_flush_buffers(h.codec);
#endif
    }

    void OpenDump(HostContext& h, uint32_t index)
    {
        if (g_dumpDirectory.empty() || h.dump)
            return;
        auto path = g_dumpDirectory / ("xma_" + std::to_string(index) + "_" + std::to_string(h.streamNumber++) + ".wav");
        h.dump = std::make_unique<WavWriter>(path, h.sampleRate, h.channels, 120);
    }

    void EnsureDecoder(HostContext& h, uint32_t index, uint32_t channels, uint32_t sampleRate)
    {
        if (h.channels == channels && h.sampleRate == sampleRate)
            return;
        FreeDecoder(h);
        ResetStream(h);
        h.channels = channels;
        h.sampleRate = sampleRate;

#if CRASH_HAVE_FFMPEG
        if (g_ffmpeg)
        {
            const AVCodec* codec = g_ffmpeg->avcodec_find_decoder(AV_CODEC_ID_WMAPRO);
            h.codec = g_ffmpeg->avcodec_alloc_context3(codec);
            h.packet = g_ffmpeg->av_packet_alloc();
            h.frame = g_ffmpeg->av_frame_alloc();
            if (h.codec && h.packet && h.frame)
            {
                // WAVEFORMATEX extension of WMA Pro: bits per sample, channel mask, decode flags.
                uint32_t channelMask = channels == 2 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
                auto* extradata = static_cast<uint8_t*>(g_ffmpeg->av_mallocz(18 + AV_INPUT_BUFFER_PADDING_SIZE));
                extradata[0] = 16;
                memcpy(extradata + 2, &channelMask, 4);
                memcpy(extradata + 14, &XMA_DECODE_FLAGS, 2);
                h.codec->extradata = extradata;
                h.codec->extradata_size = 18;
                h.codec->sample_rate = int(sampleRate);
                h.codec->block_align = PACKET_BYTES;
                g_ffmpeg->av_channel_layout_from_mask(&h.codec->ch_layout, channelMask);
                if (g_ffmpeg->avcodec_open2(h.codec, codec, nullptr) < 0)
                {
                    Log("[xma] context %u: could not open the WMA Pro decoder (%u ch, %u Hz): output is silence",
                        index, channels, sampleRate);
                    FreeDecoder(h);
                    h.channels = channels;
                    h.sampleRate = sampleRate;
                }
            }
        }
#endif
        if (LogContextEvent())
            Log("[xma] context %u: %s, %u Hz", index, channels == 2 ? "stereo" : "mono", sampleRate);
    }

#if CRASH_HAVE_FFMPEG
    // Sends one synthetic WMA Pro packet: `previousBits` bits that finish the previous packet's
    // frame, then `count` bits of `source` from `sourceBit`.
    void SendPacket(HostContext& h, uint32_t previousBits, const uint8_t* source, uint32_t sourceBit, uint32_t count)
    {
        alignas(16) uint8_t buffer[PACKET_BYTES + AV_INPUT_BUFFER_PADDING_SIZE];
        memset(buffer, 0xFF, PACKET_BYTES);
        memset(buffer + PACKET_BYTES, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        BitWriter writer{ buffer };
        writer.Put(h.sequence++ & 0xF, 4);
        writer.Put(0, 2);
        writer.Put(previousBits, FRAME_LENGTH_BITS);
        writer.Copy(source, sourceBit, count);

        h.packet->data = buffer;
        h.packet->size = PACKET_BYTES;
        if (g_ffmpeg->avcodec_send_packet(h.codec, h.packet) < 0)
            g_decodeErrors++;

        int result;
        while ((result = g_ffmpeg->avcodec_receive_frame(h.codec, h.frame)) >= 0)
        {
            const AVFrame* frame = h.frame;
            int frameChannels = frame->ch_layout.nb_channels;
            if (frame->format == AV_SAMPLE_FMT_FLTP && frameChannels > 0)
            {
                for (int i = 0; i < frame->nb_samples; i++)
                {
                    for (uint32_t c = 0; c < h.channels; c++)
                    {
                        float sample = reinterpret_cast<const float*>(frame->extended_data[std::min<int>(c, frameChannels - 1)])[i];
                        h.pcm.push_back(int16_t(std::clamp(sample * 32768.0f, -32768.0f, 32767.0f)));
                    }
                }
            }
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF)
            g_decodeErrors++;
        h.packet->data = nullptr;
        h.packet->size = 0;
    }
#endif

    // Decodes one frame (`bitLength` bits, MSB first, starting with its length field) into
    // exactly SAMPLES_PER_FRAME samples per channel appended to h.pcm. The WMA Pro decoder
    // outputs nothing for the first frame after a reset (the overlap isn't primed yet) and less
    // on errors; the difference is padded with silence so the hardware's 512-samples-per-frame
    // accounting holds.
    void DecodeFrame(HostContext& h, uint32_t index, const uint8_t* frameBits, uint32_t bitLength)
    {
        size_t before = h.pcm.size();
        // Marker frames of a mod's stand-in .rsd: samples come from a loose WAV/Ogg file.
        bool replaced = audio_override::DecodeMarkerFrame(frameBits, bitLength, h.channels, h.pcm);
#if CRASH_HAVE_FFMPEG
        if (h.codec && !replaced)
        {
            uint32_t first = std::min(bitLength, PACKET_BITS - WMAPRO_HEADER_BITS);
            SendPacket(h, 0, frameBits, 0, first);
            if (first < bitLength)
                SendPacket(h, bitLength - first, frameBits, first, bitLength - first);
        }
#endif
        size_t expected = before + size_t(SAMPLES_PER_FRAME) * h.channels;
        if (h.pcm.size() < expected)
        {
            // Besides the first frame of a stream, this is the last frame of a stream whose end the
            // encoder trimmed (WMA Pro trim_end): the hardware still outputs 512 samples.
            if (h.streamFrames > 0)
                g_paddedFrames++;
            h.pcm.resize(expected, 0);
        }

        h.frames++;
        h.streamFrames++;
        g_framesDecoded++;
        if (!g_dumpDirectory.empty())
        {
            OpenDump(h, index);
            if (h.dump)
                h.dump->Write(h.pcm.data() + before, uint32_t((h.pcm.size() - before) / h.channels));
        }
    }

    // Packet index of the stream's packet after `packet` (packets of other streams are skipped).
    // It can be past the end of the buffer: the skip then continues into the other input buffer
    // (see CarryIntoNextBuffer).
    uint32_t NextStreamPacket(const uint8_t* buffer, uint32_t packet)
    {
        return packet + 1 + HeaderSkip(ReadBE32(buffer + packet * PACKET_BYTES));
    }

    // Read offset in the other input buffer for a stream position `offset` at or past the end of
    // `buffer`. Multi-stream XMA2 (the 6-channel cutscene dialogue: 3 stereo streams in 3 contexts)
    // interleaves the streams' packets in one block, and the title gives every context the same
    // buffers, each starting at its own stream's first packet. A packet's skip count can reach
    // past the end of the buffer; the hardware carries the remainder into the next buffer, so
    // each stream lands on its own packet there instead of on packet 0 (stream 0).
    uint32_t CarryIntoNextBuffer(const ContextData& data, uint32_t buffer, uint32_t offset)
    {
        return (offset / PACKET_BITS - data.PacketCount(buffer)) * PACKET_BITS + offset % PACKET_BITS;
    }

    // Reads `count` bits of the stream from (buffer, offset), crossing packets (skipping headers
    // and other streams' packets) and into the other input buffer; `out` may be null to skip them.
    // Positions are kept just after a packet header once a packet is finished. False if the bits
    // run past the valid input.
    bool ReadStream(const ContextData& data, uint32_t& buffer, uint32_t& offset, uint32_t count, BitWriter* out)
    {
        while (count > 0)
        {
            uint32_t packet = offset / PACKET_BITS;
            if (packet >= data.PacketCount(buffer))
            {
                uint32_t other = buffer ^ 1;
                if (!data.InputValid(other) || data.PacketCount(other) == 0)
                    return false;
                offset = CarryIntoNextBuffer(data, buffer, offset);
                if (offset % PACKET_BITS < PACKET_HEADER_BITS)
                    offset += PACKET_HEADER_BITS - offset % PACKET_BITS;
                buffer = other;
                continue;
            }

            const uint8_t* bytes = PhysicalToHost(data.InputBuffer(buffer));
            uint32_t n = std::min(count, PACKET_BITS - offset % PACKET_BITS);
            if (out)
                out->Copy(bytes, offset, n);
            offset += n;
            count -= n;
            if (offset % PACKET_BITS == 0)
                offset = NextStreamPacket(bytes, offset / PACKET_BITS - 1) * PACKET_BITS + PACKET_HEADER_BITS;
        }
        return true;
    }

    // Length field of the frame at (buffer, offset); it can straddle a packet boundary. 0 if the
    // input ends first.
    uint32_t PeekFrameLength(const ContextData& data, uint32_t buffer, uint32_t offset)
    {
        uint8_t bits[4] = {};
        BitWriter writer{ bits };
        if (!ReadStream(data, buffer, offset, FRAME_LENGTH_BITS, &writer))
            return 0;
        return PeekBits(bits, 0, FRAME_LENGTH_BITS);
    }

    bool IsFrameLength(uint32_t length)
    {
        return length > FRAME_LENGTH_BITS && length != 0x7FFF;
    }

    // Number of frames that start in `packet` (of the current buffer) before `offset`, walking
    // from the packet's first frame.
    uint32_t FramesStartedBefore(const ContextData& data, uint32_t buffer, uint32_t packet, uint32_t offset)
    {
        const uint8_t* bytes = PhysicalToHost(data.InputBuffer(buffer));
        uint32_t position = packet * PACKET_BITS + PACKET_HEADER_BITS + HeaderFrameOffset(ReadBE32(bytes + packet * PACKET_BYTES));
        uint32_t count = 0;
        while (position < offset && position / PACKET_BITS == packet && count < 64)
        {
            uint32_t length = PeekFrameLength(data, buffer, position);
            uint32_t frameBuffer = buffer;
            if (!IsFrameLength(length) || !ReadStream(data, frameBuffer, position, length, nullptr) || frameBuffer != buffer)
                break;
            count++;
        }
        return count;
    }

    // Moves the context to the next frame start, releasing input buffers it runs past (as the
    // hardware does, so the title can refill them). Each packet header gives the number of frames
    // that start in it; after the last one the rest of the packet is padding (or an end-of-stream
    // marker). Returns the frame's length in bits, or 0 when there's no input.
    uint32_t SeekFrame(ContextData& data)
    {
        for (uint32_t guard = 0; guard < 2 * 4096 + 8; guard++)
        {
            uint32_t buffer = data.CurrentBuffer();
            if (!data.InputValid(buffer))
            {
                if (!data.InputValid(buffer ^ 1))
                    return 0;
                data.SetCurrentBuffer(buffer ^ 1);
                data.SetReadOffset(0);
                continue;
            }

            const uint8_t* bytes = PhysicalToHost(data.InputBuffer(buffer));
            uint32_t offset = data.ReadOffset();
            uint32_t packet = offset / PACKET_BITS;
            uint32_t bit = offset % PACKET_BITS;
            if (packet >= data.PacketCount(buffer))
            {
                // Buffer consumed: release it and continue with the other one.
                data.SetInputValid(buffer, false);
                data.SetCurrentBuffer(buffer ^ 1);
                data.SetReadOffset(CarryIntoNextBuffer(data, buffer, packet * PACKET_BITS));
                continue;
            }

            uint32_t header = ReadBE32(bytes + packet * PACKET_BYTES);
            if (bit < PACKET_HEADER_BITS)
            {
                // At a packet start: go to the first frame that starts in it.
                if (HeaderFrameCount(header) == 0)
                    data.SetReadOffset(NextStreamPacket(bytes, packet) * PACKET_BITS);
                else
                    data.SetReadOffset(packet * PACKET_BITS + PACKET_HEADER_BITS + HeaderFrameOffset(header));
                continue;
            }

            if (FramesStartedBefore(data, buffer, packet, offset) >= HeaderFrameCount(header))
            {
                data.SetReadOffset(NextStreamPacket(bytes, packet) * PACKET_BITS);
                continue;
            }

            uint32_t length = PeekFrameLength(data, buffer, offset);
            if (!length)
                return 0; // the length field continues in a buffer that isn't there yet
            if (!IsFrameLength(length))
            {
                data.SetReadOffset(NextStreamPacket(bytes, packet) * PACKET_BITS);
                continue;
            }
            return length;
        }
        return 0;
    }

    // Parses and decodes the next frame. False when the input runs out (the context waits for
    // the title to submit more).
    bool DecodeNextFrame(HostContext& h, uint32_t index, ContextData& data)
    {
        // Loops: at the loop end, go back to the loop start (count 255 = forever).
        if (data.LoopCount() > 0 && data.LoopEnd() != 0 && data.ReadOffset() == data.LoopEnd())
        {
            data.SetReadOffset(data.LoopStart());
            if (data.LoopCount() != 255)
                data.SetLoopCount(data.LoopCount() - 1);
        }

        uint32_t length = SeekFrame(data);
        if (!length)
            return false;

        uint8_t frameBits[(0x7FFF + 7) / 8 + 1];
        BitWriter writer{ frameBits };
        uint32_t buffer = data.CurrentBuffer();
        uint32_t offset = data.ReadOffset();
        if (!ReadStream(data, buffer, offset, length, &writer))
            return false; // frame continues in a buffer that isn't there yet

        if (buffer != data.CurrentBuffer())
        {
            data.SetInputValid(data.CurrentBuffer(), false);
            data.SetCurrentBuffer(buffer);
        }
        if (offset / PACKET_BITS >= data.PacketCount(buffer))
        {
            // Last frame of the buffer: release it now. The stream's next packet start (with the
            // skip carried over) is where SeekFrame picks up in the other buffer.
            offset = CarryIntoNextBuffer(data, buffer, offset / PACKET_BITS * PACKET_BITS);
            data.SetInputValid(buffer, false);
            data.SetCurrentBuffer(buffer ^ 1);
        }
        data.SetReadOffset(offset);

        DecodeFrame(h, index, frameBits, length);
        return true;
    }

    // CRASH_XMA_TRACE: logs each newly submitted input buffer with its first packet headers.
    void TraceInputBuffers(HostContext& h, uint32_t index, const ContextData& data)
    {
        for (uint32_t b = 0; b < 2; b++)
        {
            uint32_t address = data.InputValid(b) ? data.InputBuffer(b) : 0;
            if (!address || address == h.tracedBuffers[b])
            {
                h.tracedBuffers[b] = address;
                continue;
            }
            h.tracedBuffers[b] = address;
            const uint8_t* bytes = PhysicalToHost(address);
            char headers[256] = {};
            size_t used = 0;
            for (uint32_t p = 0; p < std::min<uint32_t>(data.PacketCount(b), 6); p++)
            {
                uint32_t header = ReadBE32(bytes + p * PACKET_BYTES);
                used += snprintf(headers + used, sizeof(headers) - used, " [%u f%u o%u s%u]", p, HeaderFrameCount(header),
                                 HeaderFrameOffset(header), HeaderSkip(header));
            }
            Log("[xma] trace context %u: in%u 0x%08X x%u (current %u, read offset 0x%X, %u ch %u Hz):%s", index, b,
                address, data.PacketCount(b), data.CurrentBuffer(), data.ReadOffset(), data.Channels(),
                data.SampleRate(), headers);
        }
    }

    void ProcessContext(HostContext& h, uint32_t index)
    {
        ContextData data;
        LoadContext(h.guest, data);
        ContextData original = data;

        uint32_t blockCount = data.OutputBlockCount();
        if (!data.OutputValid() || blockCount == 0)
            return;

        EnsureDecoder(h, index, data.Channels(), data.SampleRate());
        if (g_trace)
            TraceInputBuffers(h, index, data);
        // Subtitles: the sound is recognized from the packets of each newly submitted buffer.
        for (uint32_t b : { data.CurrentBuffer(), data.CurrentBuffer() ^ 1 })
        {
            if (data.InputValid(b) && !h.inputValid[b])
                subtitles::OnInputBuffer(index, PhysicalToHost(data.InputBuffer(b)), data.PacketCount(b), data.SampleRate(), h.frames);
        }

        // The title rewrote the write offset (stream reset, sub_823EDAC0): drop what's left of the old stream.
        if (data.OutputWriteOffset() != h.lastWriteOffset)
            ResetStream(h);

        uint8_t* output = PhysicalToHost(data.OutputBuffer());
        uint32_t write = data.OutputWriteOffset() % blockCount;
        uint32_t read = data.OutputReadOffset() % blockCount;

        for (uint32_t frames = 0;;)
        {
            while (h.pcm.size() - h.pcmRead >= OUTPUT_BLOCK_SAMPLES && data.OutputValid())
            {
                auto* block = reinterpret_cast<uint16_t*>(output + write * OUTPUT_BLOCK_BYTES);
                for (uint32_t i = 0; i < OUTPUT_BLOCK_SAMPLES; i++)
                    block[i] = __builtin_bswap16(uint16_t(h.pcm[h.pcmRead + i]));
                h.pcmRead += OUTPUT_BLOCK_SAMPLES;
                write = (write + 1) % blockCount;
                data.SetOutputWriteOffset(write);
                if (write == read)
                    data.SetOutputValid(false); // full
                g_blocksWritten++;
            }
            if (!data.OutputValid() || frames++ >= MAX_FRAMES_PER_PASS || !DecodeNextFrame(h, index, data))
                break;
        }

        if (h.pcmRead == h.pcm.size())
        {
            h.pcm.clear();
            h.pcmRead = 0;
        }
        else if (h.pcmRead > 64 * 1024)
        {
            h.pcm.erase(h.pcm.begin(), h.pcm.begin() + h.pcmRead);
            h.pcmRead = 0;
        }
        h.lastWriteOffset = data.OutputWriteOffset();
        h.inputValid[0] = data.InputValid(0);
        h.inputValid[1] = data.InputValid(1);
        subtitles::OnFrames(index, h.frames);

        for (uint32_t i = 0; i < CONTEXT_DWORDS; i++)
        {
            if (data.d[i] != original.d[i])
                StoreBE32(h.guest + i * 4, data.d[i]);
        }
    }

    void ClearContext(HostContext& h)
    {
        // As Xenia's XmaContext::Clear: no valid buffers, output offsets back to 0.
        ContextData data;
        LoadContext(h.guest, data);
        data.SetInputValid(0, false);
        data.SetInputValid(1, false);
        data.SetOutputValid(false);
        data.SetOutputWriteOffset(0);
        SetBits(data.d[9], 0, 5, 0);
        for (uint32_t i = 0; i < CONTEXT_DWORDS; i++)
            StoreBE32(h.guest + i * 4, data.d[i]);
        h.inputValid[0] = h.inputValid[1] = false;
        ResetStream(h);
    }

    void DecoderThread()
    {
        auto lastStats = std::chrono::steady_clock::now();
        uint64_t lastFrames = 0;
        while (true)
        {
            {
                std::unique_lock lock(g_workMutex);
                g_workCondition.wait_for(lock, std::chrono::milliseconds(2), [] { return g_workPending; });
                g_workPending = false;
            }

            for (uint32_t i = 0; i < CONTEXT_COUNT; i++)
            {
                HostContext& h = g_contexts[i];
                if (!h.enabled.load(std::memory_order_acquire))
                    continue;
                std::lock_guard lock(h.mutex);
                if (h.enabled && h.allocated)
                    ProcessContext(h, i);
            }

            auto now = std::chrono::steady_clock::now();
            if (now - lastStats >= std::chrono::seconds(5))
            {
                uint64_t frames = g_framesDecoded;
                if (frames != lastFrames)
                {
                    Log("[xma] %llu kicks, %llu frames decoded (%llu in the last 5 s), %llu output blocks, %llu decoder errors, "
                        "%llu frames padded",
                        (unsigned long long)g_kicks.load(), (unsigned long long)frames,
                        (unsigned long long)(frames - lastFrames), (unsigned long long)g_blocksWritten.load(),
                        (unsigned long long)g_decodeErrors.load(), (unsigned long long)g_paddedFrames.load());
                }
                lastFrames = frames;
                lastStats = now;
            }
        }
    }

    void OnRegisterWrite(uint32_t reg, uint32_t value)
    {
        uint32_t group = 0;
        if (reg >= REG_CONTEXT_KICK && reg < REG_CONTEXT_KICK + REG_GROUPS)
        {
            group = reg - REG_CONTEXT_KICK;
            for (uint32_t bit = 0; bit < 32; bit++)
            {
                if ((value >> bit) & 1)
                {
                    g_contexts[group * 32 + bit].enabled.store(true, std::memory_order_release);
                    g_kicks++;
                }
            }
            {
                std::lock_guard lock(g_workMutex);
                g_workPending = true;
            }
            g_workCondition.notify_one();
        }
        else if (reg >= REG_CONTEXT_LOCK && reg < REG_CONTEXT_LOCK + REG_GROUPS)
        {
            group = reg - REG_CONTEXT_LOCK;
            for (uint32_t bit = 0; bit < 32; bit++)
            {
                if ((value >> bit) & 1)
                {
                    // Waits for a decode pass in progress, so the title sees a stable context.
                    HostContext& h = g_contexts[group * 32 + bit];
                    std::lock_guard lock(h.mutex);
                    h.enabled = false;
                    g_locks++;
                }
            }
        }
        else if (reg >= REG_CONTEXT_CLEAR && reg < REG_CONTEXT_CLEAR + REG_GROUPS)
        {
            group = reg - REG_CONTEXT_CLEAR;
            for (uint32_t bit = 0; bit < 32; bit++)
            {
                if ((value >> bit) & 1)
                {
                    HostContext& h = g_contexts[group * 32 + bit];
                    std::lock_guard lock(h.mutex);
                    if (h.allocated)
                        ClearContext(h);
                    g_clears++;
                }
            }
        }
        else if (g_loggedRegisterWrites.fetch_add(1) < LOGGED_REGISTER_WRITES)
        {
            Log("[xma] guest wrote register 0x%03X = 0x%08X", reg, value);
        }
    }

    // The recompiled stw/stwbrx to the register page compile to a 32-bit `mov [mem], r32` or
    // `mov [mem], imm32`. Decoding it gives the value without ever making the page writable, so
    // stores from other threads can't slip through. Anything else falls back to single-stepping.
    bool DecodeStore(const plat::FaultInfo& fault, uint32_t& value, uint32_t& length)
    {
#if !defined(__x86_64__) && !defined(_M_X64)
        return false; // ARM64 hosts need their own store decoder (linux_port.md)
#endif
        const auto* code = reinterpret_cast<const uint8_t*>(fault.InstructionPointer());
        uint32_t i = 0;
        uint8_t rex = 0;
        if ((code[i] & 0xF0) == 0x40)
            rex = code[i++];
        if (rex & 0x08)
            return false; // 64-bit store
        uint8_t opcode = code[i++];
        if (opcode != 0x89 && opcode != 0xC7)
            return false;
        uint8_t modrm = code[i++];
        uint8_t mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
        if (mod == 3)
            return false;
        if (rm == 4)
        {
            uint8_t sib = code[i++];
            if (mod == 0 && (sib & 7) == 5)
                i += 4;
        }
        else if (mod == 0 && rm == 5)
        {
            i += 4;
        }
        if (mod == 1)
            i += 1;
        else if (mod == 2)
            i += 4;

        if (opcode == 0x89)
        {
            uint32_t index = reg | ((rex & 0x04) << 1);
            value = uint32_t(fault.Register(index));
        }
        else
        {
            if (reg != 0)
                return false;
            memcpy(&value, code + i, 4);
            i += 4;
        }
        length = i;
        return true;
    }

    bool XmaMmioHandler(plat::FaultInfo& fault)
    {
        if (fault.kind == plat::FaultKind::AccessViolation && fault.write)
        {
            if (!g_memory.IsInGuestRange(fault.address))
                return false;
            uint32_t guest = g_memory.MapVirtual(fault.address);
            if (guest < TRAPPED_PAGE || guest >= TRAPPED_PAGE + 0x1000)
                return false;

            uint32_t value, length;
            if (DecodeStore(fault, value, length))
            {
                fault.SetInstructionPointer(fault.InstructionPointer() + length);
                g_emulatedStores++;
                OnRegisterWrite((guest - XMA_MMIO_BEGIN) / 4, value);
                return true;
            }

            t_pendingWrite = guest;
            plat::ProtectMemory(g_memory.Translate(TRAPPED_PAGE), 0x1000, plat::PageAccess::ReadWrite);
            fault.RequestSingleStep(); // single-step the store
            return true;
        }

        if (fault.kind == plat::FaultKind::SingleStep && t_pendingWrite)
        {
            uint32_t guest = t_pendingWrite & ~3u;
            t_pendingWrite = 0;
            plat::ProtectMemory(g_memory.Translate(TRAPPED_PAGE), 0x1000, plat::PageAccess::ReadOnly);
            g_steppedStores++;
            OnRegisterWrite((guest - XMA_MMIO_BEGIN) / 4, *reinterpret_cast<uint32_t*>(g_memory.Translate(guest)));
            return true;
        }

        return false;
    }

    void LogStats()
    {
        uint32_t allocated = 0, enabled = 0;
        for (const HostContext& h : g_contexts)
        {
            allocated += h.allocated;
            enabled += h.allocated && h.enabled;
        }
        Log("[xma] %u context(s) allocated, %u enabled; %llu kicks, %llu locks, %llu clears; %llu frames decoded, "
            "%llu output blocks, %llu decoder errors, %llu frames padded; register stores: %u emulated, %u single-stepped",
            allocated, enabled, (unsigned long long)g_kicks.load(), (unsigned long long)g_locks.load(),
            (unsigned long long)g_clears.load(), (unsigned long long)g_framesDecoded.load(),
            (unsigned long long)g_blocksWritten.load(), (unsigned long long)g_decodeErrors.load(),
            (unsigned long long)g_paddedFrames.load(),
            g_emulatedStores.load(), g_steppedStores.load());

        // State of each live context (read without locking: diagnostics only).
        for (uint32_t i = 0; i < CONTEXT_COUNT; i++)
        {
            const HostContext& h = g_contexts[i];
            if (!h.allocated)
                continue;
            ContextData d;
            LoadContext(h.guest, d);
            Log("[xma]   context %u%s: in0 %s 0x%08X x%u, in1 %s 0x%08X x%u, current %u, read offset 0x%X (packet %u bit %u); "
                "out %s 0x%08X, %u blocks, write %u, read %u; %u ch %u Hz, loop %u [0x%X, 0x%X]; %llu frames, %zu samples pending",
                i, h.enabled ? " (enabled)" : "", d.InputValid(0) ? "valid" : "-", d.InputBuffer(0), d.PacketCount(0),
                d.InputValid(1) ? "valid" : "-", d.InputBuffer(1), d.PacketCount(1), d.CurrentBuffer(), d.ReadOffset(),
                d.ReadOffset() / PACKET_BITS, d.ReadOffset() % PACKET_BITS, d.OutputValid() ? "valid" : "full/-",
                d.OutputBuffer(), d.OutputBlockCount(), d.OutputWriteOffset(), d.OutputReadOffset(), d.Channels(),
                d.SampleRate(), d.LoopCount(), d.LoopStart(), d.LoopEnd(), (unsigned long long)h.frames,
                h.pcm.size() - h.pcmRead);
        }
    }
}

namespace xma
{
    void Init()
    {
        if (!g_memory.Commit(XMA_MMIO_BEGIN, XMA_MMIO_END - XMA_MMIO_BEGIN))
            Fatal("could not commit the XMA register window");

        g_contextArray = AllocatePhysicalMemory(CONTEXT_COUNT * CONTEXT_SIZE, 0x1000, true);
        if (!g_contextArray)
            Fatal("could not allocate the XMA context array");
        for (uint32_t i = 0; i < CONTEXT_COUNT; i++)
            g_contexts[i].guest = g_contextArray + i * CONTEXT_SIZE;

        // Little-endian register: the title reads it with lwbrx.
        *reinterpret_cast<uint32_t*>(g_memory.Translate(XMA_MMIO_BEGIN + REG_CONTEXT_ARRAY_ADDRESS * 4)) =
            g_contextArray & (PHYSICAL_SIZE - 1);

        plat::ProtectMemory(g_memory.Translate(TRAPPED_PAGE), 0x1000, plat::PageAccess::ReadOnly);
        plat::AddFaultHandler(XmaMmioHandler);

#if CRASH_HAVE_FFMPEG
        g_ffmpeg = ffmpeg::Load();
#endif
        if (const char* dump = getenv("CRASH_XMA_DUMP"); dump && *dump)
        {
            g_dumpDirectory = std::filesystem::path(dump);
            std::error_code error;
            std::filesystem::create_directories(g_dumpDirectory, error);
            Log("[xma] dumping decoded streams to %s", g_dumpDirectory.string().c_str());
        }

        g_trace = getenv("CRASH_XMA_TRACE") != nullptr;

        std::thread(DecoderThread).detach();
        AddDiagnosticReporter(LogStats);

        bool decoder = false;
#if CRASH_HAVE_FFMPEG
        decoder = g_ffmpeg != nullptr;
#endif
        Log("[xma] %u contexts at 0x%08X (physical 0x%08X), registers at 0x%08X, decoder: %s", CONTEXT_COUNT,
            g_contextArray, g_contextArray & (PHYSICAL_SIZE - 1), XMA_MMIO_BEGIN,
            decoder ? "FFmpeg WMA Pro" : "none (silence)");
    }

    uint32_t AllocateContext()
    {
        std::lock_guard allocation(g_allocationMutex);
        for (uint32_t i = 0; g_contextArray && i < CONTEXT_COUNT; i++)
        {
            HostContext& h = g_contexts[i];
            std::lock_guard lock(h.mutex);
            if (h.allocated)
                continue;
            h.allocated = true;
            h.enabled = false;
            memset(g_memory.Translate(h.guest), 0, CONTEXT_SIZE);
            h.inputValid[0] = h.inputValid[1] = false;
            ResetStream(h);
            return h.guest;
        }
        return 0;
    }

    void ReleaseContext(uint32_t context)
    {
        uint32_t index = (context - g_contextArray) / CONTEXT_SIZE;
        if (!g_contextArray || context < g_contextArray || index >= CONTEXT_COUNT)
            return;
        HostContext& h = g_contexts[index];
        std::lock_guard lock(h.mutex);
        h.enabled = false;
        h.allocated = false;
        ResetStream(h);
        memset(g_memory.Translate(h.guest), 0, CONTEXT_SIZE);
    }
}
