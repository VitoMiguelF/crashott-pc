// Blue Snipe shots (runtime/NOTES.md, "Snipe azul atirando rosa").
//
// The Snipe (Parafox internally) has one texture drawn through 256-colour palettes, and two colour
// variants of them: the pink one ("Parafox_normal.bmp" / "Parafox_jacked.bmp") and a blue one
// ("Parafox_normal_A.bmp" / "Parafox_jacked_A.bmp"), the template's normal_skin_1/2 and
// jacked_skin_1/2 (the RenderBehaviour fields; other monsters have up to 4). Each one is picked
// when the character's drawable is set up (CCharacterRenderBehaviour::vf12 sub_82149FF8 ->
// sub_8221F4A0: a random one of the variants that have both palettes, unless the counter at
// 0x824A83CC is positive, which keeps variant 0), so about half the Snipes are blue. The variant is
// the word at drawable +4548; the palettes at +4516 (normal) and +4532 (jacked), 4 each.
//
// The shot is another actor, "Weapons:Projectile:ParafoxProjectile" (package\c4ec64ea.p3d), made
// pink once for every Snipe: a feather mesh with an RGB copy of the Snipe's texture already
// painted with the pink palette ("Parafox.bmp", shader "Parafox_m"), a pink trail
// ("cha_fx_parafox_light", the state prop's effect), a pink impact ("cha_fx_parafox_impact", the
// damager's default hit effect) and a pink light (ProximityLighting colour 255, 47, 151). The blue
// Snipes fire it too: pink shots.
//
// The fix has two halves:
//
//   1. package\c4ec64ea.p3d is rebuilt when default.rcf is opened: every top-level chunk of the
//      feather, the trail and the impact (skeletons, mesh, composites, particle systems,
//      billboards and their animations, the state prop, the texture and its shader) gets a copy
//      right after it, renamed at the same length ("parafox" -> "parablu", "Parafox" -> "Parablu")
//      and recoloured: pink, magenta and purple go to the blue of the Snipe's own "_A" palette
//      (BlueFromPink, measured on the 187 entries where the two palettes differ: hue 255-360 ->
//      203-220, saturation to ~0.6, value kept). The texture's PNGs are decoded, recoloured and
//      written back (stored deflate); the particles' colour keys and the billboards' colour
//      directly. The state prop's sound callbacks (0x8020030) keep the original name. The prop's
//      collision object (0x8020020) and its physics object (0x5000000, the volumes) are copied and
//      renamed too: the game finds them by the state prop's name, and without them the blue shot
//      had only the ground collision of physical_ellipsoid_text (it went through Crash, enemies and
//      the targets that open the way, and burst as if it hit the ground).
//   2. The projectile actors come from the launcher's quiver: CProjectileLauncherBehaviour's
//      LoadProjectile (sub_8212F238) asks its quiver (one per launcher, launcher +288; owner actor
//      at launcher +304) for the next one, sub_8212EE60, which makes a new one (up to the quiver's
//      size, then they are reused in turn) with sub_821363E8(genName, template, owner, ...): the
//      Lua GenerateObject(genName, spawner, ...) -> TemplateActorFunctions[genName](...). While
//      the owner is a blue Snipe (the owner's CCharacterRenderBehaviour at actor +340, its drawable
//      at +92, variant 1), genName becomes "Weapons:Projectile:ParafoxProjectileBlue": a Lua
//      template (defined here on first use) that runs the original one and, after its
//      SetBehaviourData, points the state prop at "cbt_proj_parablu_light", the hit effect at
//      "cha_fx_parablu_impact" and the light at the blue colour. The template hash (r4, which the
//      stream manager uses to find the package) stays the original's. A Snipe keeps its quiver and
//      its variant, so a blue Snipe's reused shots stay blue.
//
// CRASH_SNIPE_COLORS=0 turns it off (the original pink shots). Tests: CRASH_FORCE_SKIN_VARIANT=<n>
// gives every character that has variant n (normal and jacked palettes) that variant instead of
// the random one (1 = the blue Snipe); profile snipe_azul. CRASH_SNIPE_DUMP=<file> writes the
// rebuilt package (check it with tools\p3d.py).

#include "settings.h"
#include "../diagnostics.h"
#include "../dev/dev_tools.h"
#include "../fs/rcf_overlay.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "../platform/image.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_8212EE60);  // CProjectileQuiver: the next projectile (made while the quiver isn't full)
extern "C" PPC_FUNC(__imp__sub_821363E8);  // a generated object: Lua GenerateObject(genName, spawner, ...)
extern "C" PPC_FUNC(__imp__sub_8221F4A0);  // character drawable: picks its colour variant
PPC_EXTERN_FUNC(sub_8222C2D8);             // character drawable (+348): sets the palette

