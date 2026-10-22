#include "mp4_writer.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace
{
    struct Box
    {
        std::vector<uint8_t> data;
        std::vector<size_t> open;

        void U8(uint32_t v) { data.push_back(uint8_t(v)); }
        void U16(uint32_t v) { U8(v >> 8); U8(v); }
        void U24(uint32_t v) { U8(v >> 16); U16(v); }
        void U32(uint32_t v) { U16(v >> 16); U16(v); }
        void U64(uint64_t v) { U32(uint32_t(v >> 32)); U32(uint32_t(v)); }
        void Bytes(const void* p, size_t n) { auto* b = static_cast<const uint8_t*>(p); data.insert(data.end(), b, b + n); }
        void Zero(size_t n) { data.insert(data.end(), n, 0); }
        void Begin(const char* type)
        {
            open.push_back(data.size());
            U32(0);
            Bytes(type, 4);
        }
        void FullBegin(const char* type, uint32_t version, uint32_t flags)
        {
            Begin(type);
            U8(version);
            U24(flags);
        }
        void End()
        {
            size_t start = open.back();
            open.pop_back();
            uint32_t size = uint32_t(data.size() - start);
            data[start] = uint8_t(size >> 24);
            data[start + 1] = uint8_t(size >> 16);
            data[start + 2] = uint8_t(size >> 8);
            data[start + 3] = uint8_t(size);
        }
        void Matrix()
        {
            const uint32_t m[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };
            for (uint32_t v : m)
                U32(v);
        }
        // MPEG-4 descriptor with a 4-byte length.
        void Descriptor(uint8_t tag, const std::vector<uint8_t>& body)
        {
            U8(tag);
            U8(0x80); U8(0x80); U8(0x80); U8(uint8_t(body.size()));
            Bytes(body.data(), body.size());
        }
    };

    uint64_t TrackDuration(const Mp4Writer::Track& t)
    {
        uint64_t d = 0;
        for (uint32_t x : t.durations)
            d += x;
        return d;
    }
}

bool Mp4Writer::Open(const std::filesystem::path& path)
{
    m_file = _wfopen(path.c_str(), L"wb");
    if (!m_file)
        return false;
    Box ftyp;
    ftyp.Begin("ftyp");
    ftyp.Bytes("isom", 4);
    ftyp.U32(0x200);
    ftyp.Bytes("isomiso2avc1mp41", 16);
    ftyp.End();
    fwrite(ftyp.data.data(), 1, ftyp.data.size(), m_file);
    m_mdatStart = ftyp.data.size();
    // mdat with a 64-bit size, patched in Close().
    const uint8_t mdat[16] = { 0, 0, 0, 1, 'm', 'd', 'a', 't' };
    fwrite(mdat, 1, 16, m_file);
    m_position = m_mdatStart + 16;
    return true;
}

int Mp4Writer::AddTrack(const Track& track)
{
    m_tracks.push_back(track);
    return int(m_tracks.size() - 1);
}

void Mp4Writer::WriteSample(int track, const uint8_t* data, size_t size, uint32_t duration, bool sync)
{
    Track& t = m_tracks[track];
    t.offsets.push_back(m_position);
    t.sizes.push_back(uint32_t(size));
    t.durations.push_back(duration);
    if (sync)
        t.syncSamples.push_back(uint32_t(t.sizes.size()));
    fwrite(data, 1, size, m_file);
    m_position += size;
}

