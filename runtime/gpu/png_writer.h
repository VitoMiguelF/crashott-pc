#pragma once

#include <cstdint>
#include <filesystem>

namespace gpu
{
    // Writes an 8-bit RGBA image as an uncompressed (stored deflate blocks) PNG.
    // `pitch` is the distance between rows in bytes.
    bool WritePng(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t pitch);
}