namespace
{
    constexpr const char* PROJECTILE_PACKAGE = "package\\c4ec64ea.p3d";  // Weapons_Projectile_ParafoxProjectile.p3d
    constexpr const char* PINK_TEMPLATE = "Weapons:Projectile:ParafoxProjectile";
    constexpr const char* BLUE_TEMPLATE = "Weapons:Projectile:ParafoxProjectileBlue";

    constexpr uint32_t ROOT = 0xFF443350, TEXTURE = 0x19000, IMAGE_DATA = 0x19002, BILLBOARD_QUAD = 0x17005, COLOUR_CHANNEL = 0x121109,
                       PROP_CALLBACK = 0x8020030, PROP_COLLISION = 0x8020020, PHYSICS_OBJECT = 0x5000000;

    constexpr uint32_t ACTOR_CHARACTER_RENDER = 340;  // actor: its CCharacterRenderBehaviour
    constexpr uint32_t CHARACTER_RENDER_VTABLE = 0x82015BC4;
    constexpr uint32_t RENDER_DRAWABLE = 92;          // CCharacterRenderBehaviour: the character drawable
    constexpr uint32_t DRAWABLE_VARIANT = 4548, DRAWABLE_NORMAL_PALETTES = 4516, DRAWABLE_JACKED_PALETTES = 4532,
                       DRAWABLE_PALETTE_TARGET = 348;
    constexpr uint32_t BLUE_VARIANT = 1;              // normal_skin_2 / jacked_skin_2 = "Parafox_*_A.bmp"

    // Same-length renames of the copies.
    const std::pair<std::string, std::string> RENAMES[] = {
        { "cbt_proj_parafox_light", "cbt_proj_parablu_light" },  // the feather: state prop, composite, skeleton, mesh
        { "cha_fx_parafox_light", "cha_fx_parablu_light" },      // its trail
        { "cha_fx_parafox_impact", "cha_fx_parablu_impact" },    // the impact
        { "Parafox.bmp", "Parablu.bmp" },                        // the feather's texture
        { "Parafox_", "Parablu_" },                              // its shader (Parafox_m) and mip images (Parafox_1.png)
    };
    constexpr uint8_t PINK_LIGHT[3] = { 255, 47, 151 };         // the template's ProximityLighting colour

    std::atomic<bool> g_packageReady{ false };
    thread_local bool g_blueOwner = false;
    uint32_t g_blueTemplateName = 0;  // guest copy of BLUE_TEMPLATE
    int g_forcedVariant = -1;

    struct Chunk
    {
        uint32_t id = 0;
        std::vector<uint8_t> header;  // after the 12-byte chunk header, before the children
        std::vector<Chunk> children;
        std::vector<uint8_t> trailing;
    };

    uint32_t ReadLE32(const uint8_t* p)
    {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }

    void AppendLE32(std::vector<uint8_t>& out, uint32_t value)
    {
        for (int i = 0; i < 4; i++)
            out.push_back(uint8_t(value >> (8 * i)));
    }

    void AppendBE32(std::vector<uint8_t>& out, uint32_t value)
    {
        for (int i = 3; i >= 0; i--)
            out.push_back(uint8_t(value >> (8 * i)));
    }

    // tools/p3d.py: u32 id, u32 header size (12 + own data), u32 total size; children follow.
    bool Parse(const uint8_t* data, size_t offset, size_t end, Chunk& chunk)
    {
        if (offset + 12 > end)
            return false;
        uint32_t headerSize = ReadLE32(data + offset + 4), totalSize = ReadLE32(data + offset + 8);
        if (headerSize < 12 || headerSize > totalSize || offset + totalSize > end)
            return false;
        chunk.id = ReadLE32(data + offset);
        chunk.header.assign(data + offset + 12, data + offset + headerSize);
        size_t at = offset + headerSize, stop = offset + totalSize;
        while (at + 12 <= stop)
        {
            uint32_t childHeader = ReadLE32(data + at + 4), childTotal = ReadLE32(data + at + 8);
            if (childTotal < 12 || childHeader < 12 || childHeader > childTotal || at + childTotal > stop)
                break;
            chunk.children.emplace_back();
            if (!Parse(data, at, stop, chunk.children.back()))
                return false;
            at += childTotal;
        }
        chunk.trailing.assign(data + at, data + stop);
        return true;
    }

