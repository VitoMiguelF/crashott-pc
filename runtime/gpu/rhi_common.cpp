// Parts of the rendering hardware interface shared by the backends (rhi.h).

#include "rhi.h"
#include "../diagnostics.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace gpu::rhi
{
    void FormatBlock(DXGI_FORMAT format, uint32_t& blockWidth, uint32_t& blockHeight, uint32_t& bytesPerBlock)
    {
        blockWidth = blockHeight = 1;
        switch (format)
        {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
            blockWidth = blockHeight = 4;
            bytesPerBlock = 8;
            return;
        case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
            blockWidth = blockHeight = 4;
            bytesPerBlock = 16;
            return;
        case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_UINT:
            bytesPerBlock = 1;
            return;
        case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM:
            bytesPerBlock = 2;
            return;
        case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS:
            bytesPerBlock = 8;
            return;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            bytesPerBlock = 16;
            return;
        default:
            bytesPerBlock = 4;
            return;
        }
    }

    Footprint LevelFootprint(DXGI_FORMAT format, uint32_t width, uint32_t height, uint32_t level)
    {
        uint32_t blockWidth, blockHeight, bytesPerBlock;
        FormatBlock(format, blockWidth, blockHeight, bytesPerBlock);
        uint32_t w = std::max(width >> level, 1u), h = std::max(height >> level, 1u);
        Footprint footprint;
        footprint.rowBytes = (w + blockWidth - 1) / blockWidth * bytesPerBlock;
        footprint.rowPitch = (footprint.rowBytes + 255) & ~255u;
        footprint.rows = (h + blockHeight - 1) / blockHeight;
        footprint.size = uint64_t(footprint.rowPitch) * footprint.rows;
        return footprint;
    }

    uint64_t HashBytes(const void* data, size_t size, uint64_t seed)
    {
        // Four independent multiply-xor lanes; fast enough to hash textures every frame.
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        constexpr uint64_t P = 0x9E3779B97F4A7C15ull;
        uint64_t h[4] = { seed ^ 0x243F6A8885A308D3ull, seed ^ 0x13198A2E03707344ull, seed ^ 0xA4093822299F31D0ull, seed ^ 0x082EFA98EC4E6C89ull };
        size_t i = 0;
        for (; i + 32 <= size; i += 32)
        {
            uint64_t v[4];
            memcpy(v, bytes + i, 32);
            for (int lane = 0; lane < 4; lane++)
            {
                h[lane] ^= v[lane];
                h[lane] *= P;
                h[lane] ^= h[lane] >> 29;
            }
        }
        uint64_t result = h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7) ^ size;
        for (; i < size; i++)
            result = (result ^ bytes[i]) * 0x100000001B3ull;
        result ^= result >> 31;
        result *= P;
        return result ^ (result >> 29);
    }

    namespace
    {
        std::filesystem::path CachePath(const std::filesystem::path& cache, uint64_t hash, const char* extension)
        {
            char fileName[64];
            snprintf(fileName, sizeof(fileName), "%016llx.%s", (unsigned long long)hash, extension);
            return cache / fileName;
        }
    }

    bool ReadCachedShader(const std::filesystem::path& cache, uint64_t hash, const char* extension, std::vector<uint8_t>& data)
    {
        if (cache.empty())
            return false;
        std::ifstream file(CachePath(cache, hash, extension), std::ios::binary);
        if (!file)
            return false;
        data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return !data.empty();
    }

    void WriteCachedShader(const std::filesystem::path& cache, uint64_t hash, const char* extension, const void* data, size_t size)
    {
        if (cache.empty())
            return;
        // Written under a temporary name and renamed: the shader warm-up threads and the renderer can
        // write the same entry at once, and a reader must never see half a file.
        std::filesystem::path path = CachePath(cache, hash, extension);
        std::filesystem::path temporary = path;
        temporary += ".tmp" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        {
            std::ofstream file(temporary, std::ios::binary);
            file.write(static_cast<const char*>(data), std::streamsize(size));
            if (!file)
                return;
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error)
            std::filesystem::remove(temporary, error);
    }

    void DumpFailedShader(const std::filesystem::path& cache, const char* name, const std::string& hlsl)
    {
        if (cache.empty())
            return;
        std::error_code error;
        std::filesystem::path dumpPath = cache.parent_path() / "shaderdump";
        std::filesystem::create_directories(dumpPath, error);
        std::ofstream(dumpPath / (std::string(name) + ".failed.hlsl")) << hlsl;
    }

    namespace
    {
        struct SafeModeName
        {
            const char* name;
            uint32_t bit;
        };
        constexpr SafeModeName SAFE_MODE_NAMES[] = { { "barriers", SAFE_BARRIERS }, { "instances", SAFE_INSTANCES }, { "serial", SAFE_SERIAL },
                                                     { "memory", SAFE_MEMORY },     { "depth", SAFE_DEPTH },         { "present", SAFE_PRESENT },
                                                     { "cache", SAFE_CACHE } };
    }

    uint32_t ParseSafeMode(const std::string& text)
    {
        std::string lower;
        for (char c : text)
            lower += char(tolower(uint8_t(c)));
        if (lower.empty() || lower == "off" || lower == "false" || lower == "0" || lower == "none")
            return 0;
        if (lower == "on" || lower == "true" || lower == "1" || lower == "all")
            return SAFE_ALL;
        uint32_t mask = 0;
        size_t start = 0;
        while (start <= lower.size())
        {
            size_t end = lower.find_first_of(",;+ ", start);
            if (end == std::string::npos)
                end = lower.size();
            std::string word = lower.substr(start, end - start);
            bool known = word.empty();
            for (const SafeModeName& entry : SAFE_MODE_NAMES)
            {
                if (word == entry.name)
                {
                    mask |= entry.bit;
                    known = true;
                }
            }
            if (!known)
                Log("[gpu] gpu_safe_mode: unknown part \"%s\" (barriers, instances, serial, memory, depth, present, cache, all)", word.c_str());
            start = end + 1;
        }
        return mask;
    }

    std::string SafeModeNames(uint32_t mask)
    {
        std::string names;
        for (const SafeModeName& entry : SAFE_MODE_NAMES)
        {
            if (mask & entry.bit)
                names += (names.empty() ? "" : ", ") + std::string(entry.name);
        }
        return names.empty() ? "off" : names;
    }
}
