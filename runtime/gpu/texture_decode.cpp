#include "texture_decode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gpu
{
    namespace
    {
        uint32_t Log2Ceil(uint32_t value)
        {
            uint32_t log = 0;
            while ((1u << log) < value)
                log++;
            return log;
        }

        uint32_t AlignUp(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        uint32_t Log2Floor(uint32_t value)
        {
            uint32_t log = 0;
            while (value > 1)
            {
                value >>= 1;
                log++;
            }
            return log;
        }

        uint8_t Expand(uint32_t value, uint32_t bits)
        {
            uint32_t max = (1u << bits) - 1;
            return uint8_t((value * 255 + max / 2) / max);
        }
    }

    bool DescribeTextureFormat(const xenos::TextureFetch& fetch, GuestTextureFormat& out)
    {
        using xenos::TextureFormat;
        bool isSigned = fetch.Sign(0) == uint32_t(xenos::TextureSign::Signed);
        bool isGamma = fetch.Sign(0) == uint32_t(xenos::TextureSign::Gamma);
        out = GuestTextureFormat{};

        auto plain = [&](uint32_t bytes, DXGI_FORMAT format) {
            out.bytesPerBlock = bytes;
            out.hostFormat = format;
            out.hostBytesPerUnit = bytes;
        };
        auto converted = [&](uint32_t bytes, TextureConversion conversion, DXGI_FORMAT format, uint32_t hostBytes) {
            out.bytesPerBlock = bytes;
            out.conversion = conversion;
            out.hostFormat = format;
            out.hostBytesPerUnit = hostBytes;
        };
        auto compressed = [&](uint32_t bytes, DXGI_FORMAT format) {
            out.blockWidth = 4;
            out.blockHeight = 4;
            out.bytesPerBlock = bytes;
            out.hostFormat = format;
            out.hostBytesPerUnit = bytes;
            out.hostIsBlockCompressed = true;
        };

        switch (TextureFormat(fetch.Format()))
        {
        case TextureFormat::k_8:
        case TextureFormat::k_8_A:
        case TextureFormat::k_8_B:
            plain(1, isSigned ? DXGI_FORMAT_R8_SNORM : DXGI_FORMAT_R8_UNORM);
            return true;
        case TextureFormat::k_8_8:
            plain(2, isSigned ? DXGI_FORMAT_R8G8_SNORM : DXGI_FORMAT_R8G8_UNORM);
            return true;
        case TextureFormat::k_8_8_8_8:
        case TextureFormat::k_8_8_8_8_A:
        case TextureFormat::k_8_8_8_8_AS_16_16_16_16:
            plain(4, isSigned ? DXGI_FORMAT_R8G8B8A8_SNORM : isGamma ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM);
            return true;
        case TextureFormat::k_2_10_10_10:
        case TextureFormat::k_2_10_10_10_AS_16_16_16_16:
            plain(4, DXGI_FORMAT_R10G10B10A2_UNORM);
            return true;
        case TextureFormat::k_5_6_5:
            converted(2, TextureConversion::R5G6B5, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
            return true;
        case TextureFormat::k_6_5_5:
            converted(2, TextureConversion::R6G5B5, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
            return true;
        case TextureFormat::k_1_5_5_5:
            converted(2, TextureConversion::R5G5B5A1, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
            return true;
        case TextureFormat::k_4_4_4_4:
            converted(2, TextureConversion::R4G4B4A4, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
            return true;
        case TextureFormat::k_10_11_11:
        case TextureFormat::k_10_11_11_AS_16_16_16_16:
            converted(4, TextureConversion::R10G11B11, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
            return true;
        case TextureFormat::k_11_11_10:
        case TextureFormat::k_11_11_10_AS_16_16_16_16:
            converted(4, TextureConversion::R11G11B10, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
            return true;
        case TextureFormat::k_DXT1:
        case TextureFormat::k_DXT1_AS_16_16_16_16:
            compressed(8, isGamma ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM);
            return true;
        case TextureFormat::k_DXT2_3:
        case TextureFormat::k_DXT2_3_AS_16_16_16_16:
            compressed(16, isGamma ? DXGI_FORMAT_BC2_UNORM_SRGB : DXGI_FORMAT_BC2_UNORM);
            return true;
        case TextureFormat::k_DXT4_5:
        case TextureFormat::k_DXT4_5_AS_16_16_16_16:
            compressed(16, isGamma ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM);
            return true;
        case TextureFormat::k_DXN:
            compressed(16, isSigned ? DXGI_FORMAT_BC5_SNORM : DXGI_FORMAT_BC5_UNORM);
            return true;
        case TextureFormat::k_DXT5A:
            compressed(8, isSigned ? DXGI_FORMAT_BC4_SNORM : DXGI_FORMAT_BC4_UNORM);
            return true;
        case TextureFormat::k_CTX1:
            out.blockWidth = out.blockHeight = 4;
            converted(8, TextureConversion::CTX1, DXGI_FORMAT_R8G8_UNORM, 2);
            return true;
        case TextureFormat::k_DXT3A:
        case TextureFormat::k_DXT3A_AS_1_1_1_1:
            out.blockWidth = out.blockHeight = 4;
            converted(8, TextureConversion::DXT3A, DXGI_FORMAT_R8_UNORM, 1);
            return true;
        case TextureFormat::k_16:
        case TextureFormat::k_16_EXPAND:
            plain(2, isSigned ? DXGI_FORMAT_R16_SNORM : DXGI_FORMAT_R16_UNORM);
            return true;
        case TextureFormat::k_16_16:
        case TextureFormat::k_16_16_EXPAND:
            plain(4, isSigned ? DXGI_FORMAT_R16G16_SNORM : DXGI_FORMAT_R16G16_UNORM);
            return true;
        case TextureFormat::k_16_16_16_16:
        case TextureFormat::k_16_16_16_16_EXPAND:
            plain(8, isSigned ? DXGI_FORMAT_R16G16B16A16_SNORM : DXGI_FORMAT_R16G16B16A16_UNORM);
            return true;
        case TextureFormat::k_16_FLOAT:
            plain(2, DXGI_FORMAT_R16_FLOAT);
            return true;
        case TextureFormat::k_16_16_FLOAT:
            plain(4, DXGI_FORMAT_R16G16_FLOAT);
            return true;
        case TextureFormat::k_16_16_16_16_FLOAT:
            plain(8, DXGI_FORMAT_R16G16B16A16_FLOAT);
            return true;
        case TextureFormat::k_32_FLOAT:
            plain(4, DXGI_FORMAT_R32_FLOAT);
            return true;
        case TextureFormat::k_32_32_FLOAT:
            plain(8, DXGI_FORMAT_R32G32_FLOAT);
            return true;
        case TextureFormat::k_32_32_32_32_FLOAT:
            plain(16, DXGI_FORMAT_R32G32B32A32_FLOAT);
            return true;
        case TextureFormat::k_24_8:
            converted(4, TextureConversion::D24S8, DXGI_FORMAT_R32_FLOAT, 4);
            return true;
        case TextureFormat::k_24_8_FLOAT:
            converted(4, TextureConversion::D24FS8, DXGI_FORMAT_R32_FLOAT, 4);
            return true;
        default:
            return false;
        }
    }

    TextureLayout ComputeTextureLayout(const xenos::TextureFetch& fetch, const GuestTextureFormat& format)
    {
        TextureLayout layout{};
        layout.baseAddress = 0xA0000000 | (fetch.BaseAddress() & 0x1FFFFFFF);
        layout.tiled = fetch.Tiled();
        uint32_t width = fetch.Width();
        uint32_t height = fetch.Height();
        layout.widthBlocks = (width + format.blockWidth - 1) / format.blockWidth;
        layout.heightBlocks = (height + format.blockHeight - 1) / format.blockHeight;

        uint32_t pitchTexels = std::max(fetch.Pitch(), width);
        uint32_t pitchBlocks = (pitchTexels + format.blockWidth - 1) / format.blockWidth;
        uint32_t heightBlocksAligned = AlignUp(layout.heightBlocks, 32);

        // A base level no larger than 16 texels in its smaller dimension sits in the packed mip
        // tail (Xenia's GetPackedMipOffset for mip 0).
        if (fetch.PackedMips())
        {
            uint32_t log2Width = Log2Ceil(width), log2Height = Log2Ceil(height);
            if (std::min(log2Width, log2Height) <= 4)
            {
                uint32_t x = 0, y = 0;
                if (log2Width > log2Height)
                    y = 16;
                else
                    x = 16;
                layout.offsetXBlocks = x / format.blockWidth;
                layout.offsetYBlocks = y / format.blockHeight;
            }
        }

        if (layout.tiled)
        {
            layout.pitchBlocks = AlignUp(std::max(pitchBlocks, layout.widthBlocks + layout.offsetXBlocks), 32);
            uint32_t rows = AlignUp(std::max(heightBlocksAligned, layout.heightBlocks + layout.offsetYBlocks), 32);
            layout.sizeBytes = layout.pitchBlocks * rows * format.bytesPerBlock;
        }
        else
        {
            layout.pitchBlocks = pitchBlocks;
            layout.rowPitchBytes = AlignUp(pitchBlocks * format.bytesPerBlock, 256);
            layout.sizeBytes = layout.rowPitchBytes * (layout.heightBlocks + layout.offsetYBlocks);
        }
        return layout;
    }

    namespace
    {
        uint32_t NextPow2(uint32_t value)
        {
            return 1u << Log2Ceil(std::max(value, 1u));
        }

        // Xenia's GetPackedMipLevel: the first level stored in the packed mip tail.
        uint32_t PackedMipLevel(uint32_t width, uint32_t height)
        {
            uint32_t log2Size = Log2Ceil(std::min(width, height));
            return log2Size > 4 ? log2Size - 4 : 0;
        }

        // Xenia's GetPackedMipOffset (2D): position of level `mip` inside the packed tail, in
        // texels; width/height are the base level's.
        void PackedMipOffset(uint32_t width, uint32_t height, uint32_t mip, uint32_t& x, uint32_t& y)
        {
            x = y = 0;
            uint32_t log2Width = Log2Ceil(width), log2Height = Log2Ceil(height);
            uint32_t log2Size = std::min(log2Width, log2Height);
            if (log2Size > 4 + mip)
                return;
            uint32_t packedBase = log2Size > 4 ? log2Size - 4 : 0;
            uint32_t packed = mip - packedBase;
            if (packed < 3)
            {
                if (log2Width > log2Height)
                    y = 16 >> packed;
                else
                    x = 16 >> packed;
            }
            else if (log2Width > log2Height)
            {
                x = (1u << (log2Width - packedBase)) >> (packed - 2);
            }
            else
            {
                y = (1u << (log2Height - packedBase)) >> (packed - 2);
            }
        }

        // Row pitch and rows (in blocks) of the storage of mip level `level`.
        void MipStorage(uint32_t width, uint32_t height, const GuestTextureFormat& format, uint32_t level, uint32_t& pitchBlocks, uint32_t& rowsBlocks)
        {
            uint32_t pitchTexels = std::max(NextPow2(width) >> level, 1u);
            uint32_t rowTexels = std::max(NextPow2(height) >> level, 1u);
            pitchBlocks = AlignUp((pitchTexels + format.blockWidth - 1) / format.blockWidth, 32);
            rowsBlocks = AlignUp((rowTexels + format.blockHeight - 1) / format.blockHeight, 32);
        }
    }

    uint32_t TextureLevelCount(const xenos::TextureFetch& fetch)
    {
        if (fetch.Dimension() != 1 || fetch.Stacked() || !fetch.MipAddress())
            return 1;
        uint32_t maxForSize = Log2Floor(std::max(fetch.Width(), fetch.Height()));
        uint32_t maxLevel = std::min(fetch.MipMaxLevel(), maxForSize);
        return maxLevel + 1;
    }

    TextureLayout ComputeMipLayout(const xenos::TextureFetch& fetch, const GuestTextureFormat& format, uint32_t level,
                                   uint32_t widthBlocks, uint32_t heightBlocks)
    {
        TextureLayout layout{};
        layout.tiled = fetch.Tiled();
        layout.widthBlocks = widthBlocks;
        layout.heightBlocks = heightBlocks;
        uint32_t width = fetch.Width(), height = fetch.Height();
        uint32_t packedLevel = fetch.PackedMips() ? PackedMipLevel(width, height) : UINT32_MAX;
        // With the whole chain packed (packed level 0), the mips' tail is stored like a level 0.
        uint32_t storageLevel = std::min(level, packedLevel);

        uint32_t offsetBytes = 0;
        for (uint32_t l = 1; l < storageLevel; l++)
        {
            uint32_t pitch, rows;
            MipStorage(width, height, format, l, pitch, rows);
            uint32_t rowBytes = pitch * format.bytesPerBlock;
            if (!layout.tiled)
                rowBytes = AlignUp(rowBytes, 256);
            offsetBytes += AlignUp(rowBytes * rows, 4096);
        }
        uint32_t pitch, rows;
        MipStorage(width, height, format, storageLevel, pitch, rows);
        layout.baseAddress = 0xA0000000 | ((fetch.MipAddress() + offsetBytes) & 0x1FFFFFFF);

        if (level >= packedLevel)
        {
            uint32_t x, y;
            PackedMipOffset(width, height, level, x, y);
            layout.offsetXBlocks = x / format.blockWidth;
            layout.offsetYBlocks = y / format.blockHeight;
        }
        if (layout.tiled)
        {
            layout.pitchBlocks = AlignUp(std::max(pitch, widthBlocks + layout.offsetXBlocks), 32);
            uint32_t allRows = AlignUp(std::max(rows, heightBlocks + layout.offsetYBlocks), 32);
            layout.sizeBytes = layout.pitchBlocks * allRows * format.bytesPerBlock;
        }
        else
        {
            layout.pitchBlocks = pitch;
            layout.rowPitchBytes = AlignUp(pitch * format.bytesPerBlock, 256);
            layout.sizeBytes = layout.rowPitchBytes * (heightBlocks + layout.offsetYBlocks);
        }
        return layout;
    }

    void HostTextureSize(const TextureLayout& layout, const GuestTextureFormat& format, uint32_t& width, uint32_t& height)
    {
        width = layout.widthBlocks * format.blockWidth;
        height = layout.heightBlocks * format.blockHeight;
    }

    void HostTextureRows(const TextureLayout& layout, const GuestTextureFormat& format, uint32_t& rowBytes, uint32_t& rows)
    {
        if (format.hostIsBlockCompressed || format.conversion == TextureConversion::None)
        {
            rowBytes = layout.widthBlocks * format.hostBytesPerUnit;
            rows = layout.heightBlocks;
        }
        else
        {
            rowBytes = layout.widthBlocks * format.blockWidth * format.hostBytesPerUnit;
            rows = layout.heightBlocks * format.blockHeight;
        }
    }

    void DecodeTexture(const uint8_t* guest, uint8_t* scratch, const TextureLayout& layout, const GuestTextureFormat& format,
                       uint32_t endian, uint8_t* dest, uint32_t destRowPitch)
    {
        // Endian swap of the whole range first: the swap applies to memory words, regardless of
        // which texels they hold.
        uint32_t words = layout.sizeBytes / 4;
        const uint32_t* source = reinterpret_cast<const uint32_t*>(guest);
        uint32_t* swapped = reinterpret_cast<uint32_t*>(scratch);
        for (uint32_t i = 0; i < words; i++)
            swapped[i] = xenos::GpuSwap(source[i], endian);

        uint32_t bpb = format.bytesPerBlock;
        uint32_t bppLog2 = Log2Floor(bpb);
        auto blockAddress = [&](uint32_t bx, uint32_t by) -> const uint8_t* {
            uint32_t x = bx + layout.offsetXBlocks, y = by + layout.offsetYBlocks;
            uint32_t offset;
            if (layout.tiled)
                offset = uint32_t(xenos::Tiled2D(int32_t(x), int32_t(y), layout.pitchBlocks, bppLog2));
            else
                offset = y * layout.rowPitchBytes + x * bpb;
            if (offset + bpb > layout.sizeBytes)
                return nullptr;
            return scratch + offset;
        };

        for (uint32_t by = 0; by < layout.heightBlocks; by++)
        {
            for (uint32_t bx = 0; bx < layout.widthBlocks; bx++)
            {
                const uint8_t* block = blockAddress(bx, by);
                switch (format.conversion)
                {
                case TextureConversion::None:
                {
                    uint8_t* out = dest + size_t(by) * destRowPitch + size_t(bx) * bpb;
                    if (block)
                        memcpy(out, block, bpb);
                    else
                        memset(out, 0, bpb);
                    break;
                }
                case TextureConversion::CTX1:
                case TextureConversion::DXT3A:
                {
                    uint8_t texels[16][2] = {};
                    if (block)
                    {
                        if (format.conversion == TextureConversion::DXT3A)
                        {
                            for (uint32_t i = 0; i < 16; i++)
                            {
                                uint32_t nibble = (block[i / 2] >> ((i & 1) * 4)) & 0xF;
                                texels[i][0] = uint8_t(nibble * 17);
                            }
                        }
                        else
                        {
                            // CTX1: two 8:8 endpoints then 2-bit indices (Xenia's texture_conversion).
                            uint8_t r0 = block[0], g0 = block[1], r1 = block[2], g1 = block[3];
                            uint32_t indices = uint32_t(block[4]) | (uint32_t(block[5]) << 8) | (uint32_t(block[6]) << 16) | (uint32_t(block[7]) << 24);
                            uint8_t palette[4][2] = { { r0, g0 }, { r1, g1 },
                                                      { uint8_t((2 * r0 + r1) / 3), uint8_t((2 * g0 + g1) / 3) },
                                                      { uint8_t((r0 + 2 * r1) / 3), uint8_t((g0 + 2 * g1) / 3) } };
                            for (uint32_t i = 0; i < 16; i++)
                            {
                                uint32_t index = (indices >> (i * 2)) & 3;
                                texels[i][0] = palette[index][0];
                                texels[i][1] = palette[index][1];
                            }
                        }
                    }
                    uint32_t unit = format.hostBytesPerUnit;
                    for (uint32_t y = 0; y < 4; y++)
                    {
                        uint8_t* row = dest + size_t(by * 4 + y) * destRowPitch + size_t(bx) * 4 * unit;
                        for (uint32_t x = 0; x < 4; x++)
                            memcpy(row + x * unit, texels[y * 4 + x], unit);
                    }
                    break;
                }
                default:
                {
                    uint8_t* out = dest + size_t(by) * destRowPitch + size_t(bx) * format.hostBytesPerUnit;
                    uint32_t value = 0;
                    if (block)
                        memcpy(&value, block, bpb);
                    switch (format.conversion)
                    {
                    case TextureConversion::R5G6B5:
                        out[0] = Expand(value & 0x1F, 5); out[1] = Expand((value >> 5) & 0x3F, 6);
                        out[2] = Expand((value >> 11) & 0x1F, 5); out[3] = 255;
                        break;
                    case TextureConversion::R6G5B5:
                        out[0] = Expand(value & 0x3F, 6); out[1] = Expand((value >> 6) & 0x1F, 5);
                        out[2] = Expand((value >> 11) & 0x1F, 5); out[3] = 255;
                        break;
                    case TextureConversion::R5G5B5A1:
                        out[0] = Expand(value & 0x1F, 5); out[1] = Expand((value >> 5) & 0x1F, 5);
                        out[2] = Expand((value >> 10) & 0x1F, 5); out[3] = (value >> 15) & 1 ? 255 : 0;
                        break;
                    case TextureConversion::R4G4B4A4:
                        out[0] = Expand(value & 0xF, 4); out[1] = Expand((value >> 4) & 0xF, 4);
                        out[2] = Expand((value >> 8) & 0xF, 4); out[3] = Expand((value >> 12) & 0xF, 4);
                        break;
                    case TextureConversion::R10G11B11:
                        out[0] = Expand(value & 0x7FF, 11); out[1] = Expand((value >> 11) & 0x7FF, 11);
                        out[2] = Expand((value >> 22) & 0x3FF, 10); out[3] = 255;
                        break;
                    case TextureConversion::R11G11B10:
                        out[0] = Expand(value & 0x3FF, 10); out[1] = Expand((value >> 10) & 0x7FF, 11);
                        out[2] = Expand((value >> 21) & 0x7FF, 11); out[3] = 255;
                        break;
                    case TextureConversion::D24S8:
                    {
                        float depth = float(value >> 8) / 16777215.0f;
                        memcpy(out, &depth, 4);
                        break;
                    }
                    case TextureConversion::D24FS8:
                    {
                        // 20e4 float depth in the upper 24 bits.
                        uint32_t f24 = value >> 8;
                        uint32_t exponent = f24 >> 20, mantissa = f24 & 0xFFFFF;
                        float depth = exponent ? ldexpf(1.0f + float(mantissa) / 1048576.0f, int(exponent) - 15)
                                               : ldexpf(float(mantissa) / 1048576.0f, -14);
                        memcpy(out, &depth, 4);
                        break;
                    }
                    default:
                        break;
                    }
                    break;
                }
                }
            }
        }
    }
}
