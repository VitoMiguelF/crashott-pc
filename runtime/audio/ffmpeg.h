#pragma once

// FFmpeg (libavcodec/libavutil, LGPL), loaded at run time from the shared DLLs of the build in
// thirdparty/ffmpeg (copied next to the exe by CMake). Only the WMA Pro decoder is used: XMA frames
// are WMA Pro frames (see audio/xma.cpp). Without the DLLs, or when the runtime is built without
// the headers, Load() returns nullptr and XMA decodes to silence.

#if CRASH_HAVE_FFMPEG
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
}

namespace ffmpeg
{
    struct Api
    {
        decltype(&::avcodec_version) avcodec_version;
        decltype(&::avcodec_find_decoder) avcodec_find_decoder;
        decltype(&::avcodec_alloc_context3) avcodec_alloc_context3;
        decltype(&::avcodec_open2) avcodec_open2;
        decltype(&::avcodec_send_packet) avcodec_send_packet;
        decltype(&::avcodec_receive_frame) avcodec_receive_frame;
        decltype(&::avcodec_flush_buffers) avcodec_flush_buffers;
        decltype(&::avcodec_free_context) avcodec_free_context;
        decltype(&::av_packet_alloc) av_packet_alloc;
        decltype(&::av_packet_free) av_packet_free;
        decltype(&::av_frame_alloc) av_frame_alloc;
        decltype(&::av_frame_free) av_frame_free;
        decltype(&::av_mallocz) av_mallocz;
        decltype(&::av_channel_layout_from_mask) av_channel_layout_from_mask;
        decltype(&::av_log_set_level) av_log_set_level;
    };

    // Loads the DLLs once (thread-safe); nullptr if they're missing or don't match the headers.
    const Api* Load();
}
#endif
