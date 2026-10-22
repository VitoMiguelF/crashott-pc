#pragma once

// FFmpeg loaded at run time from the LGPL DLLs in thirdparty/ffmpeg/bin (same approach as
// runtime/audio/ffmpeg.cpp, with the extra encoder functions the asset tool needs).

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

#define ASSETTOOL_FFMPEG_AVCODEC(X) \
    X(avcodec_version) X(avcodec_find_decoder) X(avcodec_find_encoder) X(avcodec_find_encoder_by_name) \
    X(avcodec_alloc_context3) X(avcodec_open2) X(avcodec_send_packet) X(avcodec_receive_frame) \
    X(avcodec_send_frame) X(avcodec_receive_packet) X(avcodec_flush_buffers) X(avcodec_free_context) \
    X(av_packet_alloc) X(av_packet_free) X(av_packet_unref)

#define ASSETTOOL_FFMPEG_AVUTIL(X) \
    X(av_frame_alloc) X(av_frame_free) X(av_frame_get_buffer) X(av_frame_make_writable) X(av_mallocz) \
    X(av_channel_layout_from_mask) X(av_channel_layout_default) X(av_log_set_level) X(av_opt_set) X(av_opt_set_int) \
    X(av_strerror)

namespace ff
{
    struct Api
    {
#define X(name) decltype(&::name) name;
        ASSETTOOL_FFMPEG_AVCODEC(X)
        ASSETTOOL_FFMPEG_AVUTIL(X)
#undef X
    };

    // Loads the DLLs (next to the exe, then thirdparty\ffmpeg\bin); exits with a message if missing.
    const Api& Get();
}
