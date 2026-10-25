#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Image decoding the OS provides: WIC on Windows (image_win32.cpp). Elsewhere (image_posix.cpp)
// there is no decoder yet; the font and menu art PNGs inside the game's packages need one
// (stb_image or libpng) before the Linux build shows them.
namespace plat
{
    // A PNG (any kind) to straight-alpha RGBA8, rows top-down; false on failure.
    bool DecodePng(const uint8_t* data, size_t size, int& width, int& height, std::vector<uint8_t>& rgba);
}