    size_t Size(const Chunk& chunk)
    {
        size_t size = 12 + chunk.header.size() + chunk.trailing.size();
        for (const Chunk& child : chunk.children)
            size += Size(child);
        return size;
    }

    void Serialize(const Chunk& chunk, std::vector<uint8_t>& out)
    {
        AppendLE32(out, chunk.id);
        AppendLE32(out, uint32_t(12 + chunk.header.size()));
        AppendLE32(out, uint32_t(Size(chunk)));
        out.insert(out.end(), chunk.header.begin(), chunk.header.end());
        for (const Chunk& child : chunk.children)
            Serialize(child, out);
        out.insert(out.end(), chunk.trailing.begin(), chunk.trailing.end());
    }

    bool Contains(const Chunk& chunk, const std::string& needle)
    {
        if (std::search(chunk.header.begin(), chunk.header.end(), needle.begin(), needle.end()) != chunk.header.end())
            return true;
        return std::any_of(chunk.children.begin(), chunk.children.end(), [&](const Chunk& child) { return Contains(child, needle); });
    }

    void Rename(Chunk& chunk)
    {
        if (chunk.id == PROP_CALLBACK)
            return;  // "zscripts/props.p3d", the prop's name, the event: the sounds of the original
        for (const auto& [from, to] : RENAMES)
        {
            for (auto it = chunk.header.begin(); (it = std::search(it, chunk.header.end(), from.begin(), from.end())) != chunk.header.end();
                 it += from.size())
                std::copy(to.begin(), to.end(), it);
        }
        for (Chunk& child : chunk.children)
            Rename(child);
    }

