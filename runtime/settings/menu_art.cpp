#include "menu_art.h"
#include "language_font.h"
#include "prompt_icons.h"
#include "../diagnostics.h"
#include "../fs/vfs.h"
#include "../gpu/overlay_font.h"
#include "../platform/platform.h"

#include "../platform/file.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <tuple>
#include <map>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;

namespace settings::menu_art
{
    namespace
    {
        constexpr uint32_t FONT = 0x22000, GLYPHS = 0x22001, TEXTURE = 0x19000, IMAGE = 0x19001, IMAGE_DATA = 0x19002, SPRITE = 0x19005;
        // Alpha of the glyph outline in the font pages (fill 254, outline 104-106).
        constexpr int OUTLINE_ALPHA = 105;
        constexpr int FILL_ALPHA = 250;

        uint32_t LE32(const uint8_t* p) { return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
        uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
        float LEF(const uint8_t* p)
        {
            uint32_t bits = LE32(p);
            float value;
            memcpy(&value, &bits, 4);
            return value;
        }

        std::string Lower(std::string text)
        {
            for (char& c : text)
                c = char(tolower(uint8_t(c)));
            return text;
        }

        // default.rcf (tools/rcf.py): entry name -> (offset, size) ------------------------------

        struct Rcf
        {
            plat::File file;
            std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> entries;
        };

        bool ReadAt(const plat::File& file, uint64_t offset, void* data, uint32_t size)
        {
            return file.ReadExactAt(offset, data, size);
        }

        bool OpenRcf(Rcf& rcf)
        {
            auto path = vfs::Parse("D:\\default.rcf");
            auto location = path ? vfs::Locate(*path) : std::nullopt;
            if (!location)
                return false;
            if (!rcf.file.Open(location->host))
                return false;
            uint8_t header[0x3C];
            if (!ReadAt(rcf.file, 0, header, sizeof(header)) || memcmp(header, "ATG CORE CEMENT LIBRARY", 23) != 0)
                return false;
            uint32_t dirOffset = BE32(header + 0x24), namesOffset = BE32(header + 0x2C), namesSize = BE32(header + 0x30), count = BE32(header + 0x38);
            std::vector<uint8_t> directory(size_t(count) * 12), names(namesSize);
            if (!ReadAt(rcf.file, dirOffset, directory.data(), uint32_t(directory.size())) || !ReadAt(rcf.file, namesOffset, names.data(), namesSize))
                return false;
            // Names are listed in data-offset order.
            std::vector<std::pair<uint32_t, uint32_t>> byOffset(count);
            for (uint32_t i = 0; i < count; i++)
                byOffset[i] = { BE32(&directory[i * 12 + 4]), BE32(&directory[i * 12 + 8]) };
            std::sort(byOffset.begin(), byOffset.end());
            size_t pos = 12;
            for (uint32_t i = 0; i < count && pos + 12 <= names.size(); i++)
            {
                uint32_t length = LE32(&names[pos + 8]);
                pos += 12;
                if (pos + length > names.size())
                    break;
                std::string name(reinterpret_cast<const char*>(&names[pos]), strnlen(reinterpret_cast<const char*>(&names[pos]), length));
                rcf.entries[Lower(name)] = byOffset[i];
                pos += length + 3 + 4;
            }
            return true;
        }

        // The entry as the game would see it: a mod's copy (rcf_overlay.h) or the original.
        bool ReadEntry(Rcf& rcf, const std::string& entry, std::vector<uint8_t>& data, std::string& source)
        {
            for (const vfs::ModInfo& mod : vfs::Mods())
            {
                fs::path loose = mod.root / "rcf" / "default" / fs::path(entry);
                std::error_code error;
                if (!fs::is_regular_file(loose, error))
                    continue;
                std::ifstream file(loose, std::ios::binary);
                data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
                source = "mod " + mod.name;
                return !data.empty();
            }
            auto it = rcf.entries.find(Lower(entry));
            if (!rcf.file.IsOpen() || it == rcf.entries.end())
                return false;
            data.resize(it->second.second);
            source = "default.rcf";
            return ReadAt(rcf.file, it->second.first, data.data(), uint32_t(data.size()));
        }

        // Pure3D chunks (tools/p3d.py) -----------------------------------------------------------

        struct Chunk
        {
            uint32_t id;
            size_t offset, header, size;
        };

        std::vector<Chunk> Children(const uint8_t* data, size_t begin, size_t end)
        {
            std::vector<Chunk> children;
            for (size_t offset = begin; offset + 12 <= end;)
            {
                uint32_t id = LE32(data + offset), header = LE32(data + offset + 4), size = LE32(data + offset + 8);
                if (size < 12 || header < 12 || header > size || offset + size > end)
                    break;
                children.push_back({ id, offset, header, size });
                offset += size;
            }
            return children;
        }

        // Every chunk under [begin, end), depth first.
        template <typename F>
        void Walk(const uint8_t* data, size_t begin, size_t end, F&& visit)
        {
            for (const Chunk& chunk : Children(data, begin, end))
            {
                visit(chunk);
                Walk(data, chunk.offset + chunk.header, chunk.offset + chunk.size, visit);
            }
        }

        // Pascal-style name at `p` (length byte, characters, NUL padding).
        std::string Pascal(const uint8_t* p, size_t limit)
        {
            size_t length = std::min<size_t>(p[0], limit ? limit - 1 : 0);
            return std::string(reinterpret_cast<const char*>(p + 1), strnlen(reinterpret_cast<const char*>(p + 1), length));
        }

        bool Premultiplied(const std::vector<uint8_t>& rgba, int w, int h, Image& image)
        {
            image.w = w, image.h = h;
            image.px.resize(size_t(w) * h);
            for (size_t i = 0; i < image.px.size(); i++)
            {
                const uint8_t* p = &rgba[i * 4];
                uint32_t a = p[3];
                uint32_t r = (p[0] * a + 127) / 255, g = (p[1] * a + 127) / 255, b = (p[2] * a + 127) / 255;
                image.px[i] = r | (g << 8) | (b << 16) | (a << 24);
            }
            return true;
        }

        // The PNG of the first image of chunk `owner` (a texture or a sprite), decoded.
        bool OwnerImage(const uint8_t* data, const Chunk& owner, Image& image)
        {
            for (const Chunk& child : Children(data, owner.offset + owner.header, owner.offset + owner.size))
            {
                if (child.id != IMAGE)
                    continue;
                for (const Chunk& part : Children(data, child.offset + child.header, child.offset + child.size))
                {
                    if (part.id != IMAGE_DATA || part.header < 16)
                        continue;
                    uint32_t size = LE32(data + part.offset + 12);
                    if (part.offset + 16 + size > part.offset + part.size)
                        return false;
                    int w, h;
                    std::vector<uint8_t> rgba;
                    return language_font::DecodePng(data + part.offset + 16, size, w, h, rgba) && Premultiplied(rgba, w, h, image);
                }
                return false;
            }
            return false;
        }

        Image Crop(const Image& image, int x, int y, int w, int h)
        {
            Image out;
            out.w = std::clamp(w, 0, image.w - x), out.h = std::clamp(h, 0, image.h - y);
            out.px.resize(size_t(out.w) * out.h);
            for (int row = 0; row < out.h; row++)
                std::copy_n(&image.px[size_t(y + row) * image.w + x], out.w, &out.px[size_t(row) * out.w]);
            return out;
        }

        // A sprite cut into tiles (more than one image chunk), stitched back as tools/textures.py
        // does (sprite_layout): tiles go row by row, each with a 1-pixel border copied from its
        // neighbours, so the first starts at (-1, -1) and the next 2 pixels before the previous
        // one ends; a row ends with the tile whose inside reaches the image's right edge.
        bool StitchSprite(const uint8_t* data, const Chunk& owner, int width, int height, Image& image)
        {
            std::vector<Image> tiles;
            for (const Chunk& child : Children(data, owner.offset + owner.header, owner.offset + owner.size))
            {
                if (child.id != IMAGE)
                    continue;
                tiles.emplace_back();
                bool decoded = false;
                for (const Chunk& part : Children(data, child.offset + child.header, child.offset + child.size))
                {
                    if (part.id != IMAGE_DATA || part.header < 16)
                        continue;
                    uint32_t size = LE32(data + part.offset + 12);
                    int w, h;
                    std::vector<uint8_t> rgba;
                    decoded = part.offset + 16 + size <= part.offset + part.size &&
                              language_font::DecodePng(data + part.offset + 16, size, w, h, rgba) && Premultiplied(rgba, w, h, tiles.back());
                    break;
                }
                if (!decoded)
                    return false;
            }
            if (tiles.size() < 2 || width <= 0 || height <= 0)
                return false;
            image.w = width, image.h = height;
            image.px.assign(size_t(width) * height, 0);
            int x = -1, y = -1, rowH = 0;
            for (const Image& tile : tiles)
            {
                if (y + 1 >= height || (rowH && tile.h != rowH))
                    return false;
                for (int ty = 0; ty < tile.h; ty++)
                    for (int tx = 0; tx < tile.w; tx++)
                    {
                        int ix = x + tx, iy = y + ty;
                        if (ix >= 0 && iy >= 0 && ix < width && iy < height)
                            image.px[size_t(iy) * width + ix] = tile.px[size_t(ty) * tile.w + tx];
                    }
                rowH = tile.h;
                if (x + tile.w - 1 >= width)
                    x = -1, y += tile.h - 2, rowH = 0;
                else
                    x += tile.w - 2;
            }
            return x == -1 && y + 1 >= height;
        }

        // Texture or sprite `name` ("X.tga") of a package.
        bool FindImage(const std::vector<uint8_t>& package, const std::string& name, Image& image)
        {
            bool found = false;
            std::string wanted = Lower(name);
            Walk(package.data(), 0, package.size(), [&](const Chunk& chunk) {
                if (found || (chunk.id != TEXTURE && chunk.id != SPRITE) || chunk.header < 14)
                    return;
                const uint8_t* header = package.data() + chunk.offset + 12;
                std::string chunkName = Pascal(header, chunk.header - 12);
                if (Lower(chunkName) != wanted || !OwnerImage(package.data(), chunk, image))
                    return;
                found = true;
                // Sprites are cut into tiles with a 1-pixel border (tools/textures.py); a single
                // tile holds the whole image at (1, 1) when it is bigger than the image.
                size_t pos = 1 + header[0];
                if (chunk.id == SPRITE && chunk.header >= 12 + pos + 21)
                {
                    uint32_t w = LE32(header + pos + 9), h = LE32(header + pos + 13), tiles = LE32(header + pos + 17);
                    if (tiles == 1 && w && h && int(w) + 2 <= image.w && int(h) + 2 <= image.h)
                        image = Crop(image, 1, 1, int(w), int(h));
                    else if (tiles > 1)
                        found = StitchSprite(package.data(), chunk, int(w), int(h), image);
                }
            });
            return found;
        }

        // Fonts ---------------------------------------------------------------------------------

        struct Glyph
        {
            int page;
            int x0, y0, x1, y1;
            float left, advance;
            bool icon, empty;
            Image cell;  // text glyphs: the cell, cleaned (see LoadFont)
        };

        struct Font
        {
            std::string name;
            int cell = 0;  // line height in page pixels
            float space = 0;
            std::unordered_map<char32_t, Glyph> glyphs;
            std::vector<Image> pages;
        };

        bool LoadFont(const std::vector<uint8_t>& package, const std::string& name, Font& font, std::string& log)
        {
            const uint8_t* data = package.data();
            Chunk fontChunk{};
            bool found = false;
            Walk(data, 0, package.size(), [&](const Chunk& chunk) {
                if (!found && chunk.id == FONT && chunk.header > 17 && Pascal(data + chunk.offset + 16, chunk.header - 16) == name)
                    fontChunk = chunk, found = true;
            });
            if (!found)
                return false;
            // The letters the fonts lack, composed like the PT-BR pack does (languages\pt-br\language.toml).
            std::vector<language_font::Recipe> recipes;
            for (const char16_t* text : { u"ã = a + ñ - n @ å", u"õ = o + ñ - n @ ò", u"Ã = A + Ñ - N @ Å", u"Õ = O + Ñ - N @ Ò",
                                          u"Ê = E + Â - A @ È", u"Ô = O + Â - A @ Ö" })
            {
                language_font::Recipe recipe;
                if (language_font::ParseRecipe(text, recipe))
                    recipes.push_back(recipe);
            }
            std::vector<uint8_t> composed;
            language_font::ComposeFont(data + fontChunk.offset, fontChunk.size, recipes, composed, log);
            const uint8_t* c = composed.data();
            size_t size = composed.size(), header = LE32(c + 4);
            if (size < 16 || header < 16 || header > size)
                return false;

            const Chunk* glyphChunk = nullptr;
            std::vector<Chunk> children = Children(c, header, size);
            for (const Chunk& child : children)
            {
                if (child.id == TEXTURE)
                {
                    font.pages.emplace_back();
                    if (!OwnerImage(c, child, font.pages.back()))
                        font.pages.back() = Image{};
                }
                else if (child.id == GLYPHS)
                    glyphChunk = &child;
            }
            if (!glyphChunk || glyphChunk->header < 16)
                return false;
            uint32_t count = LE32(c + glyphChunk->offset + 12);
            if (16 + size_t(count) * 40 > glyphChunk->size)
                return false;
            for (uint32_t i = 0; i < count; i++)
            {
                const uint8_t* r = c + glyphChunk->offset + 16 + i * 40;
                uint32_t texture = LE32(r);
                float u0 = LEF(r + 4), v0 = LEF(r + 8), u1 = LEF(r + 12), v1 = LEF(r + 16);
                Glyph g{};
                g.icon = (texture & 0x80000000u) != 0;
                g.page = int(texture & 0x7FFFFFFFu);
                g.left = LEF(r + 20);
                g.advance = LEF(r + 32);
                char32_t code = LE32(r + 36);
                g.empty = !(u1 > u0) || !(v1 > v0) || g.page >= int(font.pages.size()) || !font.pages[g.page].w;
                if (!g.empty)
                {
                    const Image& page = font.pages[g.page];
                    g.x0 = std::clamp(int(std::lround(u0 * page.w)), 0, page.w), g.x1 = std::clamp(int(std::lround(u1 * page.w)), g.x0, page.w);
                    g.y0 = std::clamp(int(std::lround((1.0f - v1) * page.h)), 0, page.h), g.y1 = std::clamp(int(std::lround((1.0f - v0) * page.h)), g.y0, page.h);
                    g.empty = g.x1 <= g.x0 || g.y1 <= g.y0;
                }
                if (!g.empty && !g.icon)
                {
                    // The glyphs of the row above reach a few pixels into some cells (seen as a
                    // dash over Á Í): ink that continues from the row just above the cell is
                    // removed (flood fill from there; accents never touch the row above).
                    const Image& page = font.pages[g.page];
                    g.cell = Crop(page, g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0);
                    auto inked = [](uint32_t p) { return (p >> 24) > 16; };
                    std::vector<std::pair<int, int>> stack;
                    for (int x = 0; g.y0 > 0 && x < g.cell.w; x++)
                        if (inked(page.px[size_t(g.y0 - 1) * page.w + g.x0 + x]) && inked(g.cell.px[size_t(x)]))
                            stack.push_back({ x, 0 });
                    while (!stack.empty())
                    {
                        auto [x, y] = stack.back();
                        stack.pop_back();
                        uint32_t& p = g.cell.px[size_t(y) * g.cell.w + x];
                        if (!inked(p))
                            continue;
                        p = 0;
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++)
                                if (x + dx >= 0 && x + dx < g.cell.w && y + dy >= 0 && y + dy < g.cell.h)
                                    stack.push_back({ x + dx, y + dy });
                    }
                }
                font.glyphs[code] = std::move(g);
            }
            auto a = font.glyphs.find(U'A');
            auto space = font.glyphs.find(U' ');
            if (a == font.glyphs.end() || a->second.empty)
                return false;
            font.cell = a->second.y1 - a->second.y0;
            font.space = space != font.glyphs.end() ? space->second.advance : font.cell * 0.22f;
            font.name = name;
            return true;
        }

