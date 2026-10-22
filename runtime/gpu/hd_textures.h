#pragma once

// HD texture packs: host-side replacements for guest textures (tools/hd_textures.py builds them).
//
// The guest keeps loading and sampling addresses of its own texture, so guest memory and the
// game's fixed physical pools are untouched; only the host texture the renderer binds changes.
// A texture is identified by its key: a hash of its decoded base level (host texels, rows without
// padding) with its width, height and host format (gamma ignored). The key is computed when the
// renderer uploads a guest texture, so it never covers render targets or resolve copies (those
// are bound from the resolve targets and never uploaded).
//
// Pack: mods\<mod>\hd_textures\<key as 16 hex digits>.dds (BC1/BC2/BC3/BC7 or RGBA8, any number
// of mips; the first mod in load order wins), plus a manifest.json for people and tools. The DDS
// holds the channels in the order of the guest's host texture (the fetch constant swizzle is
// applied after sampling, as for the guest texture); tools/hd_textures.py takes care of that.
//
// Dump mode (graphics.texture_dump): each new replaceable texture is written once to
// userdata\texture_dump\<key>.png (base level, host channel order, no swizzle) with a line in
// index.csv (key, size, formats, fetch swizzle), which tools/hd_textures.py matches to the
// exported textures.

#include <dxgiformat.h>

#include <cstdint>
#include <string>
#include <vector>

namespace gpu::hd
{
    // Reads the settings (graphics.hd_textures, graphics.texture_dump) and indexes the packs of the
    // enabled mods. Call after vfs::Init and settings::Load.
    void Init();
    bool Active();   // replacements on and at least one pack file found
    bool Dumping();

    // Host formats whose textures can be dumped and replaced (8-bit RGBA, BC1-BC3).
    bool Replaceable(DXGI_FORMAT format);
    bool IsSrgb(DXGI_FORMAT format);

    // `data`: the decoded base level (rows of `rowBytes` every `rowPitch` bytes; `rows` block rows
    // for BC formats).
    uint64_t Key(const uint8_t* data, uint32_t rowPitch, uint32_t rowBytes, uint32_t rows, DXGI_FORMAT format,
                 uint32_t width, uint32_t height);

    // Writes the dump of `key` once (no-op when not dumping or already dumped).
    void Dump(uint64_t key, const uint8_t* data, uint32_t rowPitch, DXGI_FORMAT format, uint32_t width, uint32_t height,
              uint32_t guestFormat, uint32_t swizzle, uint32_t levels);

    bool Has(uint64_t key);

    struct Image
    {
        uint32_t width = 0, height = 0, levels = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        std::vector<uint8_t> data;  // levels back to back, tightly packed (DDS order)
        struct Level
        {
            size_t offset;
            uint32_t rowBytes, rows;  // rows: block rows for BC formats
        };
        std::vector<Level> layout;
        std::string source;  // "mod X, file" for the log
    };
    // Loads the pack file of `key`; `srgb` picks the gamma variant of its format (as the guest's).
    bool Load(uint64_t key, bool srgb, Image& image);
}