    // The pink -> blue of the Snipe's two palettes ("Parafox_normal.bmp" -> "Parafox_normal_A.bmp"):
    // over the entries that differ, hue 300-360 goes to ~203 and the purples (255-300) to 203-220,
    // saturation to ~0.6 at most, value about the same. Browns, greys and black stay.
    void BlueFromPink(uint8_t& r, uint8_t& g, uint8_t& b)
    {
        float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
        float maximum = std::max({ rf, gf, bf }), minimum = std::min({ rf, gf, bf }), delta = maximum - minimum;
        if (maximum <= 0.0f || delta <= 0.0f)
            return;
        float saturation = delta / maximum, value = maximum;
        float hue = maximum == rf ? 60.0f * std::fmod((gf - bf) / delta + 6.0f, 6.0f)
                  : maximum == gf ? 60.0f * ((bf - rf) / delta + 2.0f)
                                  : 60.0f * ((rf - gf) / delta + 4.0f);
        if (hue < 30.0f)
            hue += 360.0f;  // the reds just past 0 belong to the pinks
        // How pink: full from 260 to 365, fading over 10 degrees on each side and below saturation 0.25.
        float weight = std::clamp((hue - 250.0f) / 10.0f, 0.0f, 1.0f) * std::clamp((375.0f - hue) / 10.0f, 0.0f, 1.0f) *
                       std::clamp((saturation - 0.12f) / 0.13f, 0.0f, 1.0f);
        if (weight <= 0.0f)
            return;
        float target = 203.0f + std::clamp(300.0f - hue, 0.0f, 45.0f) / 45.0f * 17.0f;  // 180-240: R lowest, B highest
        float s = std::min(saturation * 0.85f, 0.64f), v = value * 0.96f;
        float f = target / 60.0f - std::floor(target / 60.0f);
        float out[3] = { v * (1.0f - s), v * (1.0f - s * f), v };  // HSV sector 3 (180-240)
        float in[3] = { rf, gf, bf };
        uint8_t* channels[3] = { &r, &g, &b };
        for (int i = 0; i < 3; i++)
            *channels[i] = uint8_t(std::lround(std::clamp(in[i] + (out[i] - in[i]) * weight, 0.0f, 1.0f) * 255.0f));
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

    void AppendPngChunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& body)
    {
        AppendBE32(png, uint32_t(body.size()));
        size_t start = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), body.begin(), body.end());
        AppendBE32(png, Crc32(&png[start], png.size() - start));
    }

    // An RGB or RGBA PNG with stored (uncompressed) deflate blocks, as language_font.cpp's EncodePng.
    std::vector<uint8_t> EncodePng(const std::vector<uint8_t>& rgba, int width, int height, bool alpha)
    {
        int channels = alpha ? 4 : 3;
        std::vector<uint8_t> raw;  // filter byte + scanline per row
        raw.reserve(size_t(height) * (1 + size_t(width) * channels));
        for (int y = 0; y < height; y++)
        {
            raw.push_back(0);
            for (int x = 0; x < width; x++)
            {
                const uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
                raw.insert(raw.end(), p, p + channels);
            }
        }
        std::vector<uint8_t> zlib = { 0x78, 0x01 };
        for (size_t pos = 0;;)
        {
            size_t length = std::min<size_t>(raw.size() - pos, 65535);
            bool last = pos + length >= raw.size();
            zlib.push_back(last ? 1 : 0);
            zlib.push_back(uint8_t(length)), zlib.push_back(uint8_t(length >> 8));
            zlib.push_back(uint8_t(~length)), zlib.push_back(uint8_t(~length >> 8));
            zlib.insert(zlib.end(), raw.begin() + pos, raw.begin() + pos + length);
            pos += length;
            if (last)
                break;
        }
        uint32_t a = 1, b = 0;
        for (uint8_t byte : raw)
            a = (a + byte) % 65521, b = (b + a) % 65521;
        AppendBE32(zlib, (b << 16) | a);

        std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        std::vector<uint8_t> header;
        AppendBE32(header, uint32_t(width));
        AppendBE32(header, uint32_t(height));
        header.insert(header.end(), { 8, uint8_t(alpha ? 6 : 2), 0, 0, 0 });
        AppendPngChunk(png, "IHDR", header);
        AppendPngChunk(png, "IDAT", zlib);
        AppendPngChunk(png, "IEND", {});
        return png;
    }

    // An image data chunk (u32 size + the file): a truecolour PNG decoded, recoloured, re-encoded.
    bool RecolorImage(Chunk& data)
    {
        static const uint8_t SIGNATURE[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        if (data.header.size() < 4 + 33)
            return false;
        uint32_t size = ReadLE32(data.header.data());
        const uint8_t* png = data.header.data() + 4;
        if (4 + size_t(size) > data.header.size() || memcmp(png, SIGNATURE, 8) || memcmp(png + 12, "IHDR", 4))
            return false;
        uint8_t depth = png[24], colourType = png[25];
        if (depth != 8 || (colourType != 2 && colourType != 6))
            return false;  // only the 8-bit RGB / RGBA ones (the feather's texture and its mips)
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
        if (!plat::DecodePng(png, size, width, height, rgba) || rgba.size() != size_t(width) * height * 4)
            return false;
        for (size_t i = 0; i < rgba.size(); i += 4)
            BlueFromPink(rgba[i], rgba[i + 1], rgba[i + 2]);
        std::vector<uint8_t> encoded = EncodePng(rgba, width, height, colourType == 6);
        std::vector<uint8_t> header;
        AppendLE32(header, uint32_t(encoded.size()));
        header.insert(header.end(), encoded.begin(), encoded.end());
        header.insert(header.end(), data.header.begin() + 4 + size, data.header.end());
        data.header.swap(header);
        return true;
    }

    // The particles' colour keys and the billboards' colour (as mojo_colors.cpp), the texture's PNGs.
    void Recolor(Chunk& chunk, int& colours, int& images)
    {
        std::vector<uint8_t>& h = chunk.header;
        if (chunk.id == COLOUR_CHANNEL && h.size() >= 12)
        {
            // [version][tag: "CLR\0", "COOL"...][u32 keys][u16 frame per key][colour B,G,R,A per key]
            uint32_t keys = ReadLE32(&h[8]);
            if (keys && 12 + size_t(keys) * 6 <= h.size())
            {
                uint8_t* keyColours = &h[h.size() - size_t(keys) * 4];
                for (uint32_t k = 0; k < keys; k++, colours++)
                    BlueFromPink(keyColours[k * 4 + 2], keyColours[k * 4 + 1], keyColours[k * 4]);
            }
        }
        else if (chunk.id == BILLBOARD_QUAD)
        {
            // [version][name][...]["AAX\0"][colour B,G,R,A][width][height]...
            static const std::string AXIS = std::string("AAX") + '\0';
            auto axis = std::search(h.begin(), h.end(), AXIS.begin(), AXIS.end());
            if (axis != h.end() && h.end() - axis >= 8)
            {
                BlueFromPink(axis[6], axis[5], axis[4]);
                colours++;
            }
        }
        else if (chunk.id == IMAGE_DATA)
            images += RecolorImage(chunk);
        for (Chunk& child : chunk.children)
            Recolor(child, colours, images);
    }

    bool RebuildProjectilePackage(std::vector<uint8_t>& data)
    {
        Chunk root;
        if (data.size() < 12 || ReadLE32(data.data()) != ROOT || ReadLE32(data.data() + 8) != data.size() ||
            !Parse(data.data(), 0, data.size(), root) || !root.trailing.empty())
            return false;

        std::vector<Chunk> children;
        int copies = 0, colours = 0, images = 0, textures = 0, collisions = 0;
        for (const Chunk& chunk : root.children)
        {
            bool copied = std::any_of(std::begin(RENAMES), std::end(RENAMES), [&](const auto& rename) { return Contains(chunk, rename.first); });
            children.push_back(chunk);
            if (!copied)
                continue;
            Chunk copy = chunk;
            Rename(copy);
            Recolor(copy, colours, images);
            textures += copy.id == TEXTURE;
            collisions += copy.id == PROP_COLLISION || copy.id == PHYSICS_OBJECT;
            children.push_back(std::move(copy));
            copies++;
        }
        // Feather (skeleton, mesh, composite, state prop, its collision and physics objects), trail
        // (skeleton, particles, composite), impact (2 animations, skeleton, particles, 2 billboard
        // groups, composite), texture, shader.
        if (copies < 12 || collisions != 2 || textures != 1 || images == 0 || colours == 0)
        {
            Log("[settings] blue Snipe shots: %s has %d chunks to copy (%d textures, %d collision objects, %d images, %d colours); left unchanged",
                PROJECTILE_PACKAGE, copies, textures, collisions, images, colours);
            return false;
        }
        root.children.swap(children);
        std::vector<uint8_t> out;
        out.reserve(data.size() * 2);
        Serialize(root, out);
        Log("[settings] blue Snipe shots: %s %zu -> %zu bytes, %d chunks copied (%d colours, %d images recoloured)", PROJECTILE_PACKAGE,
            data.size(), out.size(), copies, colours, images);
        if (const char* dump = getenv("CRASH_SNIPE_DUMP"))
        {
            if (FILE* file = fopen(dump, "wb"))
            {
                fwrite(out.data(), 1, out.size(), file);
                fclose(file);
            }
        }
        data.swap(out);
        g_packageReady = true;
        return true;
    }

    bool Plausible(uint32_t pointer)
    {
        return (pointer & 3) == 0 && ((pointer >= 0x40000000 && pointer < 0x7F000000) || (pointer >= 0xA0000000 && pointer < 0xC0000000));
    }

    // The character drawable of an actor (its CCharacterRenderBehaviour's), or 0.
    uint32_t CharacterDrawable(uint32_t actor)
    {
        if (!Plausible(actor))
            return 0;
        uint32_t behaviour = LoadBE32(actor + ACTOR_CHARACTER_RENDER);
        if (!Plausible(behaviour) || LoadBE32(behaviour) != CHARACTER_RENDER_VTABLE)
            return 0;
        uint32_t drawable = LoadBE32(behaviour + RENDER_DRAWABLE);
        return Plausible(drawable) ? drawable : 0;
    }

    bool GuestStringIs(uint32_t address, const char* text)
    {
        if (!Plausible(address) && !(address >= 0x82000000 && address < 0x83000000))
            return false;
        size_t size = strlen(text) + 1;
        return memcmp(g_memory.Translate(address), text, size) == 0;
    }

    // TemplateActorFunctions[BLUE_TEMPLATE]: the original template with the blue feather, impact and
    // light. newActor (a global the template calls) is wrapped for that one call, to run the
    // template's SetBehaviourData first and change the three fields after it.
    bool DefineBlueTemplate(PPCContext& ctx, uint8_t* base)
    {
        uint8_t light[3] = { PINK_LIGHT[0], PINK_LIGHT[1], PINK_LIGHT[2] };
        BlueFromPink(light[0], light[1], light[2]);
        char code[2048];
        snprintf(code, sizeof(code),
                 "local t = TemplateActorFunctions\n"
                 "local pink = t['%s']\n"
                 "if not pink then error('no template %s') end\n"
                 "if not t['%s'] then\n"
                 "  t['%s'] = function(parent, name, position, rotation, scale)\n"
                 "    local real = newActor\n"
                 "    newActor = function(class, p, n, pos, rot, sc, setData, templateName)\n"
                 "      newActor = real\n"
                 "      return real(class, p, n, pos, rot, sc, function(bc)\n"
                 "        setData(bc)\n"
                 "        bc:GetBehaviourByName('m_StatePropBehaviour').drawable_entity = 'cbt_proj_parablu_light'\n"
                 "        bc:GetBehaviourByName('m_PhysicsBehaviour').physical_ellipsoid_text = 'cbt_proj_parablu_light'\n"
                 "        bc:GetBehaviourByName('m_IntersectionBehaviour').intersection_volume_text = 'cbt_proj_parablu_light'\n"
                 "        bc:GetBehaviourByName('m_DamagerBehaviour').default_hit_effect_entity = 'cha_fx_parablu_impact'\n"
                 "        bc:GetBehaviourByName('m_ProximityLightingBehaviour').colour_colour = tovector(%d, %d, %d)\n"
                 "      end, templateName)\n"
                 "    end\n"
                 "    local ok, actor, bc = pcall(pink, parent, name, position, rotation, scale)\n"
                 "    newActor = real\n"
                 "    if not ok then error(actor) end\n"
                 "    return actor, bc\n"
                 "  end\n"
                 "end\n",
                 PINK_TEMPLATE, PINK_TEMPLATE, BLUE_TEMPLATE, BLUE_TEMPLATE, light[0], light[1], light[2]);
        return dev::RunLua(ctx, base, code, "snipe_colors");
    }
}