        Font g_fonts[2];
        Image g_art[size_t(Art::Count)];
        bool g_fontsOk = false;
        std::mutex g_mutex;
        std::condition_variable g_loaded;
        bool g_done = false;
        bool g_started = false;

        void Load()
        {
            auto start = plat::TickCountMs();
            Rcf rcf;
            bool open = OpenRcf(rcf);
            std::string log;
            std::vector<uint8_t> persistent, inGame, frontend;
            std::string sources[3];
            bool fonts = false;
            if (open && ReadEntry(rcf, "package\\b4c85fe7.p3d", persistent, sources[0]))
            {
                fonts = LoadFont(persistent, "Titans_Small", g_fonts[int(FontId::Small)], log) &&
                        LoadFont(persistent, "Titans_Large", g_fonts[int(FontId::Large)], log);
                FindImage(persistent, "MSG_Background.tga", g_art[size_t(Art::MessageFrame)]);
            }
            if (open && ReadEntry(rcf, "package\\c1e387c7.p3d", inGame, sources[1]))
            {
                FindImage(inGame, "InGame_Pause_Background.tga", g_art[size_t(Art::PauseStrip)]);
                FindImage(inGame, "FE_star_open.tga", g_art[size_t(Art::StarFull)]);
            }
            if (open && ReadEntry(rcf, "package\\cdd70a8c.p3d", frontend, sources[2]))
            {
                FindImage(frontend, "FE_Name_Divider_Left.tga", g_art[size_t(Art::DividerLeft)]);
                FindImage(frontend, "FE_Name_Divider_Right.tga", g_art[size_t(Art::DividerRight)]);
                FindImage(frontend, "FE_Background_Weave.tga", g_art[size_t(Art::BackdropWeave)]);
                FindImage(frontend, "FE_Background_Fold.tga", g_art[size_t(Art::BackdropFold)]);
                FindImage(frontend, "FE_Background_Foliage_Top.tga", g_art[size_t(Art::FoliageTop)]);
                FindImage(frontend, "FE_Background_Foliage_Bottom.tga", g_art[size_t(Art::FoliageBottom)]);
                FindImage(frontend, "FE_Background_Light_Map.tga", g_art[size_t(Art::LightMap)]);
                FindImage(frontend, "FE_Title_Logo.tga", g_art[size_t(Art::TitleLogo)]);
            }
            int art = 0;
            for (const Image& image : g_art)
                art += image.w ? 1 : 0;
            if (fonts)
                Log("[settings] screen art: fonts %s (%zu pages, %zu glyphs) and %s (%zu pages, %zu glyphs), %d/%d images, %llu ms (%s; %s)",
                    g_fonts[0].name.c_str(), g_fonts[0].pages.size(), g_fonts[0].glyphs.size(), g_fonts[1].name.c_str(),
                    g_fonts[1].pages.size(), g_fonts[1].glyphs.size(), art, int(Art::Count), (unsigned long long)(plat::TickCountMs() - start),
                    sources[0].c_str(), log.c_str());
            else
                Log("[settings] screen art: the game's fonts were not found (%s); the screen uses the overlay font",
                    open ? "package\\b4c85fe7.p3d" : "default.rcf");
            std::lock_guard lock(g_mutex);
            g_fontsOk = fonts;
            g_done = true;
            g_loaded.notify_all();
        }

