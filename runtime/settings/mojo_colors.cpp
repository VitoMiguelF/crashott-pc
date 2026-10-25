// "PS2" mojo colours (settings: [game] mojo_colors = "ps2" | "xbox360"; see runtime/NOTES.md,
// "Cores do mojo").
//
// On the Xbox 360 every mojo size looks the same. The mojos the game draws come from
// package\PermanentContent.p3d (default.rcf), where the four meshes mojoSShape, mojoMShape,
// mojoLShape and mojoXLShape all use one shader, "c_mojo_m" (spheremap), with one 256x256 8-bit
// texture, "c_mojo_256.tga.0" (a palettized PNG), and four 256-colour palettes
// "c_mojo_256.tga.0".."3" (chunk 0x19006 + 0x19007 with 1024 bytes of B,G,R,A) that the
// "TEX_c_mojo_m" PALA animation cycles (palette switching): a blue/cyan shimmer. The PS2 version
// shows the large mojo a very light, brighter blue and the extra large one lilac purple. (package\Collectables_c_mojo*.p3d hold the same
// material once per size, also blue, but the objects of PermanentContent are the ones used.)
//
// With "ps2" the package is rebuilt on load: the shared material (the 4 palettes, the texture, the
// shader, the PALA animation and its controller, chunk 0x121201) is copied once per recoloured
// size, right after the original, with the palettes and the PNG's PLTE recoloured (HSV: hue replaced
// or kept, saturation and value scaled) and the names made unique at the same length: "c_mojo_256.tga" ->
// "c_mojo_256.tgN" and "c_mojo_m" -> "c_mojo_N" (also "TEX_c_mojo_m"; N = size digit). The halo
// effect every mojo drawable attaches ("c_mojo360glow": animation, skeleton, billboard group,
// composite, scene) is copied the same way ("360glow" -> "360gloN"), with its additive dark blue
// (quad colour after "AAX\0" and the CLR channel keys) recoloured too. The size's mesh and its
// drawable (0x802000a) are renamed the same way, so only that size uses the copies:
//
//   S, M -> blue (unchanged), L -> very light blue (brighter, also its halo), XL -> lilac purple
//
// (Checked by the user against the PS2: the first level has only blue and purple mojos; the gold one
// only shows up later in the game and needs no change.)
//
// The objects the game looks up ("c_mojoL", "c_mojoLCollectableShape", ...) keep their names.
// Applied when default.rcf is opened, so the option takes effect on the next launch.

