#include "png_writer.h"
#include "../platform/crt.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace gpu
{
    namespace
    {
        uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0)
        {
            static uint32_t table[256];
            static bool ready = false;
            if (!ready)
            {
                for (uint32_t n = 0; n < 256; n++)
                {
                    uint32_t c = n;
                    for (int k = 0; k < 8; k++)
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    table[n] = c;
                }
                ready = true;
            }
            crc = ~crc;
            for (size_t i = 0; i < size; i++)
                crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        void PutBE32(std::vector<uint8_t>& out, uint32_t value)
        {
            out.push_back(uint8_t(value >> 24));
            out.push_back(uint8_t(value >> 16));
            out.push_back(uint8_t(value >> 8));
            out.push_back(uint8_t(value));
        }

        void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
        {
            PutBE32(out, uint32_t(data.size()));
            size_t start = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), data.begin(), data.end());
            PutBE32(out, Crc32(out.data() + start, out.size() - start));
        }
    }

    bool WritePng(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t pitch)
    {
        // Raw scanlines with filter byte 0.
        std::vector<uint8_t> raw;
        raw.reserve(size_t(width * 4 + 1) * height);
        for (uint32_t y = 0; y < height; y++)
        {
            raw.push_back(0);
            raw.insert(raw.end(), rgba + size_t(y) * pitch, rgba + size_t(y) * pitch + size_t(width) * 4);
        }

        // zlib stream with stored blocks.
        std::vector<uint8_t> zlib = { 0x78, 0x01 };
        size_t offset = 0;
        do
        {
            size_t block = std::min<size_t>(65535, raw.size() - offset);
            bool last = offset + block == raw.size();
            zlib.push_back(last ? 1 : 0);
            zlib.push_back(uint8_t(block));
            zlib.push_back(uint8_t(block >> 8));
            zlib.push_back(uint8_t(~block));
            zlib.push_back(uint8_t(~block >> 8));
            zlib.insert(zlib.end(), raw.begin() + offset, raw.begin() + offset + block);
            offset += block;
        } while (offset < raw.size());
        uint32_t a = 1, b = 0;
        for (uint8_t byte : raw)
        {
            a = (a + byte) % 65521;
            b = (b + a) % 65521;
        }
        PutBE32(zlib, (b << 16) | a);

        std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        std::vector<uint8_t> header;
        PutBE32(header, width);
        PutBE32(header, height);
        header.insert(header.end(), { 8, 6, 0, 0, 0 }); // 8-bit RGBA
        Chunk(png, "IHDR", header);
        Chunk(png, "IDAT", zlib);
        Chunk(png, "IEND", {});

        FILE* file = plat::OpenCFile(path, "wb");
        if (!file)
            return false;
        bool ok = fwrite(png.data(), 1, png.size(), file) == png.size();
        fclose(file);
        return ok;
    }
}