        // Resampling ----------------------------------------------------------------------------

        using Weights = std::vector<std::vector<std::pair<int, float>>>;

        // Source pixels and weights of each of `dn` destination pixels over `sn` source pixels.
        Weights Weights1D(int sn, int dn)
        {
            Weights weights(size_t(std::max(dn, 0)));
            if (sn <= 0 || dn <= 0)
                return weights;
            float scale = float(sn) / float(dn);
            for (int i = 0; i < dn; i++)
            {
                if (dn <= sn)
                {
                    float a = i * scale, b = (i + 1) * scale;
                    for (int j = int(std::floor(a)); j < int(std::ceil(b)) && j < sn; j++)
                    {
                        float overlap = std::min(b, float(j + 1)) - std::max(a, float(j));
                        if (overlap > 0.0f)
                            weights[i].push_back({ j, overlap / scale });
                    }
                }
                else
                {
                    float center = (i + 0.5f) * scale - 0.5f;
                    int j = int(std::floor(center));
                    float f = center - float(j);
                    weights[i].push_back({ std::clamp(j, 0, sn - 1), 1.0f - f });
                    weights[i].push_back({ std::clamp(j + 1, 0, sn - 1), f });
                }
            }
            return weights;
        }

        Image Resample(const Image& image, int sx, int sy, int sw, int sh, int dw, int dh, bool flipX, bool flipY)
        {
            Image out;
            sx = std::clamp(sx, 0, image.w), sy = std::clamp(sy, 0, image.h);
            sw = std::clamp(sw, 0, image.w - sx), sh = std::clamp(sh, 0, image.h - sy);
            if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
                return out;
            Weights wx = Weights1D(sw, dw), wy = Weights1D(sh, dh);
            std::vector<float> rows(size_t(sh) * dw * 4, 0.0f);
            for (int y = 0; y < sh; y++)
            {
                const uint32_t* src = &image.px[size_t(sy + y) * image.w + sx];
                for (int x = 0; x < dw; x++)
                {
                    float* d = &rows[(size_t(y) * dw + x) * 4];
                    for (auto [j, w] : wx[x])
                    {
                        uint32_t p = src[j];
                        d[0] += w * float(p & 0xFF), d[1] += w * float((p >> 8) & 0xFF), d[2] += w * float((p >> 16) & 0xFF), d[3] += w * float(p >> 24);
                    }
                }
            }
            out.w = dw, out.h = dh;
            out.px.resize(size_t(dw) * dh);
            for (int y = 0; y < dh; y++)
            {
                for (int x = 0; x < dw; x++)
                {
                    float c[4] = {};
                    for (auto [j, w] : wy[y])
                    {
                        const float* s = &rows[(size_t(j) * dw + x) * 4];
                        c[0] += w * s[0], c[1] += w * s[1], c[2] += w * s[2], c[3] += w * s[3];
                    }
                    uint32_t a = uint32_t(std::clamp(std::lround(c[3]), 0L, 255L));
                    uint32_t r = uint32_t(std::clamp(std::lround(c[0]), 0L, long(a))), g = uint32_t(std::clamp(std::lround(c[1]), 0L, long(a))),
                             b = uint32_t(std::clamp(std::lround(c[2]), 0L, long(a)));
                    int tx = flipX ? dw - 1 - x : x, ty = flipY ? dh - 1 - y : y;
                    out.px[size_t(ty) * dw + tx] = r | (g << 8) | (b << 16) | (a << 24);
                }
            }
            return out;
        }

