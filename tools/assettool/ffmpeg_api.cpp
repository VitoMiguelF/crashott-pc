#include "ffmpeg_api.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifndef ASSETTOOL_ROOT_DIR
#define ASSETTOOL_ROOT_DIR "."
#endif

namespace
{
    std::filesystem::path ExeDirectory()
    {
        wchar_t path[MAX_PATH];
        DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(std::wstring(path, length)).parent_path();
    }

    bool LoadFrom(const std::filesystem::path& directory, HMODULE& avutil, HMODULE& avcodec)
    {
        auto avutilPath = directory / ("avutil-" + std::to_string(LIBAVUTIL_VERSION_MAJOR) + ".dll");
        auto avcodecPath = directory / ("avcodec-" + std::to_string(LIBAVCODEC_VERSION_MAJOR) + ".dll");
        if (!std::filesystem::exists(avutilPath) || !std::filesystem::exists(avcodecPath))
            return false;
        avutil = LoadLibraryExW(avutilPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        avcodec = avutil ? LoadLibraryExW(avcodecPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
        return avcodec != nullptr;
    }

    ff::Api g_api;
    bool g_loaded;
}

namespace ff
{
    const Api& Get()
    {
        if (g_loaded)
            return g_api;
        HMODULE avutil = nullptr, avcodec = nullptr;
        if (!LoadFrom(ExeDirectory(), avutil, avcodec) &&
            !LoadFrom(std::filesystem::path(ASSETTOOL_ROOT_DIR) / "thirdparty" / "ffmpeg" / "bin", avutil, avcodec))
        {
            fprintf(stderr, "error: avcodec-%d.dll not found next to the exe or in thirdparty\\ffmpeg\\bin\n",
                    LIBAVCODEC_VERSION_MAJOR);
            exit(2);
        }
        bool ok = true;
#define X(name)                                                                              \
        g_api.name = reinterpret_cast<decltype(&::name)>(GetProcAddress(avcodec, #name));  \
        if (!g_api.name) { fprintf(stderr, "error: avcodec has no %s\n", #name); ok = false; }
        ASSETTOOL_FFMPEG_AVCODEC(X)
#undef X
#define X(name)                                                                             \
        g_api.name = reinterpret_cast<decltype(&::name)>(GetProcAddress(avutil, #name));   \
        if (!g_api.name) { fprintf(stderr, "error: avutil has no %s\n", #name); ok = false; }
        ASSETTOOL_FFMPEG_AVUTIL(X)
#undef X
        if (!ok)
            exit(2);
        if ((g_api.avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR)
        {
            fprintf(stderr, "error: avcodec DLL major version doesn't match the headers (%d)\n", LIBAVCODEC_VERSION_MAJOR);
            exit(2);
        }
        g_api.av_log_set_level(AV_LOG_ERROR);
        g_loaded = true;
        return g_api;
    }
}
