#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// UTF-8 <-> std::wstring without the OS (MultiByteToWideChar / WideCharToMultiByte): wchar_t is
// UTF-16 on Windows and UTF-32 on Linux/Android, and both are handled. Invalid input becomes
// U+FFFD.
namespace plat
{
    inline std::wstring WidenUtf8(std::string_view text)
    {
        std::wstring out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size();)
        {
            uint8_t c = uint8_t(text[i]);
            uint32_t cp = 0xFFFD;
            size_t length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
            if (length == 1)
            {
                cp = c;
            }
            else if (length && i + length <= text.size())
            {
                cp = c & (0xFF >> (length + 1));
                bool ok = true;
                for (size_t k = 1; k < length; k++)
                {
                    uint8_t next = uint8_t(text[i + k]);
                    ok = ok && (next & 0xC0) == 0x80;
                    cp = (cp << 6) | (next & 0x3F);
                }
                if (!ok || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000))
                    cp = 0xFFFD;
            }
            else
            {
                length = 1;
            }
            i += length;
            if constexpr (sizeof(wchar_t) == 2)
            {
                if (cp >= 0x10000)
                {
                    cp -= 0x10000;
                    out += wchar_t(0xD800 + (cp >> 10));
                    out += wchar_t(0xDC00 + (cp & 0x3FF));
                    continue;
                }
            }
            out += wchar_t(cp);
        }
        return out;
    }

    inline std::string NarrowUtf8(std::wstring_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); i++)
        {
            uint32_t cp = uint32_t(text[i]);
            if constexpr (sizeof(wchar_t) == 2)
            {
                if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < text.size() && uint32_t(text[i + 1]) >= 0xDC00 && uint32_t(text[i + 1]) < 0xE000)
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (uint32_t(text[++i]) - 0xDC00);
                else if (cp >= 0xD800 && cp < 0xE000)
                    cp = 0xFFFD;
            }
            if (cp < 0x80)
            {
                out += char(cp);
            }
            else if (cp < 0x800)
            {
                out += char(0xC0 | (cp >> 6));
                out += char(0x80 | (cp & 0x3F));
            }
            else if (cp < 0x10000)
            {
                out += char(0xE0 | (cp >> 12));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
            }
            else
            {
                out += char(0xF0 | (cp >> 18));
                out += char(0x80 | ((cp >> 12) & 0x3F));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
            }
        }
        return out;
    }
}