        using CacheKey = std::tuple<const Image*, int, int, int, int, int, int, bool, bool>;
        std::map<CacheKey, Image> g_cache;

        uint32_t Over(uint32_t dst, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
        {
            if (a == 0 && !(r | g | b))
                return dst;
            uint32_t k = 255 - a;
            uint32_t dr = dst & 0xFF, dg = (dst >> 8) & 0xFF, db = (dst >> 16) & 0xFF, da = dst >> 24;
            dr = std::min(255u, r + (dr * k + 127) / 255);
            dg = std::min(255u, g + (dg * k + 127) / 255);
            db = std::min(255u, b + (db * k + 127) / 255);
            da = std::min(255u, a + (da * k + 127) / 255);
            return dr | (dg << 8) | (db << 16) | (da << 24);
        }

        // Font fallback: the overlay's 5x7 font (no accents), when the game's fonts are missing.
        char Plain(char32_t c)
        {
            static const std::pair<const char32_t*, char> MAP[] = {
                { U"ÁÀÂÃÄáàâãä", 'A' }, { U"ÉÈÊËéèêë", 'E' }, { U"ÍÌÎÏíìîï", 'I' }, { U"ÓÒÔÕÖóòôõö", 'O' },
                { U"ÚÙÛÜúùûü", 'U' }, { U"Çç", 'C' }, { U"Ññ", 'N' },
            };
            if (c < 0x80)
                return char(toupper(int(c)));
            for (auto& [chars, plain] : MAP)
                for (const char32_t* p = chars; *p; p++)
                    if (*p == c)
                        return plain;
            return ' ';
        }

        const Font* LoadedFont(FontId font)
        {
            std::lock_guard lock(g_mutex);
            return g_fontsOk ? &g_fonts[int(font)] : nullptr;
        }
    }

