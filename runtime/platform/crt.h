#pragma once

#include <cstddef>
#include <cstdio>
#include <ctime>
#include <filesystem>

#ifndef _WIN32
#include <strings.h>
#endif

// The Microsoft CRT calls the runtime used (_wfopen, _strnicmp, localtime_s) with their POSIX
// counterparts.
namespace plat
{
    // fopen with a path that may hold any character (_wfopen on Windows).
    inline FILE* OpenCFile(const std::filesystem::path& path, const char* mode)
    {
#ifdef _WIN32
        wchar_t wide[8] = {};
        for (size_t i = 0; mode[i] && i < 7; i++)
            wide[i] = wchar_t(mode[i]);
        return _wfopen(path.c_str(), wide);
#else
        return fopen(path.c_str(), mode);
#endif
    }

    // Case-insensitive (ASCII) comparison of at most `count` characters.
    inline int CompareNoCaseN(const char* a, const char* b, size_t count)
    {
#ifdef _WIN32
        return _strnicmp(a, b, count);
#else
        return strncasecmp(a, b, count);
#endif
    }

    inline bool LocalTime(std::time_t time, std::tm& out)
    {
#ifdef _WIN32
        return localtime_s(&out, &time) == 0;
#else
        return localtime_r(&time, &out) != nullptr;
#endif
    }
}
