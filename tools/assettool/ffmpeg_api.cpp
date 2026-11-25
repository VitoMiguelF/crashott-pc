#include "ffmpeg_api.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <libswresample/version_major.h>
#endif

#ifndef ASSETTOOL_ROOT_DIR
#define ASSETTOOL_ROOT_DIR "."
#endif

// The FFmpeg libraries: avcodec-62.dll / avutil-60.dll on Windows (thirdparty/ffmpeg/bin), libavcodec.so.62 /
// libavutil.so.60 on Linux (thirdparty/ffmpeg/lib). Same API either way; only the loader differs.
#ifdef _WIN32
#define ASSETTOOL_FFMPEG_LIBDIR "bin"
#else
#define ASSETTOOL_FFMPEG_LIBDIR "lib"
#endif

namespace
{
#ifdef _WIN32
    using Module = HMODULE;

    std::filesystem::path ExeDirectory()
    {
        wchar_t path[MAX_PATH];
        DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(std::wstring(path, length)).parent_path();
    }

    std::filesystem::path LibraryName(const char* stem, int major)
    {
        return std::string(stem) + "-" + std::to_string(major) + ".dll";
    }

    Module OpenLibrary(const std::filesystem::path& path)
    {
        return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }

    void* Symbol(Module module, const char* name)
    {
        return reinterpret_cast<void*>(GetProcAddress(module, name));
    }
#else
    using Module = void*;

    std::filesystem::path ExeDirectory()
    {
        std::error_code error;
        std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", error);
        return error ? std::filesystem::path() : exe.parent_path();
    }

    std::filesystem::path LibraryName(const char* stem, int major)
    {
        return std::string("lib") + stem + ".so." + std::to_string(major);
    }

    Module OpenLibrary(const std::filesystem::path& path)
    {
        return dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    }

    void* Symbol(Module module, const char* name)
    {
        return dlsym(module, name);
    }
#endif

    bool LoadFrom(const std::filesystem::path& directory, Module& avutil, Module& avcodec)
    {
        auto avutilPath = directory / LibraryName("avutil", LIBAVUTIL_VERSION_MAJOR);
        auto avcodecPath = directory / LibraryName("avcodec", LIBAVCODEC_VERSION_MAJOR);
        if (!std::filesystem::exists(avutilPath) || !std::filesystem::exists(avcodecPath))
            return false;
        avutil = OpenLibrary(avutilPath);
#ifndef _WIN32
        // Linux: libswresample depends on libavutil and libavcodec on both, by SONAME. The loader matches them
        // against the libraries already opened, so they go in in this order, global.
        auto swresamplePath = directory / LibraryName("swresample", LIBSWRESAMPLE_VERSION_MAJOR);
        if (std::filesystem::exists(swresamplePath) && (!avutil || !OpenLibrary(swresamplePath)))
            return false;
#endif
        avcodec = avutil ? OpenLibrary(avcodecPath) : nullptr;
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
        Module avutil = nullptr, avcodec = nullptr;
        if (!LoadFrom(ExeDirectory(), avutil, avcodec) &&
            !LoadFrom(std::filesystem::path(ASSETTOOL_ROOT_DIR) / "thirdparty" / "ffmpeg" / ASSETTOOL_FFMPEG_LIBDIR,
                      avutil, avcodec))
        {
            fprintf(stderr, "error: %s not found next to the exe or in thirdparty/ffmpeg/%s\n",
                    LibraryName("avcodec", LIBAVCODEC_VERSION_MAJOR).string().c_str(), ASSETTOOL_FFMPEG_LIBDIR);
            exit(2);
        }
        bool ok = true;
#define X(name)                                                                              \
        g_api.name = reinterpret_cast<decltype(&::name)>(Symbol(avcodec, #name));          \
        if (!g_api.name) { fprintf(stderr, "error: avcodec has no %s\n", #name); ok = false; }
        ASSETTOOL_FFMPEG_AVCODEC(X)
#undef X
#define X(name)                                                                             \
        g_api.name = reinterpret_cast<decltype(&::name)>(Symbol(avutil, #name));           \
        if (!g_api.name) { fprintf(stderr, "error: avutil has no %s\n", #name); ok = false; }
        ASSETTOOL_FFMPEG_AVUTIL(X)
#undef X
        if (!ok)
            exit(2);
        if ((g_api.avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR)
        {
            fprintf(stderr, "error: avcodec library major version doesn't match the headers (%d)\n", LIBAVCODEC_VERSION_MAJOR);
            exit(2);
        }
        g_api.av_log_set_level(AV_LOG_ERROR);
        g_loaded = true;
        return g_api;
    }
}