    void StartLoading()
    {
        {
            std::lock_guard lock(g_mutex);
            if (g_started)
                return;
            g_started = true;
        }
        std::thread(Load).detach();
    }

    bool Ready(int waitMs)
    {
        std::unique_lock lock(g_mutex);
        if (!g_started)
            return false;
        g_loaded.wait_for(lock, std::chrono::milliseconds(waitMs), [] { return g_done; });
        return g_done && g_fontsOk;
    }

    const Image* GetArt(Art art)
    {
        std::lock_guard lock(g_mutex);
        const Image& image = g_art[size_t(art)];
        return g_done && image.w ? &image : nullptr;
    }

    // Canvas ------------------------------------------------------------------------------------

    void Canvas::Reset(int width, int height)
    {
        w_ = std::max(width, 0), h_ = std::max(height, 0);
        px_.assign(size_t(w_) * h_, 0);
    }

    void Canvas::Fill(int x0, int y0, int x1, int y1, Color color)
    {
        FillGradient(x0, y0, x1, y1, color, color);
    }

    void Canvas::FillGradient(int x0, int y0, int x1, int y1, Color top, Color bottom)
    {
        int rows = std::max(y1 - y0, 1);
        x0 = std::max(x0, 0), x1 = std::min(x1, w_);
        for (int y = std::max(y0, 0); y < std::min(y1, h_); y++)
        {
            float t = rows > 1 ? float(y - y0) / float(rows - 1) : 0.0f;
            auto mix = [&](uint8_t a, uint8_t b) { return uint32_t(std::lround(a + (float(b) - float(a)) * t)); };
            uint32_t a = mix(top.a, bottom.a);
            uint32_t r = mix(top.r, bottom.r) * a / 255, g = mix(top.g, bottom.g) * a / 255, b = mix(top.b, bottom.b) * a / 255;
            for (int x = x0; x < x1; x++)
                px_[size_t(y) * w_ + x] = Over(px_[size_t(y) * w_ + x], r, g, b, a);
        }
    }

    void Canvas::Blit(const Image& image, int x, int y, Color tint, int clipX1)
    {
        int x1 = std::min({ x + image.w, w_, clipX1 });
        for (int row = std::max(0, -y); row < image.h && y + row < h_; row++)
        {
            const uint32_t* src = &image.px[size_t(row) * image.w];
            uint32_t* dst = &px_[size_t(y + row) * w_];
            for (int col = std::max(0, -x); x + col < x1; col++)
            {
                uint32_t p = src[col];
                if (!(p >> 24))
                    continue;
                uint32_t r = (p & 0xFF) * tint.r / 255 * tint.a / 255, g = ((p >> 8) & 0xFF) * tint.g / 255 * tint.a / 255,
                         b = ((p >> 16) & 0xFF) * tint.b / 255 * tint.a / 255, a = (p >> 24) * tint.a / 255;
                dst[x + col] = Over(dst[x + col], r, g, b, a);
            }
        }
    }

    void Canvas::BlitGlow(const Image& image, int x, int y, Color tint)
    {
        for (int row = std::max(0, -y); row < image.h && y + row < h_; row++)
        {
            const uint32_t* src = &image.px[size_t(row) * image.w];
            uint32_t* dst = &px_[size_t(y + row) * w_];
            for (int col = std::max(0, -x); col < image.w && x + col < w_; col++)
            {
                uint32_t p = src[col];
                uint32_t light = ((p & 0xFF) + ((p >> 8) & 0xFF) + ((p >> 16) & 0xFF)) / 3;  // premultiplied: alpha included
                uint32_t a = light * tint.a / 255;
                if (a)
                    dst[x + col] = Over(dst[x + col], tint.r * a / 255, tint.g * a / 255, tint.b * a / 255, a);
            }
        }
    }

    void Canvas::BlitGlyph(const Image& glyph, int x, int y, const TextStyle& style)
    {
        for (int row = std::max(0, -y); row < glyph.h && y + row < h_; row++)
        {
            const uint32_t* src = &glyph.px[size_t(row) * glyph.w];
            uint32_t* dst = &px_[size_t(y + row) * w_];
            for (int col = std::max(0, -x); col < glyph.w && x + col < w_; col++)
            {
                int alpha = int(src[col] >> 24);
                if (!alpha)
                    continue;
                float coverage = std::min(1.0f, float(alpha) / OUTLINE_ALPHA);
                float t = std::clamp(float(alpha - OUTLINE_ALPHA) / float(FILL_ALPHA - OUTLINE_ALPHA), 0.0f, 1.0f);
                auto mix = [&](uint8_t a, uint8_t b) { return float(a) + (float(b) - float(a)) * t; };
                float a = coverage * mix(style.outline.a, style.fill.a) / 255.0f * style.opacity;
                uint32_t a8 = uint32_t(std::lround(a * 255.0f));
                uint32_t r = uint32_t(std::lround(mix(style.outline.r, style.fill.r) * a)), g = uint32_t(std::lround(mix(style.outline.g, style.fill.g) * a)),
                         b = uint32_t(std::lround(mix(style.outline.b, style.fill.b) * a));
                dst[x + col] = Over(dst[x + col], r, g, b, a8);
            }
        }
    }

    void Canvas::Export(std::vector<uint32_t>& out) const
    {
        out.resize(px_.size());
        for (size_t i = 0; i < px_.size(); i++)
        {
            uint32_t p = px_[i], a = p >> 24;
            if (!a)
            {
                out[i] = 0;
                continue;
            }
            uint32_t r = std::min(255u, ((p & 0xFF) * 255 + a / 2) / a), g = std::min(255u, (((p >> 8) & 0xFF) * 255 + a / 2) / a),
                     b = std::min(255u, (((p >> 16) & 0xFF) * 255 + a / 2) / a);
            out[i] = r | (g << 8) | (b << 16) | (a << 24);
        }
    }