bool Mp4Writer::Close()
{
    if (!m_file)
        return false;
    uint64_t mdatSize = m_position - m_mdatStart;
    _fseeki64(m_file, int64_t(m_mdatStart + 8), SEEK_SET);
    uint8_t size[8];
    for (int i = 0; i < 8; i++)
        size[i] = uint8_t(mdatSize >> (56 - 8 * i));
    fwrite(size, 1, 8, m_file);
    _fseeki64(m_file, 0, SEEK_END);

    const uint32_t movieTimescale = 1000;
    uint64_t movieDuration = 0;
    for (auto& t : m_tracks)
        movieDuration = std::max(movieDuration, TrackDuration(t) * movieTimescale / t.timescale);

    Box b;
    b.Begin("moov");
    b.FullBegin("mvhd", 0, 0);
    b.U32(0); b.U32(0); b.U32(movieTimescale); b.U32(uint32_t(movieDuration));
    b.U32(0x00010000); b.U16(0x0100); b.Zero(10); b.Matrix(); b.Zero(24);
    b.U32(uint32_t(m_tracks.size() + 1));
    b.End();

    for (size_t i = 0; i < m_tracks.size(); i++)
    {
        const Track& t = m_tracks[i];
        uint64_t duration = TrackDuration(t);
        b.Begin("trak");
        b.FullBegin("tkhd", 0, 3);
        b.U32(0); b.U32(0); b.U32(uint32_t(i + 1)); b.U32(0);
        b.U32(uint32_t(duration * movieTimescale / t.timescale));
        b.Zero(8); b.U16(0); b.U16(0); b.U16(t.video ? 0 : 0x0100); b.U16(0); b.Matrix();
        b.U32(t.video ? t.width << 16 : 0); b.U32(t.video ? t.height << 16 : 0);
        b.End();

        b.Begin("mdia");
        b.FullBegin("mdhd", 0, 0);
        b.U32(0); b.U32(0); b.U32(t.timescale); b.U32(uint32_t(duration)); b.U16(0x55C4); b.U16(0);
        b.End();
        b.FullBegin("hdlr", 0, 0);
        b.U32(0); b.Bytes(t.video ? "vide" : "soun", 4); b.Zero(12);
        const char* name = t.video ? "VideoHandler" : "SoundHandler";
        b.Bytes(name, strlen(name) + 1);
        b.End();

        b.Begin("minf");
        if (t.video)
        {
            b.FullBegin("vmhd", 0, 1);
            b.Zero(8);
        }
        else
        {
            b.FullBegin("smhd", 0, 0);
            b.Zero(4);
        }
        b.End();
        b.Begin("dinf");
        b.FullBegin("dref", 0, 0);
        b.U32(1);
        b.FullBegin("url ", 0, 1);
        b.End();
        b.End();
        b.End();

        b.Begin("stbl");
        b.FullBegin("stsd", 0, 0);
        b.U32(1);
        if (t.video)
        {
            b.Begin("avc1");
            b.Zero(6); b.U16(1); b.Zero(16);
            b.U16(t.width); b.U16(t.height); b.U32(0x00480000); b.U32(0x00480000); b.U32(0); b.U16(1);
            b.Zero(32); b.U16(0x18); b.U16(0xFFFF);
            b.Begin("avcC");
            b.Bytes(t.avcC.data(), t.avcC.size());
            b.End();
            b.End();
        }
        else
        {
            b.Begin("mp4a");
            b.Zero(6); b.U16(1); b.Zero(8);
            b.U16(t.channels); b.U16(16); b.U16(0); b.U16(0); b.U32(t.sampleRate << 16);
            b.FullBegin("esds", 0, 0);
            Box dsi;
            dsi.Descriptor(5, t.audioSpecificConfig);
            Box dcd;
            dcd.U8(0x40); dcd.U8(0x15); dcd.U24(0); dcd.U32(0); dcd.U32(0);
            dcd.Bytes(dsi.data.data(), dsi.data.size());
            Box es;
            es.U16(0); es.U8(0);
            es.Descriptor(4, dcd.data);
            es.Descriptor(6, { 0x02 });
            b.Descriptor(3, es.data);
            b.End();
            b.End();
        }
        b.End(); // stsd

        b.FullBegin("stts", 0, 0);
        std::vector<std::pair<uint32_t, uint32_t>> runs;
        for (uint32_t d : t.durations)
        {
            if (!runs.empty() && runs.back().second == d)
                runs.back().first++;
            else
                runs.push_back({ 1, d });
        }
        b.U32(uint32_t(runs.size()));
        for (auto& r : runs)
        {
            b.U32(r.first);
            b.U32(r.second);
        }
        b.End();
        if (t.video)
        {
            b.FullBegin("stss", 0, 0);
            b.U32(uint32_t(t.syncSamples.size()));
            for (uint32_t s : t.syncSamples)
                b.U32(s);
            b.End();
        }
        b.FullBegin("stsc", 0, 0);
        b.U32(1); b.U32(1); b.U32(1); b.U32(1);
        b.End();
        b.FullBegin("stsz", 0, 0);
        b.U32(0); b.U32(uint32_t(t.sizes.size()));
        for (uint32_t s : t.sizes)
            b.U32(s);
        b.End();
        b.FullBegin("co64", 0, 0);
        b.U32(uint32_t(t.offsets.size()));
        for (uint64_t o : t.offsets)
            b.U64(o);
        b.End();
        b.End(); // stbl
        b.End(); // minf
        b.End(); // mdia
        b.End(); // trak
    }
    b.End(); // moov
    fwrite(b.data.data(), 1, b.data.size(), m_file);
    bool ok = !ferror(m_file);
    fclose(m_file);
    m_file = nullptr;
    return ok;
}

std::vector<uint8_t> AnnexBToLengthPrefixed(const uint8_t* data, size_t size, std::vector<uint8_t>* sps,
                                            std::vector<uint8_t>* pps)
{
    std::vector<uint8_t> out;
    auto startCode = [&](size_t i) -> size_t {
        if (i + 3 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            return 3;
        if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
            return 4;
        return 0;
    };
    size_t i = 0;
    while (i < size && !startCode(i))
        i++;
    while (i < size)
    {
        size_t begin = i + startCode(i);
        size_t end = begin;
        while (end < size && !startCode(end))
            end++;
        size_t n = end - begin;
        if (n > 0)
        {
            uint8_t type = data[begin] & 0x1F;
            if (type == 7 && sps && sps->empty())
                sps->assign(data + begin, data + end);
            if (type == 8 && pps && pps->empty())
                pps->assign(data + begin, data + end);
            out.push_back(uint8_t(n >> 24));
            out.push_back(uint8_t(n >> 16));
            out.push_back(uint8_t(n >> 8));
            out.push_back(uint8_t(n));
            out.insert(out.end(), data + begin, data + end);
        }
        i = end;
    }
    return out;
}

std::vector<uint8_t> BuildAvcC(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps)
{
    std::vector<uint8_t> c;
    c.push_back(1);
    c.push_back(sps.size() > 1 ? sps[1] : 66);
    c.push_back(sps.size() > 2 ? sps[2] : 0);
    c.push_back(sps.size() > 3 ? sps[3] : 31);
    c.push_back(0xFF); // 4-byte lengths
    c.push_back(0xE1); // one SPS
    c.push_back(uint8_t(sps.size() >> 8));
    c.push_back(uint8_t(sps.size()));
    c.insert(c.end(), sps.begin(), sps.end());
    c.push_back(1);
    c.push_back(uint8_t(pps.size() >> 8));
    c.push_back(uint8_t(pps.size()));
    c.insert(c.end(), pps.begin(), pps.end());
    return c;
}
