#include "hd_textures.h"
#include "png_writer.h"
#include "../diagnostics.h"
#include "../fs/vfs.h"
#include "../settings/settings.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace gpu::hd
{
    namespace
    {
        struct PackFile
        {
            fs::path path;
            std::string mod;
        };

        std::mutex g_mutex;
        bool g_enabled = false, g_dumping = false;
        std::unordered_map<uint64_t, PackFile> g_index;
        fs::path g_dumpDir;
        std::unordered_set<uint64_t> g_dumped;
        std::ofstream g_dumpIndex;
        uint32_t g_dumpCount = 0;

        DXGI_FORMAT Unorm(DXGI_FORMAT format)
        {
            switch (format)
            {
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_UNORM;
            case DXGI_FORMAT_BC2_UNORM_SRGB: return DXGI_FORMAT_BC2_UNORM;
            case DXGI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_UNORM;
            case DXGI_FORMAT_BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_UNORM;
            default: return format;
            }
        }

        DXGI_FORMAT Srgb(DXGI_FORMAT format)
        {
            switch (format)
            {
            case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            case DXGI_FORMAT_BC1_UNORM: return DXGI_FORMAT_BC1_UNORM_SRGB;
            case DXGI_FORMAT_BC2_UNORM: return DXGI_FORMAT_BC2_UNORM_SRGB;
            case DXGI_FORMAT_BC3_UNORM: return DXGI_FORMAT_BC3_UNORM_SRGB;
            case DXGI_FORMAT_BC7_UNORM: return DXGI_FORMAT_BC7_UNORM_SRGB;
            default: return format;
            }
        }

        // Bytes per 4x4 block, 0 for uncompressed RGBA8.
        uint32_t BlockBytes(DXGI_FORMAT format)
        {
            switch (Unorm(format))
            {
            case DXGI_FORMAT_BC1_UNORM: return 8;
            case DXGI_FORMAT_BC2_UNORM:
            case DXGI_FORMAT_BC3_UNORM:
            case DXGI_FORMAT_BC7_UNORM: return 16;
            default: return 0;
            }
        }

        const char* FormatName(DXGI_FORMAT format)
        {
            switch (Unorm(format))
            {
            case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
            case DXGI_FORMAT_BC1_UNORM: return "BC1";
            case DXGI_FORMAT_BC2_UNORM: return "BC2";
            case DXGI_FORMAT_BC3_UNORM: return "BC3";
            case DXGI_FORMAT_BC7_UNORM: return "BC7";
            default: return "?";
            }
        }

        uint32_t LoadLE32(const uint8_t* p)
        {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }

        // BC1-BC3 block -> 4x4 RGBA8 (for the dump PNGs).
        void Rgb565(uint16_t c, uint8_t* out)
        {
            out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
            out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
            out[2] = uint8_t((c & 31) * 255 / 31);
            out[3] = 255;
        }

        void DecodeColorBlock(const uint8_t* block, bool allowPunchThrough, uint8_t out[16][4])
        {
            uint16_t c0 = uint16_t(block[0] | (block[1] << 8)), c1 = uint16_t(block[2] | (block[3] << 8));
            uint8_t palette[4][4];
            Rgb565(c0, palette[0]);
            Rgb565(c1, palette[1]);
            if (c0 > c1 || !allowPunchThrough)
            {
                for (int k = 0; k < 3; k++)
                {
                    palette[2][k] = uint8_t((2 * palette[0][k] + palette[1][k]) / 3);
                    palette[3][k] = uint8_t((palette[0][k] + 2 * palette[1][k]) / 3);
                }
                palette[2][3] = palette[3][3] = 255;
            }
            else
            {
                for (int k = 0; k < 3; k++)
                    palette[2][k] = uint8_t((palette[0][k] + palette[1][k]) / 2);
                palette[2][3] = 255;
                palette[3][0] = palette[3][1] = palette[3][2] = palette[3][3] = 0;
            }
            uint32_t bits = LoadLE32(block + 4);
            for (int i = 0; i < 16; i++)
                memcpy(out[i], palette[(bits >> (2 * i)) & 3], 4);
        }

        void DecodeBlock(DXGI_FORMAT format, const uint8_t* block, uint8_t out[16][4])
        {
            switch (Unorm(format))
            {
            case DXGI_FORMAT_BC1_UNORM:
                DecodeColorBlock(block, true, out);
                break;
            case DXGI_FORMAT_BC2_UNORM:
                DecodeColorBlock(block + 8, false, out);
                for (int i = 0; i < 16; i++)
                    out[i][3] = uint8_t(((block[i / 2] >> ((i & 1) * 4)) & 15) * 17);
                break;
            default:  // BC3
            {
                DecodeColorBlock(block + 8, false, out);
                uint8_t a[8] = { block[0], block[1] };
                if (a[0] > a[1])
                    for (int k = 1; k < 7; k++)
                        a[k + 1] = uint8_t(((7 - k) * a[0] + k * a[1]) / 7);
                else
                {
                    for (int k = 1; k < 5; k++)
                        a[k + 1] = uint8_t(((5 - k) * a[0] + k * a[1]) / 5);
                    a[6] = 0;
                    a[7] = 255;
                }
                uint64_t bits = 0;
                for (int k = 0; k < 6; k++)
                    bits |= uint64_t(block[2 + k]) << (8 * k);
                for (int i = 0; i < 16; i++)
                    out[i][3] = a[(bits >> (3 * i)) & 7];
                break;
            }
            }
        }

        bool ParseKey(const std::string& stem, uint64_t& key)
        {
            if (stem.size() != 16)
                return false;
            char* end = nullptr;
            key = strtoull(stem.c_str(), &end, 16);
            return end == stem.c_str() + 16;
        }

        void Preload(std::vector<std::pair<uint64_t, PackFile>> files);
    }

    void Init()
    {
        std::lock_guard lock(g_mutex);
        settings::Values values = settings::AtStartup();
        g_dumping = values.textureDump;
        std::error_code error;
        size_t packs = 0;
        for (const vfs::ModInfo& mod : vfs::Mods())
        {
            fs::path folder = mod.root / "hd_textures";
            if (!fs::is_directory(folder, error))
                continue;
            // graphics.hd_textures (only in settings.toml, on by default) switches the mods' packs;
            // the HD AI Remaster's (hd_remaster.cpp) is used whenever graphics.hd_remaster is on.
            bool remaster = values.hdRemaster && fs::equivalent(mod.root, settings::HdRemasterDir(), error);
            bool use = values.hdTextures || remaster;
            g_enabled |= use;
            size_t files = 0;
            for (auto it = fs::directory_iterator(folder, error); !error && it != fs::directory_iterator(); it.increment(error))
            {
                uint64_t key;
                std::string extension = it->path().extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(), [](char c) { return char(tolower(uint8_t(c))); });
                if (extension != ".dds" || !ParseKey(it->path().stem().string(), key))
                    continue;
                if (!use)
                    files++;
                else if (g_index.emplace(key, PackFile{ it->path(), mod.name }).second)
                    files++;
            }
            packs++;
            Log("[gpu] hd textures: mod %s has %zu texture(s)%s", mod.name.c_str(), files, use ? "" : " (graphics.hd_textures = false: not used)");
        }
        if (!packs)
            Log("[gpu] hd textures: no mod has an hd_textures folder");
        if (g_enabled && !g_index.empty())
        {
            std::vector<std::pair<uint64_t, PackFile>> files(g_index.begin(), g_index.end());
            std::thread(Preload, std::move(files)).detach();
        }
        if (g_dumping)
        {
            g_dumpDir = vfs::UserDataDir() / "texture_dump";
            fs::create_directories(g_dumpDir, error);
            for (auto it = fs::directory_iterator(g_dumpDir, error); !error && it != fs::directory_iterator(); it.increment(error))
            {
                uint64_t key;
                if (it->path().extension() == ".png" && ParseKey(it->path().stem().string(), key))
                    g_dumped.insert(key);
            }
            bool fresh = !fs::exists(g_dumpDir / "index.csv", error);
            g_dumpIndex.open(g_dumpDir / "index.csv", std::ios::app);
            if (fresh)
                g_dumpIndex << "key,width,height,guest_format,host_format,swizzle,levels\n";
            Log("[gpu] texture dump on: %s (%zu already there)", g_dumpDir.string().c_str(), g_dumped.size());
        }
    }

    bool Active()
    {
        return g_enabled && !g_index.empty();
    }

    bool Dumping()
    {
        return g_dumping;
    }

    bool Replaceable(DXGI_FORMAT format)
    {
        switch (Unorm(format))
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC3_UNORM: return true;
        default: return false;
        }
    }

    bool IsSrgb(DXGI_FORMAT format)
    {
        return Unorm(format) != format;
    }

    uint64_t Key(const uint8_t* data, uint32_t rowPitch, uint32_t rowBytes, uint32_t rows, DXGI_FORMAT format, uint32_t width,
                 uint32_t height)
    {
        // Stable across versions (packs are named by it): 64-bit multiply-xor over 8-byte words,
        // the row tail byte by byte, seeded with the size and format.
        constexpr uint64_t P = 0x9E3779B97F4A7C15ull;
        uint64_t h = 0xCBF29CE484222325ull ^ (uint64_t(width) << 40) ^ (uint64_t(height) << 16) ^ uint64_t(Unorm(format));
        h *= P;
        for (uint32_t row = 0; row < rows; row++)
        {
            const uint8_t* p = data + size_t(row) * rowPitch;
            uint32_t i = 0;
            for (; i + 8 <= rowBytes; i += 8)
            {
                uint64_t v;
                memcpy(&v, p + i, 8);
                h = (h ^ v) * P;
                h ^= h >> 29;
            }
            for (; i < rowBytes; i++)
                h = (h ^ p[i]) * 0x100000001B3ull;
        }
        h ^= h >> 32;
        h *= P;
        h ^= h >> 29;
        return h;
    }

    void Dump(uint64_t key, const uint8_t* data, uint32_t rowPitch, DXGI_FORMAT format, uint32_t width, uint32_t height,
              uint32_t guestFormat, uint32_t swizzle, uint32_t levels)
    {
        if (!g_dumping)
            return;
        {
            std::lock_guard lock(g_mutex);
            if (!g_dumped.insert(key).second)
                return;
        }
        char name[32];
        snprintf(name, sizeof(name), "%016llX", (unsigned long long)key);
        std::vector<uint8_t> rgba(size_t(width) * height * 4);
        uint32_t blockBytes = BlockBytes(format);
        if (!blockBytes)
        {
            for (uint32_t y = 0; y < height; y++)
                memcpy(&rgba[size_t(y) * width * 4], data + size_t(y) * rowPitch, size_t(width) * 4);
        }
        else
        {
            for (uint32_t by = 0; by < (height + 3) / 4; by++)
            {
                for (uint32_t bx = 0; bx < (width + 3) / 4; bx++)
                {
                    uint8_t texels[16][4];
                    DecodeBlock(format, data + size_t(by) * rowPitch + size_t(bx) * blockBytes, texels);
                    for (uint32_t i = 0; i < 16; i++)
                    {
                        uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                        if (x < width && y < height)
                            memcpy(&rgba[(size_t(y) * width + x) * 4], texels[i], 4);
                    }
                }
            }
        }
        if (!WritePng(g_dumpDir / (std::string(name) + ".png"), rgba.data(), width, height, width * 4))
        {
            Log("[gpu] texture dump: could not write %s.png", name);
            return;
        }
        std::lock_guard lock(g_mutex);
        g_dumpIndex << name << ',' << width << ',' << height << ',' << guestFormat << ',' << FormatName(format) << (IsSrgb(format) ? "_SRGB" : "")
                    << ",0x" << std::hex << swizzle << std::dec << ',' << levels << '\n';
        g_dumpIndex.flush();
        if (++g_dumpCount % 100 == 0)
            Log("[gpu] texture dump: %u new texture(s)", g_dumpCount);
    }

    bool Has(uint64_t key)
    {
        std::lock_guard lock(g_mutex);
        return g_index.count(key) != 0;
    }

    namespace
    {
        // Reads and checks a pack file (format without gamma).
        bool ReadPack(const PackFile& file, Image& image)
        {
        image.source = "mod " + file.mod + ", " + file.path.filename().string();
        std::ifstream in(file.path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto fail = [&](const char* why) {
            Log("[gpu] hd texture %s: %s (the guest texture is used)", file.path.string().c_str(), why);
            return false;
        };
        if (bytes.size() < 128 || memcmp(bytes.data(), "DDS ", 4) != 0 || LoadLE32(&bytes[4]) != 124)
            return fail("not a DDS file");
        image.height = LoadLE32(&bytes[12]);
        image.width = LoadLE32(&bytes[16]);
        image.levels = std::max(1u, LoadLE32(&bytes[28]));
        uint32_t pfFlags = LoadLE32(&bytes[80]), fourCC = LoadLE32(&bytes[84]), bits = LoadLE32(&bytes[88]);
        size_t dataOffset = 128;
        auto cc = [](const char* s) { return uint32_t(uint8_t(s[0])) | (uint32_t(uint8_t(s[1])) << 8) | (uint32_t(uint8_t(s[2])) << 16) | (uint32_t(uint8_t(s[3])) << 24); };
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        if ((pfFlags & 4) && fourCC == cc("DX10"))
        {
            if (bytes.size() < 148)
                return fail("truncated DX10 header");
            format = DXGI_FORMAT(LoadLE32(&bytes[128]));
            dataOffset = 148;
        }
        else if ((pfFlags & 4) && fourCC == cc("DXT1"))
            format = DXGI_FORMAT_BC1_UNORM;
        else if ((pfFlags & 4) && (fourCC == cc("DXT2") || fourCC == cc("DXT3")))
            format = DXGI_FORMAT_BC2_UNORM;
        else if ((pfFlags & 4) && (fourCC == cc("DXT4") || fourCC == cc("DXT5")))
            format = DXGI_FORMAT_BC3_UNORM;
        else if ((pfFlags & 0x40) && bits == 32 && LoadLE32(&bytes[92]) == 0xFF && LoadLE32(&bytes[100]) == 0xFF0000)
            format = DXGI_FORMAT_R8G8B8A8_UNORM;
        format = Unorm(format);
        if (format != DXGI_FORMAT_R8G8B8A8_UNORM && !BlockBytes(format))
            return fail("unsupported format (use BC1, BC2, BC3, BC7 or RGBA8)");
        if (!image.width || !image.height || image.width > 16384 || image.height > 16384)
            return fail("bad size");
        image.levels = std::min(image.levels, 15u);
        uint32_t blockBytes = BlockBytes(format);
        size_t offset = 0;
        image.layout.clear();
        for (uint32_t level = 0; level < image.levels; level++)
        {
            uint32_t w = std::max(image.width >> level, 1u), h = std::max(image.height >> level, 1u);
            Image::Level l{ offset, blockBytes ? ((w + 3) / 4) * blockBytes : w * 4, blockBytes ? (h + 3) / 4 : h };
            image.layout.push_back(l);
            offset += size_t(l.rowBytes) * l.rows;
        }
        if (dataOffset + offset > bytes.size())
            return fail("truncated data");
        image.data.assign(bytes.begin() + dataOffset, bytes.begin() + dataOffset + offset);
        image.format = format;
        return true;
        }

        // Pack files read ahead by a background thread at startup (up to PRELOAD_LIMIT bytes),
        // so the renderer doesn't wait for the disk when a level first draws them; each one is
        // dropped once the renderer took it.
        constexpr size_t PRELOAD_LIMIT = size_t(1) << 30;
        std::mutex g_preloadMutex;
        std::unordered_map<uint64_t, Image> g_preloaded;
        std::unordered_set<uint64_t> g_taken;  // taken (or loaded directly) before the preload reached them

        void Preload(std::vector<std::pair<uint64_t, PackFile>> files)
        {
            size_t total = 0, count = 0;
            for (auto& [key, file] : files)
            {
                {
                    std::lock_guard lock(g_preloadMutex);
                    if (g_taken.count(key))
                        continue;
                }
                Image image;
                if (!ReadPack(file, image))
                    continue;
                total += image.data.size();
                count++;
                std::lock_guard lock(g_preloadMutex);
                if (!g_taken.count(key))
                    g_preloaded.emplace(key, std::move(image));
                if (total >= PRELOAD_LIMIT)
                {
                    Log("[gpu] hd textures: preload stopped at %zu MB (%zu file(s)); the rest load when first drawn", total >> 20, count);
                    return;
                }
            }
            Log("[gpu] hd textures: %zu file(s) preloaded (%zu MB)", count, total >> 20);
        }
    }

    bool Load(uint64_t key, bool srgb, Image& image)
    {
        {
            std::lock_guard lock(g_preloadMutex);
            g_taken.insert(key);
            auto it = g_preloaded.find(key);
            if (it != g_preloaded.end())
            {
                image = std::move(it->second);
                g_preloaded.erase(it);
                image.format = srgb ? Srgb(image.format) : image.format;
                return true;
            }
        }
        PackFile file;
        {
            std::lock_guard lock(g_mutex);
            auto it = g_index.find(key);
            if (it == g_index.end())
                return false;
            file = it->second;
        }
        if (!ReadPack(file, image))
            return false;
        image.format = srgb ? Srgb(image.format) : image.format;
        return true;
    }
}
