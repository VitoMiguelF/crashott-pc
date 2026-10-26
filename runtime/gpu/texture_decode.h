#pragma once

// Guest texture -> host texture data: format mapping, tiled (2D) addressing, endian swap, mip
// levels (ComputeMipLayout) and the CPU conversions for formats D3D12 lacks. Layout rules follow
// Xenia's texture_info / texture_util (BSD license).

#include "xenos.h"

#include "dxgi_format.h"

#include <cstdint>

namespace gpu
{
    enum class TextureConversion
    {
        None,       // copy blocks as they are
        R5G6B5,     // -> RGBA8
        R5G5B5A1,   // -> RGBA8
        R4G4B4A4,   // -> RGBA8
        R6G5B5,     // -> RGBA8
        R10G11B11,  // -> RGBA8 (unorm approximation)
        R11G11B10,  // -> RGBA8
        CTX1,       // 4x4 block -> R8G8 texels
        DXT3A,      // 4x4 block -> R8 texels
        // A GPU without BC textures (Mali; SetHostBlockCompression(false)): the DXT blocks decoded.
        BC1,        // 4x4 block -> RGBA8 texels
        BC2,        // 4x4 block -> RGBA8 texels
        BC3,        // 4x4 block -> RGBA8 texels
        BC4,        // 4x4 block -> R8 texels (unsigned or signed, as the host format)
        BC5,        // 4x4 block -> R8G8 texels
        D24S8,      // -> R32_FLOAT depth
        D24FS8,     // -> R32_FLOAT depth
    };

    struct GuestTextureFormat
    {
        uint32_t blockWidth = 1;
        uint32_t blockHeight = 1;
        uint32_t bytesPerBlock = 4;     // guest
        DXGI_FORMAT hostFormat = DXGI_FORMAT_UNKNOWN;
        TextureConversion conversion = TextureConversion::None;
        // Host data: either blocks (no conversion: same block size as the guest) or texels.
        uint32_t hostBytesPerUnit = 4;
        bool hostIsBlockCompressed = false;
    };

    // Whether the host samples BC1-5 (default true); false makes DescribeTextureFormat decode the
    // DXT formats into plain texels. Set once the device is known.
    void SetHostBlockCompression(bool supported);

    // False if the format isn't supported (the texture is then bound as black).
    bool DescribeTextureFormat(const xenos::TextureFetch& fetch, GuestTextureFormat& out);

    struct TextureLayout
    {
        uint32_t baseAddress;   // guest (0xA0000000-based)
        uint32_t widthBlocks;
        uint32_t heightBlocks;
        uint32_t pitchBlocks;   // tiled: aligned to 32
        uint32_t offsetXBlocks; // packed mip tail position of the base level
        uint32_t offsetYBlocks;
        uint32_t rowPitchBytes; // linear only
        uint32_t sizeBytes;     // guest memory read
        bool tiled;
    };

    TextureLayout ComputeTextureLayout(const xenos::TextureFetch& fetch, const GuestTextureFormat& format);

    // Mip levels stored for a 2D texture (1 = base only): the mip address must be set, and the
    // fetch constant's maximum level is clamped to the size.
    uint32_t TextureLevelCount(const xenos::TextureFetch& fetch);
    // Layout of level `level` (>= 1; level 0 is ComputeTextureLayout) with the given size in
    // blocks (the host level's, which can be a little larger than the guest's for sizes that
    // aren't multiples of the block size). Mips are stored from the mip address with the pitch of
    // a power-of-two texture, 32x32-block aligned, each level 4 KB aligned; the levels from the
    // first one whose smaller side is <= 16 texels share one packed tail.
    TextureLayout ComputeMipLayout(const xenos::TextureFetch& fetch, const GuestTextureFormat& format, uint32_t level,
                                   uint32_t widthBlocks, uint32_t heightBlocks);

    // Host texture size in texels.
    void HostTextureSize(const TextureLayout& layout, const GuestTextureFormat& format, uint32_t& width, uint32_t& height);
    // Bytes per host row (unpadded) and number of host rows.
    void HostTextureRows(const TextureLayout& layout, const GuestTextureFormat& format, uint32_t& rowBytes, uint32_t& rows);

    // `guest` points at the guest memory of the texture (layout.sizeBytes); `scratch` must hold
    // layout.sizeBytes (used for the endian-swapped copy).
    void DecodeTexture(const uint8_t* guest, uint8_t* scratch, const TextureLayout& layout, const GuestTextureFormat& format,
                       uint32_t endian, uint8_t* dest, uint32_t destRowPitch);
}
