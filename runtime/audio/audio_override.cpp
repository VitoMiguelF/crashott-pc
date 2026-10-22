#include "audio_override.h"
#include "ffmpeg.h"
#include "../diagnostics.h"
#include "../fs/vfs.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t MARKER_MAGIC = 0x43525756; // "CRWV"
    constexpr uint32_t MARKER_BITS = 128;
    constexpr uint32_t SAMPLES_PER_FRAME = 512;

    uint32_t PeekBits(const uint8_t* data, uint32_t bit, uint32_t count)
    {
        uint32_t value = 0;
        for (uint32_t i = 0; i < count; i++, bit++)
            value = (value << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1);
        return value;
    }

    struct Entry
    {
        fs::path file;
        std::string name;    // archive/entry, for logs
        std::string mod;
        uint32_t channels = 0; // of the .rsd (all streams)
        uint32_t rate = 0;
    };

    struct Pcm
    {
        uint32_t channels = 0, rate = 0;
        std::vector<float> samples; // interleaved
    };

    struct Sound
    {
        bool failed = false;
        uint32_t channels = 0;
        std::vector<int16_t> pcm; // interleaved, at the .rsd's channel count and rate
    };

    std::once_flag g_once;
    std::mutex g_mutex;
    std::unordered_map<uint32_t, Entry> g_entries;
    std::unordered_map<uint32_t, Sound> g_sounds;
    std::unordered_set<uint32_t> g_unknown;

    void LoadManifests()
    {
        size_t total = 0;
        for (const vfs::ModInfo& mod : vfs::Mods())
        {
            fs::path list = mod.root / "audio" / "overrides.txt";
            std::ifstream in(list);
            if (!in)
                continue;
            std::string line;
            size_t count = 0;
            while (std::getline(in, line))
            {
                if (line.empty() || line[0] == '#')
                    continue;
                if (line.back() == '\r')
                    line.pop_back();
                std::vector<std::string> fields;
                std::stringstream stream(line);
                for (std::string field; std::getline(stream, field, '\t');)
                    fields.push_back(field);
                if (fields.size() < 5)
                    continue;
                uint32_t id = uint32_t(strtoul(fields[0].c_str(), nullptr, 16));
                Entry entry;
                entry.file = mod.root / "audio" / fs::path(std::u8string(fields[1].begin(), fields[1].end()));
                entry.name = fields[2];
                entry.mod = mod.name;
                entry.channels = uint32_t(atoi(fields[3].c_str()));
                entry.rate = uint32_t(atoi(fields[4].c_str()));
                if (!entry.channels || !entry.rate)
                    continue;
                if (g_entries.emplace(id, std::move(entry)).second)
                    count++;
            }
            if (count)
                Log("[audio] mod %s: %zu sound replacement(s) (audio\\overrides.txt)", mod.name.c_str(), count);
            total += count;
        }
        if (total)
            Log("[audio] %zu sound replacement(s) from loose files", total);
    }

    uint32_t LE32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
    uint16_t LE16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

    bool ReadWav(const std::vector<uint8_t>& d, Pcm& out, std::string& error)
    {
        if (d.size() < 12 || memcmp(d.data(), "RIFF", 4) || memcmp(&d[8], "WAVE", 4))
            return error = "not a RIFF/WAVE file", false;
        uint16_t format = 0, channels = 0, bits = 0, align = 0;
        uint32_t rate = 0;
        const uint8_t* data = nullptr;
        size_t dataSize = 0;
        for (size_t pos = 12; pos + 8 <= d.size();)
        {
            uint32_t size = LE32(&d[pos + 4]);
            if (!memcmp(&d[pos], "fmt ", 4) && size >= 16 && pos + 8 + 16 <= d.size())
            {
                format = LE16(&d[pos + 8]);
                channels = LE16(&d[pos + 10]);
                rate = LE32(&d[pos + 12]);
                align = LE16(&d[pos + 20]);
                bits = LE16(&d[pos + 22]);
                if (format == 0xFFFE && size >= 26 && pos + 8 + 26 <= d.size())
                    format = LE16(&d[pos + 8 + 24]); // WAVE_FORMAT_EXTENSIBLE: sub-format
            }
            else if (!memcmp(&d[pos], "data", 4))
            {
                data = &d[pos + 8];
                dataSize = std::min<size_t>(size, d.size() - pos - 8);
                break;
            }
            pos += 8 + size + (size & 1);
        }
        if (!data || !channels || !rate || !align)
            return error = "WAV without fmt/data", false;
        if (!(format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !(format == 3 && bits == 32))
            return error = "unsupported WAV encoding (use PCM 8/16/24/32-bit or 32-bit float)", false;
        out.channels = channels;
        out.rate = rate;
        size_t frames = dataSize / align, bytes = bits / 8;
        out.samples.resize(frames * channels);
        for (size_t i = 0; i < frames * channels; i++)
        {
            const uint8_t* p = data + (i / channels) * align + (i % channels) * bytes;
            float v;
            if (format == 3)
                memcpy(&v, p, 4);
            else if (bits == 8)
                v = (int(p[0]) - 128) / 128.0f;
            else if (bits == 16)
                v = int16_t(LE16(p)) / 32768.0f;
            else if (bits == 24)
                v = float(int32_t((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24)) >> 8) / 8388608.0f;
            else
                v = float(int32_t(LE32(p))) / 2147483648.0f;
            out.samples[i] = v;
        }
        return true;
    }

    // Ogg pages -> packets of the first logical stream.
    bool OggPackets(const std::vector<uint8_t>& d, std::vector<std::vector<uint8_t>>& packets, int64_t& lastGranule)
    {
        std::vector<uint8_t> current;
        uint32_t serial = 0;
        bool first = true;
        lastGranule = -1;
        for (size_t pos = 0; pos + 27 <= d.size();)
        {
            if (memcmp(&d[pos], "OggS", 4) != 0)
                return !packets.empty();
            uint8_t segments = d[pos + 26];
            if (pos + 27 + segments > d.size())
                break;
            int64_t granule;
            memcpy(&granule, &d[pos + 6], 8);
            uint32_t pageSerial = LE32(&d[pos + 14]);
            size_t body = pos + 27 + segments;
            bool mine = first || pageSerial == serial;
            if (first)
            {
                serial = pageSerial;
                first = false;
            }
            for (uint8_t s = 0; s < segments; s++)
            {
                uint8_t lace = d[pos + 27 + s];
                if (mine && body + lace <= d.size())
                {
                    current.insert(current.end(), &d[body], &d[body] + lace);
                    if (lace < 255)
                    {
                        packets.push_back(std::move(current));
                        current.clear();
                    }
                }
                body += lace;
            }
            if (mine && granule >= 0)
                lastGranule = granule;
            pos = body;
        }
        return !packets.empty();
    }

    bool ReadOgg(const std::vector<uint8_t>& d, Pcm& out, std::string& error)
    {
#if CRASH_HAVE_FFMPEG
        const ffmpeg::Api* f = ffmpeg::Load();
        if (!f)
            return error = "FFmpeg not available", false;
        std::vector<std::vector<uint8_t>> packets;
        int64_t lastGranule;
        if (!OggPackets(d, packets, lastGranule) || packets.size() < 3 || packets[0].size() < 16 ||
            memcmp(packets[0].data(), "\x01vorbis", 7) != 0)
            return error = "not an Ogg Vorbis file", false;
        const AVCodec* codec = f->avcodec_find_decoder(AV_CODEC_ID_VORBIS);
        if (!codec)
            return error = "this FFmpeg build has no Vorbis decoder", false;
        AVCodecContext* ctx = f->avcodec_alloc_context3(codec);
        // Xiph-laced headers as extradata.
        std::vector<uint8_t> extra{ 2 };
        for (int i = 0; i < 2; i++)
        {
            size_t n = packets[i].size();
            for (; n >= 255; n -= 255)
                extra.push_back(255);
            extra.push_back(uint8_t(n));
        }
        for (int i = 0; i < 3; i++)
            extra.insert(extra.end(), packets[i].begin(), packets[i].end());
        ctx->extradata = static_cast<uint8_t*>(f->av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        memcpy(ctx->extradata, extra.data(), extra.size());
        ctx->extradata_size = int(extra.size());
        AVPacket* packet = f->av_packet_alloc();
        AVFrame* frame = f->av_frame_alloc();
        bool ok = f->avcodec_open2(ctx, codec, nullptr) >= 0;
        if (!ok)
            error = "could not open the Vorbis decoder";
        std::vector<uint8_t> buffer;
        auto receive = [&] {
            while (f->avcodec_receive_frame(ctx, frame) >= 0)
            {
                int n = frame->ch_layout.nb_channels;
                if (frame->format != AV_SAMPLE_FMT_FLTP || n <= 0)
                    continue;
                out.channels = uint32_t(n);
                out.rate = uint32_t(frame->sample_rate);
                for (int i = 0; i < frame->nb_samples; i++)
                    for (int c = 0; c < n; c++)
                        out.samples.push_back(reinterpret_cast<const float*>(frame->extended_data[c])[i]);
            }
        };
        for (size_t i = 3; ok && i < packets.size(); i++)
        {
            buffer.assign(packets[i].begin(), packets[i].end());
            buffer.resize(buffer.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
            packet->data = buffer.data();
            packet->size = int(packets[i].size());
            f->avcodec_send_packet(ctx, packet);
            receive();
        }
        if (ok)
        {
            packet->data = nullptr;
            packet->size = 0;
            f->avcodec_send_packet(ctx, packet);
            receive();
        }
        f->av_packet_free(&packet);
        f->av_frame_free(&frame);
        f->avcodec_free_context(&ctx);
        if (ok && out.channels && lastGranule >= 0 && size_t(lastGranule) * out.channels < out.samples.size())
            out.samples.resize(size_t(lastGranule) * out.channels);
        if (ok && !out.channels)
            return error = "no audio decoded", false;
        return ok;
#else
        return error = "built without FFmpeg", false;
#endif
    }

    // Channel `o` of `outChannels` from one input frame (layouts: 1 mono, 2 L R, 6 FL FR C LFE BL BR).
    float MapChannel(const float* in, uint32_t inChannels, uint32_t o, uint32_t outChannels)
    {
        if (inChannels == outChannels)
            return in[o];
        if (outChannels == 1)
        {
            float sum = 0;
            for (uint32_t c = 0; c < inChannels; c++)
                sum += in[c];
            return sum / float(inChannels);
        }
        if (inChannels == 1)
            return outChannels >= 6 ? (o == 2 ? in[0] : 0.0f) : (o < 2 ? in[0] : 0.0f);
        if (inChannels >= 6 && outChannels == 2)
            return std::clamp(in[o] + 0.7071f * (in[2] + in[4 + o]), -1.0f, 1.0f);
        return o < inChannels && (inChannels != 2 || o < 2) ? in[o] : 0.0f;
    }

    void Convert(const Pcm& in, uint32_t channels, uint32_t rate, std::vector<int16_t>& out)
    {
        size_t inFrames = in.samples.size() / in.channels;
        size_t outFrames = in.rate == rate ? inFrames : size_t(double(inFrames) * rate / in.rate);
        out.resize(outFrames * channels);
        std::vector<float> a(in.channels), b(in.channels);
        for (size_t i = 0; i < outFrames; i++)
        {
            double position = in.rate == rate ? double(i) : double(i) * in.rate / rate;
            size_t i0 = std::min(size_t(position), inFrames - 1), i1 = std::min(i0 + 1, inFrames - 1);
            float t = float(position - double(i0));
            for (uint32_t c = 0; c < in.channels; c++)
                a[c] = in.samples[i0 * in.channels + c] * (1 - t) + in.samples[i1 * in.channels + c] * t;
            for (uint32_t o = 0; o < channels; o++)
                out[i * channels + o] = int16_t(std::clamp(MapChannel(a.data(), in.channels, o, channels) * 32768.0f, -32768.0f, 32767.0f));
        }
    }

    Sound& Load(uint32_t id, const Entry& entry)
    {
        Sound& sound = g_sounds[id];
        std::vector<uint8_t> data;
        {
            std::ifstream in(entry.file, std::ios::binary);
            if (in)
                data.assign(std::istreambuf_iterator<char>(in), {});
        }
        Pcm pcm;
        std::string error;
        bool ok;
        if (data.empty())
            ok = (error = "file missing or empty", false);
        else if (data.size() >= 4 && !memcmp(data.data(), "OggS", 4))
            ok = ReadOgg(data, pcm, error);
        else
            ok = ReadWav(data, pcm, error);
        if (ok && pcm.samples.empty())
            ok = (error = "no samples", false);
        if (!ok)
        {
            Log("[audio] %s (mod %s): %s: %s; playing silence", entry.name.c_str(), entry.mod.c_str(),
                entry.file.string().c_str(), error.c_str());
            sound.failed = true;
            return sound;
        }
        sound.channels = entry.channels;
        Convert(pcm, entry.channels, entry.rate, sound.pcm);
        Log("[audio] %s <- mod %s: %s (%u ch, %u Hz, %.2f s -> %u ch, %u Hz)", entry.name.c_str(), entry.mod.c_str(),
            entry.file.filename().string().c_str(), pcm.channels, pcm.rate,
            double(pcm.samples.size() / pcm.channels) / pcm.rate, entry.channels, entry.rate);
        return sound;
    }
}

namespace audio_override
{
    bool DecodeMarkerFrame(const uint8_t* frameBits, uint32_t bitLength, uint32_t channels, std::vector<int16_t>& pcm)
    {
        if (bitLength != MARKER_BITS || PeekBits(frameBits, 15, 32) != MARKER_MAGIC)
            return false;
        uint32_t id = PeekBits(frameBits, 47, 32);
        uint32_t stream = PeekBits(frameBits, 79, 8);
        uint32_t frame = PeekBits(frameBits, 87, 24);

        std::call_once(g_once, LoadManifests);
        size_t start = pcm.size();
        pcm.resize(start + size_t(SAMPLES_PER_FRAME) * channels, 0);

        std::lock_guard lock(g_mutex);
        auto entry = g_entries.find(id);
        if (entry == g_entries.end())
        {
            if (g_unknown.insert(id).second)
                Log("[audio] marker frame for unknown sound %08X (no mod lists it in audio\\overrides.txt): silence", id);
            return true;
        }
        auto found = g_sounds.find(id);
        Sound& sound = found != g_sounds.end() ? found->second : Load(id, entry->second);
        if (sound.failed || !sound.channels)
            return true;
        // This context decodes channels [stream * channels, +channels) of the sound.
        size_t frames = sound.pcm.size() / sound.channels;
        size_t first = size_t(frame) * SAMPLES_PER_FRAME;
        for (uint32_t i = 0; i < SAMPLES_PER_FRAME && first + i < frames; i++)
            for (uint32_t c = 0; c < channels; c++)
            {
                uint32_t source = stream * channels + c;
                if (source < sound.channels)
                    pcm[start + size_t(i) * channels + c] = sound.pcm[(first + i) * sound.channels + source];
            }
        return true;
    }
}
