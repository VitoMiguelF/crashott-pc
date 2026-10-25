#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Image decoding for the fonts and menu art inside the game's packages: WIC on Windows
// (image_win32.cpp), stb_image elsewhere (image_posix.cpp, thirdparty/stb). Both give the same
// pixels for every PNG in default.rcf.
namespace plat
{
    // A PNG (any kind) to straight-alpha RGBA8, rows top-down; false on failure.
    bool DecodePng(const uint8_t* data, size_t size, int& width, int& height, std::vector<uint8_t>& rgba);
}
