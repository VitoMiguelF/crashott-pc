// .bik -> .mp4 (H.264 via libopenh264, AAC) plus one .wav per Bink audio track.
//
// Bink 1 container (as libavformat/bink.c reads it; the bundled FFmpeg has no libavformat):
//   0x00 "BIKi", u32 file size - 8, u32 frames, u32 largest frame, u32 frames, u32 width,
//   u32 height, u32 fps numerator, u32 fps denominator, u32 video flags (the decoder's 4 bytes
//   of extradata), u32 audio tracks; per track u32 max decoded size; per track u16 sample rate,
//   u16 flags (0x1000 DCT, 0x2000 stereo); per track u32 id; then frames+1 u32 frame offsets
//   (bit 0 = keyframe). Each frame: per audio track u32 size + packet, then the video packet.

#include "assettool.h"
#include "ffmpeg_api.h"
#include "mp4_writer.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <stdexcept>

namespace
{
    uint32_t LE32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
    uint16_t LE16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

    struct AudioTrack
    {
        uint32_t sampleRate = 0, channels = 0;
        bool dct = false;
        AVCodecContext* decoder = nullptr;
        std::vector<std::vector<float>> pcm; // per channel
    };

    std::string Error(int code)
    {
        char text[256];
        ff::Get().av_strerror(code, text, sizeof(text));
        return text;
    }

    void Check(int result, const char* what)
    {
        if (result < 0)
            throw std::runtime_error(std::string(what) + ": " + Error(result));
    }

    void DecodeAudio(AudioTrack& track, AVPacket* packet, AVFrame* frame)
    {
        const ff::Api& f = ff::Get();
        f.avcodec_send_packet(track.decoder, packet);
        while (f.avcodec_receive_frame(track.decoder, frame) >= 0)
        {
            int n = frame->ch_layout.nb_channels;
            if (frame->format != AV_SAMPLE_FMT_FLTP)
                throw std::runtime_error("unexpected Bink audio sample format");
            for (int c = 0; c < int(track.channels); c++)
            {
                const float* src = reinterpret_cast<const float*>(frame->extended_data[std::min(c, n - 1)]);
                track.pcm[c].insert(track.pcm[c].end(), src, src + frame->nb_samples);
            }
        }
    }
}