namespace settings
{
    void RegisterSnipePatches()
    {
        if (const char* forced = getenv("CRASH_FORCE_SKIN_VARIANT"))
        {
            g_forcedVariant = std::clamp(atoi(forced), 0, 3);
            Log("[settings] CRASH_FORCE_SKIN_VARIANT=%d: every character with that colour variant takes it", g_forcedVariant);
        }
        const char* off = getenv("CRASH_SNIPE_COLORS");
        if (off && strcmp(off, "0") == 0)
        {
            Log("[settings] blue Snipe shots: off (CRASH_SNIPE_COLORS=0)");
            return;
        }
        rcf_overlay::RegisterEntryRewrite("default", PROJECTILE_PACKAGE, "tiro azul do Snipe azul", RebuildProjectilePackage);
    }
}

// The quiver's next projectile: r4 = the launcher's owner (actor).
PPC_FUNC(sub_8212EE60)
{
    uint32_t drawable = g_packageReady ? CharacterDrawable(ctx.r4.u32) : 0;
    g_blueOwner = drawable && LoadBE32(drawable + DRAWABLE_VARIANT) == BLUE_VARIANT;
    __imp__sub_8212EE60(ctx, base);
    g_blueOwner = false;
}

// A new generated object: r3 = genName (the template's name), r4 = the template (stream manager).
PPC_FUNC(sub_821363E8)
{
    if (g_blueOwner && GuestStringIs(ctx.r3.u32, PINK_TEMPLATE))
    {
        g_blueOwner = false;
        if (!g_blueTemplateName)
        {
            g_blueTemplateName = GuestAlloc(64);
            if (g_blueTemplateName)
                memcpy(g_memory.Translate(g_blueTemplateName), BLUE_TEMPLATE, strlen(BLUE_TEMPLATE) + 1);
        }
        static bool logged = false;
        if (g_blueTemplateName && DefineBlueTemplate(ctx, base))
        {
            if (!logged)
                Log("[settings] blue Snipe shots: a blue Snipe's quiver makes %s", BLUE_TEMPLATE);
            logged = true;
            ctx.r3.u64 = g_blueTemplateName;
        }
    }
    __imp__sub_821363E8(ctx, base);
}

// Tests: CRASH_FORCE_SKIN_VARIANT=<n> after the game's own pick (r3 = the character drawable).
PPC_FUNC(sub_8221F4A0)
{
    uint32_t drawable = ctx.r3.u32;
    __imp__sub_8221F4A0(ctx, base);
    if (g_forcedVariant < 0 || !Plausible(drawable))
        return;
    uint32_t normal = LoadBE32(drawable + DRAWABLE_NORMAL_PALETTES + 4 * g_forcedVariant);
    uint32_t jacked = LoadBE32(drawable + DRAWABLE_JACKED_PALETTES + 4 * g_forcedVariant);
    if (!Plausible(normal) || !Plausible(jacked))
        return;
    StoreBE32(drawable + DRAWABLE_VARIANT, uint32_t(g_forcedVariant));
    PPCContext call = ctx;
    call.r3.u64 = drawable + DRAWABLE_PALETTE_TARGET;
    call.r4.u64 = normal + 16;
    sub_8222C2D8(call, base);
}
