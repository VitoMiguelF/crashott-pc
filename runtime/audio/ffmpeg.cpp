#include "ffmpeg.h"

#if CRASH_HAVE_FFMPEG
#include "../diagnostics.h"

#include "../platform/platform.h"

#include <filesystem>
#include <mutex>
#include <string>

namespace
{
    // FFmpeg's file names: avcodec-62.dll on Windows, libavcodec.so.62 elsewhere.
    std::string LibraryName(const char* name, int major)
    {
#ifdef _WIN32
        return std::string(name) + "-" + std::to_string(major) + ".dll";
#else
        return "lib" + std::string(name) + ".so." + std::to_string(major);
#endif
    }

    // Loads avutil then avcodec from `directory` (their own dependencies, such as swresample,
    // resolve from the same folder: LOAD_WITH_ALTERED_SEARCH_PATH, or the libraries' $ORIGIN rpath).
    bool LoadFrom(const std::filesystem::path& directory, void*& avutil, void*& avcodec)
    {
        auto avutilPath = directory / LibraryName("avutil", LIBAVUTIL_VERSION_MAJOR);
        auto avcodecPath = directory / LibraryName("avcodec", LIBAVCODEC_VERSION_MAJOR);
        if (!std::filesystem::exists(avutilPath) || !std::filesystem::exists(avcodecPath))
            return false;
        avutil = plat::OpenLibrary(avutilPath);
        avcodec = avutil ? plat::OpenLibrary(avcodecPath) : nullptr;
        if (!avcodec)
        {
            Log("[ffmpeg] could not load %s (%s)", avcodecPath.string().c_str(), plat::LastErrorText().c_str());
            return false;
        }
        Log("[ffmpeg] loaded %s", avcodecPath.string().c_str());
        return true;
    }

    template <typename T>
    bool Resolve(void* module, const char* name, T& function)
    {
        function = reinterpret_cast<T>(plat::FindSymbol(module, name));
        if (!function)
            Log("[ffmpeg] missing export %s", name);
        return function != nullptr;
    }

    ffmpeg::Api g_api;
    const ffmpeg::Api* g_loaded;
    std::once_flag g_once;

    void LoadOnce()
    {
        void* avutil = nullptr;
        void* avcodec = nullptr;
        if (!LoadFrom(plat::ExecutablePath().parent_path(), avutil, avcodec))
        {
            Log("[ffmpeg] %s not found next to the exe: XMA decodes to silence",
                LibraryName("avcodec", LIBAVCODEC_VERSION_MAJOR).c_str());
            return;
        }

        ffmpeg::Api& a = g_api;
        bool ok = Resolve(avcodec, "avcodec_version", a.avcodec_version) &&
                  Resolve(avcodec, "avcodec_find_decoder", a.avcodec_find_decoder) &&
                  Resolve(avcodec, "avcodec_alloc_context3", a.avcodec_alloc_context3) &&
                  Resolve(avcodec, "avcodec_open2", a.avcodec_open2) &&
                  Resolve(avcodec, "avcodec_send_packet", a.avcodec_send_packet) &&
                  Resolve(avcodec, "avcodec_receive_frame", a.avcodec_receive_frame) &&
                  Resolve(avcodec, "avcodec_flush_buffers", a.avcodec_flush_buffers) &&
                  Resolve(avcodec, "avcodec_free_context", a.avcodec_free_context) &&
                  Resolve(avcodec, "av_packet_alloc", a.av_packet_alloc) &&
                  Resolve(avcodec, "av_packet_free", a.av_packet_free) &&
                  Resolve(avutil, "av_frame_alloc", a.av_frame_alloc) &&
                  Resolve(avutil, "av_frame_free", a.av_frame_free) &&
                  Resolve(avutil, "av_mallocz", a.av_mallocz) &&
                  Resolve(avutil, "av_channel_layout_from_mask", a.av_channel_layout_from_mask) &&
                  Resolve(avutil, "av_log_set_level", a.av_log_set_level);
        if (!ok)
            return;

        // The structures are used through the headers, so the DLL must be the same major version.
        unsigned version = a.avcodec_version();
        if ((version >> 16) != LIBAVCODEC_VERSION_MAJOR)
        {
            Log("[ffmpeg] avcodec version %u.%u.%u doesn't match the headers (%d.x): not used",
                version >> 16, (version >> 8) & 0xFF, version & 0xFF, LIBAVCODEC_VERSION_MAJOR);
            return;
        }
        if (!a.avcodec_find_decoder(AV_CODEC_ID_WMAPRO))
        {
            Log("[ffmpeg] this avcodec build has no WMA Pro decoder: not used");
            return;
        }

        a.av_log_set_level(AV_LOG_QUIET);
        g_loaded = &g_api;
    }
}

namespace ffmpeg
{
    const Api* Load()
    {
        std::call_once(g_once, LoadOnce);
        return g_loaded;
    }
}
#endif