#include "settings.h"
#include "../diagnostics.h"
#include "../fs/rcf_overlay.h"
#include "../platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace settings
{
    namespace
    {
        struct Recolor
        {
            const char* size;    // "S", "M", "L", "XL"
            const char* mesh;    // "mojoLShape"
            char digit;          // makes the copied names unique
            bool enabled;        // false = the original blue material
            float hue;           // degrees; < 0 = each colour keeps its own hue
            float saturation;    // factor
            float value;         // factor
        };

        constexpr float KEEP = -1.0f, PURPLE = 258.0f;  // lilac, matched to a PS2 capture (hue ~262, sat ~0.35)
        constexpr Recolor PS2_COLORS[] = {
            { "S", "mojoSShape", '1', false, KEEP, 1.0f, 1.0f },
            { "M", "mojoMShape", '2', false, KEEP, 1.0f, 1.0f },
            { "L", "mojoLShape", '3', true, KEEP, 0.60f, 1.45f },     // very light blue, brighter
            { "XL", "mojoXLShape", '4', true, PURPLE, 0.60f, 1.05f },  // lilac purple
        };

        constexpr const char* PERMANENT_CONTENT = "package\\bca1bc0b.p3d";  // package\PermanentContent.p3d
        constexpr uint32_t ROOT = 0xFF443350, MESH = 0x10000, DRAWABLE = 0x802000A, PALETTE = 0x19006, PALETTE_DATA = 0x19007,
                           IMAGE_DATA = 0x19002, BILLBOARD_QUAD = 0x17005, COLOUR_CHANNEL = 0x121109;
        // The halo: "c_mojo360glow" (skeleton, scene, composite), "c_mojo360glowShape" (billboard
        // group), "c_mojo_360glowBBShape" (the quad) and "BQG_c_mojo360glowShape" (its animation).
        const std::string TEXTURE_NAME = "c_mojo_256.tga", SHADER_NAME = "c_mojo_m", GLOW_NAME = "360glow";
        constexpr int MATERIAL_CHUNKS = 8, GLOW_CHUNKS = 5;

        std::vector<Recolor> g_recolors;

        uint32_t LoadLE32(const uint8_t* p)
        {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }

        void StoreLE32(uint8_t* p, uint32_t value)
        {
            p[0] = uint8_t(value);
            p[1] = uint8_t(value >> 8);
            p[2] = uint8_t(value >> 16);
            p[3] = uint8_t(value >> 24);
        }

        uint32_t LoadBE32(const uint8_t* p)
        {
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }

        void StoreBE32(uint8_t* p, uint32_t value)
        {
            p[0] = uint8_t(value >> 24);
            p[1] = uint8_t(value >> 16);
            p[2] = uint8_t(value >> 8);
            p[3] = uint8_t(value);
        }

        uint32_t Crc32(const uint8_t* data, size_t size)
        {
            static uint32_t table[256];
            static bool ready = false;
            if (!ready)
            {
                for (uint32_t i = 0; i < 256; i++)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; k++)
                        c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    table[i] = c;
                }
                ready = true;
            }
            uint32_t crc = 0xFFFFFFFFu;
            for (size_t i = 0; i < size; i++)
                crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return crc ^ 0xFFFFFFFFu;
        }

        // One colour (0..255 per channel) with the hue replaced (or kept) and saturation/value scaled.
        void Shift(uint8_t& r, uint8_t& g, uint8_t& b, const Recolor& recolor)
        {
            float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
            float maximum = std::max({ rf, gf, bf }), minimum = std::min({ rf, gf, bf });
            float value = maximum, saturation = maximum > 0.0f ? (maximum - minimum) / maximum : 0.0f;
            saturation = std::clamp(saturation * recolor.saturation, 0.0f, 1.0f);
            value = std::clamp(value * recolor.value, 0.0f, 1.0f);
            float hue = recolor.hue;
            if (hue < 0.0f)
            {
                float delta = maximum - minimum;
                hue = delta <= 0.0f ? 0.0f
                    : maximum == rf ? 60.0f * std::fmod((gf - bf) / delta + 6.0f, 6.0f)
                    : maximum == gf ? 60.0f * ((bf - rf) / delta + 2.0f)
                                    : 60.0f * ((rf - gf) / delta + 4.0f);
            }
            float h = std::fmod(hue, 360.0f) / 60.0f;
            int sector = int(h) % 6;
            float f = h - std::floor(h);
            float p = value * (1.0f - saturation), q = value * (1.0f - saturation * f), t = value * (1.0f - saturation * (1.0f - f));
            float out[3];
            switch (sector)
            {
            case 0: out[0] = value, out[1] = t, out[2] = p; break;
            case 1: out[0] = q, out[1] = value, out[2] = p; break;
            case 2: out[0] = p, out[1] = value, out[2] = t; break;
            case 3: out[0] = p, out[1] = q, out[2] = value; break;
            case 4: out[0] = t, out[1] = p, out[2] = value; break;
            default: out[0] = value, out[1] = p, out[2] = q; break;
            }
            r = uint8_t(std::lround(out[0] * 255.0f));
            g = uint8_t(std::lround(out[1] * 255.0f));
            b = uint8_t(std::lround(out[2] * 255.0f));
        }

        // The PLTE of a palettized PNG (the texture's own copy of palette 0).
        bool RecolorPng(uint8_t* png, size_t size, const Recolor& recolor)
        {
            static const uint8_t SIGNATURE[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
            if (size < 8 || memcmp(png, SIGNATURE, 8))
                return false;
            for (size_t at = 8; at + 12 <= size;)
            {
                uint32_t length = LoadBE32(png + at);
                if (at + 12 + size_t(length) > size)
                    return false;
                if (!memcmp(png + at + 4, "PLTE", 4))
                {
                    uint8_t* entries = png + at + 8;
                    for (uint32_t i = 0; i + 3 <= length; i += 3)
                        Shift(entries[i], entries[i + 1], entries[i + 2], recolor);
                    StoreBE32(png + at + 8 + length, Crc32(png + at + 4, size_t(length) + 4));
                    return true;
                }
                if (!memcmp(png + at + 4, "IEND", 4))
                    return false;
                at += 12 + size_t(length);
            }
            return false;
        }

        // Recolours the palettes and PNG palettes inside [begin, end) (a chunk's children).
        void RecolorChunks(uint8_t* data, size_t begin, size_t end, const Recolor& recolor, int& palettes, int& pngs)
        {
            size_t offset = begin;
            while (offset + 12 <= end)
            {
                uint32_t id = LoadLE32(data + offset), headerSize = LoadLE32(data + offset + 4), totalSize = LoadLE32(data + offset + 8);
                if (totalSize < 12 || headerSize < 12 || headerSize > totalSize || offset + totalSize > end)
                    return;
                if (id == PALETTE_DATA && headerSize == 12 + 1024)
                {
                    uint8_t* entries = data + offset + 12;
                    for (int i = 0; i < 256; i++)
                        Shift(entries[i * 4 + 2], entries[i * 4 + 1], entries[i * 4], recolor);  // B, G, R, A
                    palettes++;
                }
                else if (id == IMAGE_DATA && headerSize >= 16)
                {
                    uint32_t size = LoadLE32(data + offset + 12);
                    if (16 + size_t(size) <= headerSize && RecolorPng(data + offset + 16, size, recolor))
                        pngs++;
                }
                else if (id == BILLBOARD_QUAD)
                {
                    // [version][name][...]["AAX\0"][colour B,G,R,A][width][height]...
                    static const std::string AXIS = std::string("AAX") + '\0';
                    uint8_t* header = data + offset + 12;
                    uint8_t* axis = std::search(header, data + offset + headerSize, AXIS.begin(), AXIS.end());
                    if (axis + 8 <= data + offset + headerSize)
                        Shift(axis[6], axis[5], axis[4], recolor);
                }
                else if (id == COLOUR_CHANNEL && headerSize >= 12 + 12)
                {
                    // [version]["CLR\0"][u32 keys][u16 frame per key][colour B,G,R,A per key]
                    uint32_t keys = LoadLE32(data + offset + 20);
                    if (keys && 12 + 12 + size_t(keys) * 6 <= headerSize)
                    {
                        uint8_t* colours = data + offset + headerSize - size_t(keys) * 4;
                        for (uint32_t k = 0; k < keys; k++)
                            Shift(colours[k * 4 + 2], colours[k * 4 + 1], colours[k * 4], recolor);
                    }
                }
                RecolorChunks(data, offset + headerSize, offset + totalSize, recolor, palettes, pngs);
                offset += totalSize;
            }
        }

        bool Contains(const uint8_t* data, size_t size, const std::string& needle)
        {
            return std::search(data, data + size, needle.begin(), needle.end()) != data + size;
        }

        int ReplaceAll(uint8_t* data, size_t size, const std::string& from, const std::string& to)
        {
            int count = 0;
            for (uint8_t* it = data; (it = std::search(it, data + size, from.begin(), from.end())) != data + size; it += from.size())
            {
                std::copy(to.begin(), to.end(), it);
                count++;
            }
            return count;
        }

        void Rename(uint8_t* data, size_t size, const Recolor& recolor)
        {
            ReplaceAll(data, size, TEXTURE_NAME, TEXTURE_NAME.substr(0, TEXTURE_NAME.size() - 1) + recolor.digit);
            ReplaceAll(data, size, SHADER_NAME, SHADER_NAME.substr(0, SHADER_NAME.size() - 1) + recolor.digit);
            ReplaceAll(data, size, GLOW_NAME, GLOW_NAME.substr(0, GLOW_NAME.size() - 1) + recolor.digit);
        }

        bool RebuildPermanentContent(std::vector<uint8_t>& data)
        {
            if (data.size() < 12 || LoadLE32(data.data()) != ROOT || LoadLE32(data.data() + 4) != 12 || LoadLE32(data.data() + 8) != data.size())
                return false;

            struct Top { size_t offset, size; bool shared; const Recolor* owner; };
            std::vector<Top> chunks;
            int shared = 0, glows = 0, palettes = 0;
            for (size_t offset = 12; offset < data.size();)
            {
                if (offset + 12 > data.size())
                    return false;
                uint32_t id = LoadLE32(&data[offset]), size = LoadLE32(&data[offset + 8]);
                if (size < 12 || offset + size > data.size())
                    return false;
                const uint8_t* chunk = &data[offset];
                Top top{ offset, size, false, nullptr };
                bool material = Contains(chunk, size, TEXTURE_NAME) || Contains(chunk, size, SHADER_NAME);
                bool glow = !material && Contains(chunk, size, GLOW_NAME);
                if ((material || glow) && id != MESH && id != DRAWABLE)
                {
                    top.shared = true;
                    (material ? shared : glows)++;
                    palettes += id == PALETTE;
                }
                else if (material)
                {
                    // A mesh (or its drawable) of a recoloured size: "mojoLShape" + the name's NUL.
                    for (const Recolor& recolor : g_recolors)
                    {
                        if (recolor.enabled && Contains(chunk, size, std::string(recolor.mesh) + '\0'))
                            top.owner = &recolor;
                    }
                }
                chunks.push_back(top);
                offset += size;
            }
            // Material: 4 palettes, the texture, the shader, the animation and its controller.
            // Halo: animation, skeleton, billboard group, composite and scene.
            if (shared != MATERIAL_CHUNKS || palettes != 4 || glows != GLOW_CHUNKS)
            {
                Log("[settings] mojo colours: PermanentContent has %d shared mojo chunks (%d palettes) and %d halo chunks, expected %d (4) "
                    "and %d; left unchanged", shared, palettes, glows, MATERIAL_CHUNKS, GLOW_CHUNKS);
                return false;
            }

            std::vector<uint8_t> out(data.begin(), data.begin() + 12);
            out.reserve(data.size() + 64 * 1024);
            for (const Top& top : chunks)
            {
                size_t at = out.size();
                out.insert(out.end(), data.begin() + top.offset, data.begin() + top.offset + top.size);
                if (top.owner)
                    Rename(&out[at], top.size, *top.owner);
                if (!top.shared)
                    continue;
                for (const Recolor& recolor : g_recolors)
                {
                    if (!recolor.enabled)
                        continue;
                    size_t copy = out.size();
                    out.insert(out.end(), data.begin() + top.offset, data.begin() + top.offset + top.size);
                    Rename(&out[copy], top.size, recolor);
                    int recoloured = 0, pngs = 0;
                    uint32_t headerSize = LoadLE32(&out[copy + 4]);
                    RecolorChunks(&out[copy], headerSize, top.size, recolor, recoloured, pngs);
                }
            }
            StoreLE32(&out[8], uint32_t(out.size()));

            std::string sizes;
            for (const Recolor& recolor : g_recolors)
            {
                if (recolor.enabled)
                    sizes += std::string(sizes.empty() ? "" : ", ") + recolor.size + " hue " +
                             (recolor.hue < 0.0f ? std::string("kept") : std::to_string(int(recolor.hue))) + " sat x" +
                             std::to_string(recolor.saturation).substr(0, 4) + " val x" + std::to_string(recolor.value).substr(0, 4);
            }
            Log("[settings] mojo colours: PermanentContent %zu -> %zu bytes, %d material + %d halo chunks copied for %s", data.size(), out.size(), shared, glows,
                sizes.c_str());
            // Debugging: CRASH_MOJO_DUMP=<file> writes the rebuilt package (check with tools\p3d.py).
            if (const char* dump = getenv("CRASH_MOJO_DUMP"))
            {
                if (FILE* file = fopen(dump, "wb"))
                {
                    fwrite(out.data(), 1, out.size(), file);
                    fclose(file);
                }
            }
            data.swap(out);
            return true;
        }
    }

    void RegisterMojoPatches()
    {
        g_recolors.assign(std::begin(PS2_COLORS), std::end(PS2_COLORS));
        if (!AtStartup().ps2MojoColors)
        {
            for (Recolor& recolor : g_recolors)
                recolor.enabled = false;
        }
        // Debugging: CRASH_MOJO_COLORS="S=290,M=44" sets the hue of those sizes (the others stay
        // blue), whatever game.mojo_colors says.
        if (const char* list = getenv("CRASH_MOJO_COLORS"))
        {
            for (Recolor& recolor : g_recolors)
                recolor.enabled = false;
            std::string text = list;
            for (size_t begin = 0; begin <= text.size();)
            {
                size_t end = std::min(text.find(',', begin), text.size());
                std::string item = text.substr(begin, end - begin);
                size_t equals = item.find('=');
                for (Recolor& recolor : g_recolors)
                {
                    if (equals != std::string::npos && plat::CompareNoCase(item.substr(0, equals).c_str(), recolor.size) == 0)
                    {
                        recolor.enabled = true;
                        recolor.hue = float(atof(item.c_str() + equals + 1));
                        recolor.saturation = 1.15f;
                        recolor.value = 1.0f;
                    }
                }
                begin = end + 1;
            }
            Log("[settings] mojo colours: CRASH_MOJO_COLORS=%s", list);
        }

        if (std::none_of(g_recolors.begin(), g_recolors.end(), [](const Recolor& r) { return r.enabled; }))
        {
            Log("[settings] mojo colours: Xbox 360 (all sizes blue)");
            return;
        }
        rcf_overlay::RegisterEntryRewrite("default", PERMANENT_CONTENT, "cores do mojo (PS2)", RebuildPermanentContent);
    }
}
