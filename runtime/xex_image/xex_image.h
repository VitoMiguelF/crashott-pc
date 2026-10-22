#pragma once
// Decrypts and decompresses the title's default.xex into the flat image the recompiled code
// expects (the same bytes tools/xexdump wrote to image.bin), with XenonRecomp's XenonUtils.
// Built as its own library (CMake target XexImage) because XenonUtils' headers clash with the
// runtime's (ppc_context.h); this header only uses the standard library.

#include <cstdint>
#include <string>
#include <vector>

namespace xex_image
{
    // `xex` is the whole default.xex. On success `image` holds the image from its base address
    // (0x82000000) and `base` that address; on failure `error` says why.
    bool Load(const std::vector<uint8_t>& xex, std::vector<uint8_t>& image, uint32_t& base, std::string& error);
}