    // Scaling and text --------------------------------------------------------------------------

    const Image& Scaled(const Image& image, int sx, int sy, int sw, int sh, int dw, int dh, bool flipX, bool flipY)
    {
        CacheKey key{ &image, sx, sy, sw, sh, dw, dh, flipX, flipY };
        auto it = g_cache.find(key);
        if (it == g_cache.end())
            it = g_cache.emplace(key, Resample(image, sx, sy, sw, sh, dw, dh, flipX, flipY)).first;
        return it->second;
    }

    const Image& Scaled(const Image& image, int dw, int dh, bool flipX, bool flipY)
    {
        return Scaled(image, 0, 0, image.w, image.h, dw, dh, flipX, flipY);
    }

    void ClearCache()
    {
        g_cache.clear();
    }

    bool HasGlyph(FontId font, char32_t c)
    {
        const Font* f = LoadedFont(font);
        if (!f)
            return c < 0x80 && gpu::overlay_font::Find(Plain(c));
        auto it = f->glyphs.find(c);
        return it != f->glyphs.end() && !it->second.empty;
    }

    float IconWidth(char32_t icon, float height)
    {
        const Font* f = LoadedFont(FontId::Large);
        auto it = f ? f->glyphs.find(icon) : decltype(f->glyphs.end()){};
        if (!f || it == f->glyphs.end() || it->second.empty)
            return 0.0f;
        const Glyph& g = it->second;
        return std::round(height * float(g.x1 - g.x0) / float(g.y1 - g.y0));
    }

    float DrawIconTurned(Canvas& canvas, char32_t icon, float x, float y, float height, bool clockwise)
    {
        const Font* f = LoadedFont(FontId::Large);
        auto it = f ? f->glyphs.find(icon) : decltype(f->glyphs.end()){};
        if (!f || it == f->glyphs.end() || it->second.empty)
            return 0.0f;
        const Glyph& g = it->second;
        int h = int(std::lround(height)), w = int(IconWidth(icon, height));
        if (w <= 0 || h <= 0)
            return 0.0f;
        const Image& image = Scaled(f->pages[g.page], g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0, w, h);
        static std::mutex s_mutex;
        static std::map<std::tuple<char32_t, int, int, bool>, Image> s_turned;
        std::lock_guard lock(s_mutex);
        auto [found, inserted] = s_turned.try_emplace({ icon, w, h, clockwise });
        Image& turned = found->second;
        if (inserted)
        {
            // w x h -> h x w: clockwise the new (x, y) is the old (y, h - 1 - x); the other way (w - 1 - y, x).
            turned.w = h, turned.h = w;
            turned.px.resize(size_t(w) * h);
            for (int ty = 0; ty < w; ty++)
                for (int tx = 0; tx < h; tx++)
                {
                    int sx = clockwise ? ty : w - 1 - ty, sy = clockwise ? h - 1 - tx : tx;
                    turned.px[size_t(ty) * h + tx] = image.px[size_t(sy) * w + sx];
                }
        }
        canvas.Blit(turned, int(std::lround(x)), int(std::lround(y)));
        return float(turned.w);
    }

    float DrawIcon(Canvas& canvas, char32_t icon, float x, float y, float height, float opacity, bool flipX)
    {
        const Font* f = LoadedFont(FontId::Large);
        auto it = f ? f->glyphs.find(icon) : decltype(f->glyphs.end()){};
        if (!f || it == f->glyphs.end() || it->second.empty)
            return 0.0f;
        const Glyph& g = it->second;
        int h = int(std::lround(height)), w = int(IconWidth(icon, height));
        const Image& image = Scaled(f->pages[g.page], g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0, w, h, flipX);
        Color tint{ 255, 255, 255, uint8_t(std::lround(255.0f * opacity)) };
        // PlayStation / keyboard icons when that is the device in use (prompt_icons.h), drawn
        // over the game's icon's area; cached per style and size.
        if (uint32_t style = prompts::Style(); style && w > 0 && h > 0)
        {
            static std::mutex s_mutex;
            static std::map<std::tuple<uint32_t, char32_t, int, int, bool>, Image> s_icons;
            std::lock_guard lock(s_mutex);
            auto [found, inserted] = s_icons.try_emplace({ style, icon, w, h, flipX });
            if (inserted)
            {
                std::vector<uint32_t> rgba;
                if (prompts::Draw(icon, style, w, h, image.px.data(), rgba))
                {
                    found->second.w = w, found->second.h = h;
                    found->second.px.resize(rgba.size());
                    for (size_t i = 0; i < rgba.size(); i++)
                    {
                        uint32_t p = rgba[i], a = p >> 24;
                        auto premultiply = [&](int shift) { return (((p >> shift) & 0xFF) * a + 127) / 255 << shift; };
                        found->second.px[i] = premultiply(0) | premultiply(8) | premultiply(16) | a << 24;
                    }
                }
            }
            if (found->second.w)
            {
                canvas.Blit(found->second, int(std::lround(x)), int(std::lround(y)), tint);
                return float(w);
            }
        }
        canvas.Blit(image, int(std::lround(x)), int(std::lround(y)), tint);
        return float(w);
    }

