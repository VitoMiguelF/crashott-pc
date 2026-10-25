#include "image.h"

// stb_image (thirdparty/stb, public domain), PNG only, from memory.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

#include <climits>
#include <cstring>

namespace plat
{
    bool DecodePng(const uint8_t* data, size_t size, int& width, int& height, std::vector<uint8_t>& rgba)
    {
        if (size > size_t(INT_MAX))
            return false;
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(data, int(size), &width, &height, &channels, 4);  // palette + tRNS -> RGBA
        if (!pixels)
            return false;
        rgba.assign(pixels, pixels + size_t(width) * size_t(height) * 4);
        stbi_image_free(pixels);
        return width > 0 && height > 0;
    }
}