int ConvertBink(const std::filesystem::path& input, const std::filesystem::path& output, int bitrateKbps)
{
    try
    {
        const ff::Api& f = ff::Get();
        std::vector<uint8_t> file = ReadFile(input);
        if (file.size() < 0x2C || memcmp(file.data(), "BIK", 3) != 0)
            throw std::runtime_error("not a Bink 1 file");
        uint32_t tag = LE32(&file[0]);
        uint32_t frames = LE32(&file[8]);
        uint32_t width = LE32(&file[0x14]), height = LE32(&file[0x18]);
        uint32_t fpsNum = LE32(&file[0x1C]), fpsDen = LE32(&file[0x20]);
        uint32_t videoFlags = LE32(&file[0x24]);
        uint32_t trackCount = LE32(&file[0x28]);
        size_t pos = 0x2C + 4 * trackCount;
        std::vector<AudioTrack> tracks(trackCount);
        for (auto& t : tracks)
        {
            t.sampleRate = LE16(&file[pos]);
            uint16_t flags = LE16(&file[pos + 2]);
            t.dct = flags & 0x1000;
            t.channels = (flags & 0x2000) ? 2 : 1;
            t.pcm.resize(t.channels);
            pos += 4;
        }
        pos += 4 * trackCount;
        std::vector<uint32_t> offsets(frames + 1);
        for (uint32_t i = 0; i <= frames; i++)
            offsets[i] = LE32(&file[pos + 4 * i]);
        offsets[frames] = uint32_t(file.size()) | (offsets[frames] & 1);
        printf("%ux%u, %u frames at %u/%u fps, video flags 0x%X, %u audio track(s):", width, height, frames, fpsNum,
               fpsDen, videoFlags, trackCount);
        for (auto& t : tracks)
            printf(" [%u Hz %u ch %s]", t.sampleRate, t.channels, t.dct ? "DCT" : "RDFT");
        printf("\n");

        AVPacket* packet = f.av_packet_alloc();
        AVFrame* frame = f.av_frame_alloc();

        // Audio first (all of it), so the MP4 can interleave it with the video.
        for (auto& t : tracks)
        {
            const AVCodec* codec = f.avcodec_find_decoder(t.dct ? AV_CODEC_ID_BINKAUDIO_DCT : AV_CODEC_ID_BINKAUDIO_RDFT);
            t.decoder = f.avcodec_alloc_context3(codec);
            t.decoder->sample_rate = int(t.sampleRate);
            f.av_channel_layout_default(&t.decoder->ch_layout, int(t.channels));
            t.decoder->extradata = static_cast<uint8_t*>(f.av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE));
            memcpy(t.decoder->extradata, &tag, 4);
            t.decoder->extradata_size = 4;
            Check(f.avcodec_open2(t.decoder, codec, nullptr), "Bink audio decoder");
        }
        std::vector<uint8_t> padded;
        auto setPacket = [&](const uint8_t* data, size_t size) {
            padded.assign(data, data + size);
            padded.resize(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
            packet->data = padded.data();
            packet->size = int(size);
        };
        for (uint32_t i = 0; i < frames; i++)
        {
            size_t at = offsets[i] & ~1u;
            for (auto& t : tracks)
            {
                uint32_t size = LE32(&file[at]);
                at += 4;
                if (size >= 4)
                {
                    setPacket(&file[at], size);
                    DecodeAudio(t, packet, frame);
                }
                at += size;
            }
        }
        for (auto& t : tracks)
        {
            packet->data = nullptr;
            packet->size = 0;
            DecodeAudio(t, packet, frame);
        }

        // One WAV per track.
        for (size_t n = 0; n < tracks.size(); n++)
        {
            const AudioTrack& t = tracks[n];
            size_t samples = t.pcm[0].size();
            std::vector<int16_t> pcm(samples * t.channels);
            for (size_t s = 0; s < samples; s++)
                for (uint32_t c = 0; c < t.channels; c++)
                    pcm[s * t.channels + c] = int16_t(std::clamp(t.pcm[c][s] * 32768.0f, -32768.0f, 32767.0f));
            std::filesystem::path wav = output;
            wav.replace_extension(".track" + std::to_string(n) + ".wav");
            WriteWav(wav, pcm.data(), samples, t.channels, t.sampleRate);
            float peak = 0;
            for (auto& ch : t.pcm)
                for (float v : ch)
                    peak = std::max(peak, std::abs(v));
            printf("track %zu: %zu samples, peak %.3f -> %s\n", n, samples, peak, wav.string().c_str());
        }

        // MP4 audio: all tracks' channels in order (Bink stores 5.1 as stereo pairs: front, center/LFE, back).
        std::vector<const std::vector<float>*> channels;
        uint32_t sampleRate = tracks.empty() ? 0 : tracks[0].sampleRate;
        for (auto& t : tracks)
            if (t.sampleRate == sampleRate)
                for (auto& c : t.pcm)
                    channels.push_back(&c);
        if (channels.size() > 8 || channels.size() == 3 || channels.size() == 4 || channels.size() == 5 || channels.size() == 7)
            channels.resize(std::min<size_t>(channels.size(), 2));

        Mp4Writer mp4;
        if (!mp4.Open(output))
            throw std::runtime_error("cannot create " + output.string());

        std::deque<std::pair<std::vector<uint8_t>, int64_t>> audioPackets; // data, pts (samples)
        int audioTrack = -1;
        if (!channels.empty())
        {
            const AVCodec* aac = f.avcodec_find_encoder(AV_CODEC_ID_AAC);
            AVCodecContext* enc = f.avcodec_alloc_context3(aac);
            enc->sample_rate = int(sampleRate);
            enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
            f.av_channel_layout_default(&enc->ch_layout, int(channels.size()));
            enc->bit_rate = 96000 * int64_t(channels.size());
            enc->time_base = { 1, int(sampleRate) };
            enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            Check(f.avcodec_open2(enc, aac, nullptr), "AAC encoder");
            Mp4Writer::Track track;
            track.timescale = sampleRate;
            track.channels = uint32_t(channels.size());
            track.sampleRate = sampleRate;
            track.audioSpecificConfig.assign(enc->extradata, enc->extradata + enc->extradata_size);
            audioTrack = mp4.AddTrack(track);

            size_t total = 0;
            for (auto* c : channels)
                total = std::max(total, c->size());
            AVPacket* out = f.av_packet_alloc();
            auto drain = [&] {
                while (f.avcodec_receive_packet(enc, out) >= 0)
                {
                    audioPackets.emplace_back(std::vector<uint8_t>(out->data, out->data + out->size), out->pts);
                    f.av_packet_unref(out);
                }
            };
            for (size_t at = 0; at < total; at += enc->frame_size)
            {
                AVFrame* af = f.av_frame_alloc();
                af->nb_samples = enc->frame_size;
                af->format = AV_SAMPLE_FMT_FLTP;
                f.av_channel_layout_default(&af->ch_layout, int(channels.size()));
                af->sample_rate = int(sampleRate);
                Check(f.av_frame_get_buffer(af, 0), "audio frame");
                for (size_t c = 0; c < channels.size(); c++)
                {
                    float* dst = reinterpret_cast<float*>(af->extended_data[c]);
                    for (int s = 0; s < enc->frame_size; s++)
                        dst[s] = at + s < channels[c]->size() ? (*channels[c])[at + s] : 0.0f;
                }
                af->pts = int64_t(at);
                f.avcodec_send_frame(enc, af);
                f.av_frame_free(&af);
                drain();
            }
            f.avcodec_send_frame(enc, nullptr);
            drain();
            f.av_packet_free(&out);
            f.avcodec_free_context(&enc);
        }

        // Video.
        const AVCodec* bink = f.avcodec_find_decoder(AV_CODEC_ID_BINKVIDEO);
        AVCodecContext* dec = f.avcodec_alloc_context3(bink);
        dec->codec_tag = tag;
        dec->width = int(width);
        dec->height = int(height);
        dec->extradata = static_cast<uint8_t*>(f.av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE));
        memcpy(dec->extradata, &videoFlags, 4);
        dec->extradata_size = 4;
        Check(f.avcodec_open2(dec, bink, nullptr), "Bink video decoder");

        const AVCodec* h264 = f.avcodec_find_encoder_by_name("libopenh264");
        if (!h264)
            throw std::runtime_error("this FFmpeg build has no libopenh264 encoder");
        AVCodecContext* enc = f.avcodec_alloc_context3(h264);
        enc->width = int(width);
        enc->height = int(height);
        enc->pix_fmt = AV_PIX_FMT_YUV420P;
        enc->time_base = { int(fpsDen), int(fpsNum) };
        enc->framerate = { int(fpsNum), int(fpsDen) };
        enc->bit_rate = int64_t(bitrateKbps) * 1000;
        enc->gop_size = 60;
        enc->max_b_frames = 0;
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        f.av_opt_set(enc->priv_data, "rc_mode", "bitrate", 0);
        Check(f.avcodec_open2(enc, h264, nullptr), "H.264 encoder");

        Mp4Writer::Track vtrack;
        vtrack.video = true;
        vtrack.timescale = fpsNum;
        vtrack.width = width;
        vtrack.height = height;
        int videoTrack = mp4.AddTrack(vtrack);
        std::vector<uint8_t> sps, pps;
        if (enc->extradata_size > 0)
            AnnexBToLengthPrefixed(enc->extradata, size_t(enc->extradata_size), &sps, &pps);

        AVPacket* out = f.av_packet_alloc();
        int64_t written = 0;
        auto flushAudioUntil = [&](double seconds) {
            while (audioTrack >= 0 && !audioPackets.empty() &&
                   double(audioPackets.front().second) / sampleRate <= seconds)
            {
                auto& p = audioPackets.front();
                mp4.WriteSample(audioTrack, p.first.data(), p.first.size(), 1024, true);
                audioPackets.pop_front();
            }
        };
        auto drainVideo = [&] {
            while (f.avcodec_receive_packet(enc, out) >= 0)
            {
                std::vector<uint8_t> sample = AnnexBToLengthPrefixed(out->data, size_t(out->size), &sps, &pps);
                flushAudioUntil(double(written) * fpsDen / fpsNum);
                mp4.WriteSample(videoTrack, sample.data(), sample.size(), fpsDen, (out->flags & AV_PKT_FLAG_KEY) != 0);
                written++;
                f.av_packet_unref(out);
            }
        };
        int64_t pts = 0;
        auto drainDecoder = [&] {
            while (f.avcodec_receive_frame(dec, frame) >= 0)
            {
                frame->pts = pts++;
                frame->pict_type = AV_PICTURE_TYPE_NONE;
                Check(f.avcodec_send_frame(enc, frame), "encode");
                drainVideo();
            }
        };
        for (uint32_t i = 0; i < frames; i++)
        {
            size_t at = offsets[i] & ~1u, end = offsets[i + 1] & ~1u;
            for (size_t t = 0; t < tracks.size(); t++)
                at += 4 + LE32(&file[at]);
            setPacket(&file[at], end - at);
            if (offsets[i] & 1)
                packet->flags |= AV_PKT_FLAG_KEY;
            else
                packet->flags &= ~AV_PKT_FLAG_KEY;
            f.avcodec_send_packet(dec, packet);
            drainDecoder();
            if (i % 200 == 0)
            {
                printf("\rframe %u/%u", i, frames);
                fflush(stdout);
            }
        }
        packet->data = nullptr;
        packet->size = 0;
        f.avcodec_send_packet(dec, packet);
        drainDecoder();
        f.avcodec_send_frame(enc, nullptr);
        drainVideo();
        flushAudioUntil(1e30);
        if (sps.empty() || pps.empty())
            throw std::runtime_error("the H.264 encoder produced no SPS/PPS");
        mp4.SetAvcC(videoTrack, BuildAvcC(sps, pps));
        if (!mp4.Close())
            throw std::runtime_error("write error on " + output.string());
        printf("\rok\t%s\tframes=%lld\taudio_channels=%zu\n", output.string().c_str(), (long long)written, channels.size());
        f.av_packet_free(&out);
        f.av_packet_free(&packet);
        f.av_frame_free(&frame);
        f.avcodec_free_context(&enc);
        f.avcodec_free_context(&dec);
        for (auto& t : tracks)
            f.avcodec_free_context(&t.decoder);
        return 0;
    }
    catch (const std::exception& e)
    {
        printf("error\t%s\t%s\n", input.string().c_str(), e.what());
        return 1;
    }
}
