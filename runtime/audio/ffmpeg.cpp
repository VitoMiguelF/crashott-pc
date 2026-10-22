#include "ffmpeg.h"

#if CRASH_HAVE_FFMPEG
#include "../diagnostics.h"

#include <windows.h>

#include <filesystem>
#include <mutex>
#include <string>

namespace
{
    std::filesystem::path ExeDirectory()
    {
        wchar_t path[MAX_PATH];
        DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(std::wstring(path, length)).parent_path();
    }

    // Loads avutil then avcodec from `directory` (their own dependencies, such as swresample,
    // resolve from the same folder thanks to LOAD_WITH_ALTERED_SEARCH_PATH).
    bool LoadFrom(const std::filesystem::path& directory, HMODULE& avutil, HMODULE& avcodec)
    {
        auto avutilPath = directory / ("avutil-" + std::to_string(LIBAVUTIL_VERSION_MAJOR) + ".dll");
        auto avcodecPath = directory / ("avcodec-" + std::to_string(LIBAVCODEC_VERSION_MAJOR) + ".dll");
        if (!std::filesystem::exists(avutilPath) || !std::filesystem::exists(avcodecPath))
            return false;
        avutil = LoadLibraryExW(avutilPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        avcodec = avutil ? LoadLibraryExW(avcodecPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
        if (!avcodec)
        {
            Log("[ffmpeg] could not load %s (error %lu)", avcodecPath.string().c_str(), GetLastError());
            return false;
        }
        Log("[ffmpeg] loaded %s", avcodecPath.string().c_str());
        return true;
    }

    template <typename T>
    bool Resolve(HMODULE module, const char* name, T& function)
    {
        function = reinterpret_cast<T>(GetProcAddress(module, name));
        if (!function)
            Log("[ffmpeg] missing export %s", name);
        return function != nullptr;
    }

    ffmpeg::Api g_api;
    const ffmpeg::Api* g_loaded;
    std::once_flag g_once;

    void LoadOnce()
    {
        HMODULE avutil = nullptr, avcodec = nullptr;
        if (!LoadFrom(ExeDirectory(), avutil, avcodec))
        {
            Log("[ffmpeg] avcodec-%d.dll not found next to the exe: XMA decodes to silence",
                LIBAVCODEC_VERSION_MAJOR);
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
