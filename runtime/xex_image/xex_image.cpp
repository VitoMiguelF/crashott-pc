// See xex_image.h. Compiled in the XexImage target with XenonUtils' include paths, not in the runtime.

#include "xex_image.h"

#include <image.h>

namespace xex_image
{
    bool Load(const std::vector<uint8_t>& xex, std::vector<uint8_t>& image, uint32_t& base, std::string& error)
    {
        Image parsed = Image::ParseImage(xex.data(), xex.size());
        if (!parsed.data || parsed.size == 0)
        {
            error = "not a valid XEX2 (or it could not be decrypted / decompressed)";
            return false;
        }
        image.assign(parsed.data.get(), parsed.data.get() + parsed.size);
        base = uint32_t(parsed.base);
        return true;
    }
}
