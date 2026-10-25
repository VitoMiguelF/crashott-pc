#include "image.h"

namespace plat
{
    // TODO(linux): stb_image (one header, zlib licence-compatible) or libpng.
    bool DecodePng(const uint8_t*, size_t, int&, int&, std::vector<uint8_t>&)
    {
        return false;
    }
}