    namespace
    {
        // Lays out (and with a canvas, draws) `text`; returns the pen position after it.
        float Layout(Canvas* canvas, FontId font, const std::u32string& text, float x, float y, float cell, const TextStyle* style,
                     float iconScale)
        {
            const Font* f = LoadedFont(font);
            float pen = x;
            if (!f)
            {
                // 5x7 + 2 rows of margin per cell.
                float unit = cell / 9.0f;
                for (char32_t c : text)
                {
                    const char* rows = gpu::overlay_font::Find(Plain(c));
                    if (rows && canvas && style)
                    {
                        Color color = style->fill;
                        color.a = uint8_t(std::lround(color.a * style->opacity));
                        for (int row = 0; row < gpu::overlay_font::HEIGHT; row++)
                            for (int col = 0; col < gpu::overlay_font::WIDTH; col++)
                                if (rows[row * gpu::overlay_font::WIDTH + col] == '#')
                                    canvas->Fill(int(pen + col * unit), int(y + (row + 1) * unit), int(pen + (col + 1) * unit),
                                                 int(y + (row + 2) * unit), color);
                    }
                    pen += 6.0f * unit;
                }
                return pen;
            }
            float k = cell / float(f->cell);
            for (char32_t c : text)
            {
                auto it = f->glyphs.find(c);
                if (it != f->glyphs.end() && it->second.icon && !it->second.empty)
                {
                    float h = cell * iconScale;
                    float width = IconWidth(c, h);
                    if (canvas)
                        DrawIcon(*canvas, c, pen, y + (cell - h) * 0.5f, h, style ? style->opacity : 1.0f);
                    pen += width;
                    continue;
                }
                if (it == f->glyphs.end() || it->second.empty)
                {
                    pen += f->space * k;
                    continue;
                }
                const Glyph& g = it->second;
                if (canvas && style)
                {
                    int w = std::max(1, int(std::lround((g.x1 - g.x0) * k))), h = std::max(1, int(std::lround((g.y1 - g.y0) * k)));
                    const Image& image = Scaled(g.cell, w, h);
                    canvas->BlitGlyph(image, int(std::lround(pen + g.left * k)), int(std::lround(y)), *style);
                }
                pen += g.advance * k;
            }
            return pen;
        }
    }

    float TextWidth(FontId font, const std::u32string& text, float cell, float iconScale)
    {
        return Layout(nullptr, font, text, 0.0f, 0.0f, cell, nullptr, iconScale);
    }

    float DrawGameText(Canvas& canvas, FontId font, const std::u32string& text, float x, float y, float cell, const TextStyle& style,
                   float iconScale)
    {
        return Layout(&canvas, font, text, x, y, cell, &style, iconScale);
    }

    std::u32string FitText(FontId font, const std::u32string& text, float& cell, float maxWidth, float minScale)
    {
        float width = TextWidth(font, text, cell);
        if (width <= maxWidth || width <= 0.0f)
            return text;
        float scale = std::max(minScale, maxWidth / width);
        cell *= scale;
        if (width * scale <= maxWidth + 0.01f)
            return text;
        std::u32string cut = text;
        while (!cut.empty() && TextWidth(font, cut + U"...", cell) > maxWidth)
            cut.pop_back();
        while (!cut.empty() && cut.back() == U' ')
            cut.pop_back();
        return cut + U"...";
    }

    // Message frames ----------------------------------------------------------------------------

    namespace
    {
        // MSG_Background's shape: per row of the corner image, where the border stops in its curl
        // third (a fraction of that third's width); the top border's depth (a fraction of the height);
        // the curl third's width over the height. Before the art is there (or without it): the
        // original's, measured the same way.
        struct FrameShape
        {
            std::vector<float> side;
            float top = 0.25f, curlAspect = 85.0f / 128.0f, sideMax = 0.9f;
        };

        FrameShape MeasureFrame(const Image& corner)
        {
            FrameShape shape;
            int curl = std::max(1, corner.w / 3);
            // The border is light (the box tints it), the inside black: opaque and bright.
            auto border = [&](int x, int y) {
                uint32_t p = corner.px[size_t(y) * corner.w + x], a = p >> 24;
                return a >= 48 && (p & 0xFF) + ((p >> 8) & 0xFF) + ((p >> 16) & 0xFF) >= a * 3 / 2;
            };
            int topRows = 0;
            for (int x = curl; x < corner.w; x++)
                for (int y = corner.h - 1; y >= 0; y--)
                    if (border(x, y))
                    {
                        topRows = std::max(topRows, y + 1);
                        break;
                    }
            shape.top = float(topRows) / corner.h;
            shape.side.assign(size_t(corner.h), 0.0f);
            shape.sideMax = 0.0f;
            for (int y = 0; y < corner.h; y++)
            {
                for (int x = curl - 1; x >= 0; x--)
                    if (border(x, y))
                    {
                        shape.side[size_t(y)] = float(x + 1) / curl;
                        break;
                    }
                if (y >= topRows)
                    shape.sideMax = std::max(shape.sideMax, shape.side[size_t(y)]);
            }
            shape.curlAspect = float(curl) / corner.h;
            return shape;
        }

        const FrameShape& Shape()
        {
            static const FrameShape original;
            static FrameShape measured;
            static std::once_flag once;
            const Image* corner = GetArt(Art::MessageFrame);
            if (!corner)
                return original;
            std::call_once(once, [&] { measured = MeasureFrame(*corner); });
            return measured;
        }
    }

    void DrawMessageFrame(Canvas& canvas, int left, int top, int right, int bottom, Color tint, Color fallback)
    {
        const Image* corner = GetArt(Art::MessageFrame);
        if (!corner)
        {
            canvas.Fill(left, top, right, bottom, fallback);
            return;
        }
        // The right / bottom quadrants take the odd pixel (no seam in the middle).
        int halfW = (right - left) / 2, halfH = (bottom - top) / 2;
        float k = float(halfH) / float(corner->h);
        int curl = corner->w / 3;  // the curl is in the left third
        int curlW = std::min(int(std::lround(curl * k)), halfW);
        for (int q = 0; q < 4; q++)
        {
            bool flipX = q & 1, flipY = q & 2;
            int qw = flipX ? right - left - halfW : halfW, qh = flipY ? bottom - top - halfH : halfH;
            int qx = flipX ? left + halfW : left, qy = flipY ? top + halfH : top;
            const Image& c = Scaled(*corner, 0, 0, curl, corner->h, curlW, qh, flipX, flipY);
            const Image& edge = Scaled(*corner, curl, 0, corner->w - curl, corner->h, qw - curlW, qh, flipX, flipY);
            canvas.Blit(c, flipX ? qx + qw - curlW : qx, qy, tint);
            canvas.Blit(edge, flipX ? qx : qx + curlW, qy, tint);
        }
    }

    float FrameTopInset(float y0, float y1)
    {
        return Shape().top * (y1 - y0) * 0.5f;
    }

    float FrameSideInset(float x0, float y0, float x1, float y1, float lineTop, float lineBottom)
    {
        const FrameShape& shape = Shape();
        float halfW = (x1 - x0) * 0.5f, halfH = (y1 - y0) * 0.5f;
        if (halfH <= 0.0f || halfW <= 0.0f)
            return 0.0f;
        float curlW = std::min(halfH * shape.curlAspect, halfW);
        if (shape.side.empty())
            return shape.sideMax * curlW;
        // The rows of the corner the line covers (the bottom half is the top one mirrored).
        float mid = (y0 + y1) * 0.5f;
        auto depth = [&](float y) { return std::clamp((y < mid ? y - y0 : y1 - y) / halfH, 0.0f, 1.0f); };
        float a = depth(lineTop), b = depth(lineBottom);
        float from = std::min(a, b), to = lineTop < mid && lineBottom > mid ? 1.0f : std::max(a, b);
        int n = int(shape.side.size());
        int r0 = std::clamp(int(from * n), 0, n - 1), r1 = std::clamp(int(std::ceil(to * n)), r0 + 1, n);
        float side = 0.0f;
        for (int r = r0; r < r1; r++)
            side = std::max(side, shape.side[size_t(r)]);
        return side * curlW;
    }

    void FrameOuterSize(float contentW, float contentH, float gap, float& outerW, float& outerH)
    {
        const FrameShape& shape = Shape();
        outerH = (contentH + 2.0f * gap) / std::max(0.2f, 1.0f - shape.top);
        float curlW = outerH * 0.5f * shape.curlAspect;
        outerW = contentW + 2.0f * (shape.sideMax * curlW + gap);
    }

    std::u32string Utf32(const std::string& utf8)
    {
        std::u32string out;
        for (size_t i = 0; i < utf8.size();)
        {
            uint8_t c = uint8_t(utf8[i]);
            char32_t code;
            int extra;
            if (c < 0x80)
                code = c, extra = 0;
            else if ((c & 0xE0) == 0xC0)
                code = c & 0x1F, extra = 1;
            else if ((c & 0xF0) == 0xE0)
                code = c & 0x0F, extra = 2;
            else
                code = c & 0x07, extra = 3;
            i++;
            for (int k = 0; k < extra && i < utf8.size(); k++, i++)
                code = (code << 6) | (uint8_t(utf8[i]) & 0x3F);
            out += code;
        }
        return out;
    }

    // The main menu's cloth (FE_Background_*: weave, folds, light map, foliage), on the CPU (menu_art.h).
    std::vector<uint32_t> ComposeBackdrop(int W, int H, float s)
    {
        std::vector<float> rgb(size_t(W) * H * 3);
        for (int y = 0; y < H; y++)
        {
            float t = float(y) / std::max(1, H - 1);
            float r = 255 - 14 * t, g = 204 - 58 * t, b = 66 - 34 * t;
            for (int x = 0; x < W; x++)
            {
                float* p = &rgb[(size_t(y) * W + x) * 3];
                p[0] = r, p[1] = g, p[2] = b;
            }
        }
        auto luminance = [](uint32_t p) { return float(p & 0xFF) / 255.0f; };  // grey images: R = G = B
        if (const Image* weave = GetArt(Art::BackdropWeave))
        {
            int tile = std::max(16, int(std::lround(256 * s)));
            const Image& w = Scaled(*weave, tile, tile);
            double mean = 0;
            for (uint32_t p : w.px) mean += luminance(p);
            mean /= std::max<size_t>(1, w.px.size());
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    float k = 1.0f + 1.1f * (luminance(w.px[size_t(y % tile) * tile + x % tile]) - float(mean));
                    float* p = &rgb[(size_t(y) * W + x) * 3];
                    p[0] *= k, p[1] *= k, p[2] *= k;
                }
        }
        if (const Image* fold = GetArt(Art::BackdropFold))
        {
            const Image& f = Scaled(*fold, W, H);
            for (size_t i = 0; i < f.px.size(); i++)
            {
                float v = luminance(f.px[i]);
                float* p = &rgb[i * 3];
                p[0] = p[0] * (0.86f + 0.3f * v) + 40 * v, p[1] = p[1] * (0.86f + 0.3f * v) + 30 * v, p[2] = p[2] * (0.86f + 0.3f * v);
            }
        }
        if (const Image* light = GetArt(Art::LightMap))
        {
            const Image& l = Scaled(*light, W, H);
            for (size_t i = 0; i < l.px.size(); i++)
            {
                float k = 1.1f - 0.34f * luminance(l.px[i]);
                float* p = &rgb[i * 3];
                p[0] *= k, p[1] *= k, p[2] *= k;
            }
        }
        // Foliage silhouettes in the corners, in a darker orange.
        auto foliage = [&](Art art, bool bottom) {
            const Image* leaf = GetArt(art);
            if (!leaf)
                return;
            int size = int(std::lround(H * 0.62f));
            for (int side = 0; side < 2; side++)
            {
                const Image& img = Scaled(*leaf, size, size, side == 1);
                int ox = side ? W - size : 0, oy = bottom ? H - size : 0;
                for (int y = 0; y < size; y++)
                    for (int x = 0; x < size; x++)
                    {
                        int px = ox + x, py = oy + y;
                        if (px < 0 || py < 0 || px >= W || py >= H)
                            continue;
                        float a = float(img.px[size_t(y) * size + x] >> 24) / 255.0f * 0.36f;
                        float* p = &rgb[(size_t(py) * W + px) * 3];
                        p[0] += (196 - p[0]) * a, p[1] += (92 - p[1]) * a, p[2] += (12 - p[2]) * a;
                    }
            }
        };
        foliage(Art::FoliageTop, false);
        foliage(Art::FoliageBottom, true);
        std::vector<uint32_t> out(size_t(W) * H);
        for (size_t i = 0; i < out.size(); i++)
        {
            auto c = [&](int k) { return uint32_t(std::clamp(rgb[i * 3 + k], 0.0f, 255.0f)); };
            out[i] = c(0) | (c(1) << 8) | (c(2) << 16) | 0xFF000000u;
        }
        return out;
    }
}
