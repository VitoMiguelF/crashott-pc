#include "language_font.h"

#include "../platform/image.h"
#include "../platform/text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace settings::language_font
{
    namespace
    {
        constexpr uint32_t GLYPHS = 0x22001, TEXTURE = 0x19000, IMAGE = 0x19001, IMAGE_DATA = 0x19002;
        // Pages per font the engine takes: with a 33rd page on Titans_Large (32 pages) the new
        // glyphs drew nothing and the menu text flickered.
        constexpr size_t MAX_TEXTURES = 32;

        uint32_t LoadLE32(const uint8_t* p)
        {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }

        void StoreLE32(uint8_t* p, uint32_t value)
        {
            p[0] = uint8_t(value), p[1] = uint8_t(value >> 8), p[2] = uint8_t(value >> 16), p[3] = uint8_t(value >> 24);
        }

        void AppendLE32(std::vector<uint8_t>& out, uint32_t value)
        {
            uint8_t bytes[4];
            StoreLE32(bytes, value);
            out.insert(out.end(), bytes, bytes + 4);
        }

        void AppendBE32(std::vector<uint8_t>& out, uint32_t value)
        {
            out.push_back(uint8_t(value >> 24)), out.push_back(uint8_t(value >> 16)), out.push_back(uint8_t(value >> 8)), out.push_back(uint8_t(value));
        }

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
                uint32_t id = LoadLE32(data + offset), header = LoadLE32(data + offset + 4), size = LoadLE32(data + offset + 8);
                if (size < 12 || header < 12 || header > size || offset + size > end)
                    break;
                children.push_back({ id, offset, header, size });
                offset += size;
            }
            return children;
        }

        struct Glyph
        {
            uint32_t texture;
            float u0, v0, u1, v1, left, right, width, advance;
            uint32_t code;

            bool Empty() const { return !(u1 > u0) || !(v1 > v0); }
        };
        static_assert(sizeof(Glyph) == 40, "glyph records are 40 bytes");

        struct Image
        {
            int w = 0, h = 0;
            std::vector<uint8_t> rgba;  // straight alpha

            uint8_t* At(int x, int y) { return &rgba[(size_t(y) * w + x) * 4]; }
            const uint8_t* At(int x, int y) const { return &rgba[(size_t(y) * w + x) * 4]; }
        };

        bool DecodePng(const uint8_t* data, size_t size, Image& image)
        {
            return plat::DecodePng(data, size, image.w, image.h, image.rgba) && image.w <= 4096 && image.h <= 4096;
        }

        // Raw contents of the first `type` chunk of a PNG, or empty.
        std::vector<uint8_t> PngChunk(const uint8_t* png, size_t size, const char* type)
        {
            for (size_t pos = 8; pos + 12 <= size;)
            {
                uint32_t length = (uint32_t(png[pos]) << 24) | (uint32_t(png[pos + 1]) << 16) | (uint32_t(png[pos + 2]) << 8) | png[pos + 3];
                if (pos + 12 + length > size)
                    break;
                if (memcmp(png + pos + 4, type, 4) == 0)
                    return std::vector<uint8_t>(png + pos + 8, png + pos + 8 + length);
                pos += 12 + length;
            }
            return {};
        }

        uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc)
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
            crc = ~crc;
            for (size_t i = 0; i < size; i++)
                crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        void WritePngChunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& body)
        {
            AppendBE32(png, uint32_t(body.size()));
            size_t start = png.size();
            png.insert(png.end(), type, type + 4);
            png.insert(png.end(), body.begin(), body.end());
            AppendBE32(png, Crc32(&png[start], png.size() - start, 0));
        }

        // A PNG with stored (uncompressed) deflate blocks: indexed with `plte`/`trns` when `plte`
        // is not empty (each pixel takes the nearest palette entry), RGBA otherwise.
        std::vector<uint8_t> EncodePng(const Image& image, const std::vector<uint8_t>& plte, const std::vector<uint8_t>& trns)
        {
            bool indexed = !plte.empty();
            std::vector<uint8_t> raw;  // filter byte + scanline per row
            raw.reserve(size_t(image.h) * (1 + size_t(image.w) * (indexed ? 1 : 4)));
            size_t entries = plte.size() / 3;
            std::map<uint32_t, uint8_t> nearest;
            for (int y = 0; y < image.h; y++)
            {
                raw.push_back(0);
                for (int x = 0; x < image.w; x++)
                {
                    const uint8_t* p = image.At(x, y);
                    if (!indexed)
                    {
                        raw.insert(raw.end(), p, p + 4);
                        continue;
                    }
                    uint32_t key = LoadLE32(p);
                    auto it = nearest.find(key);
                    if (it == nearest.end())
                    {
                        int best = 0;
                        long bestDistance = LONG_MAX;
                        for (size_t i = 0; i < entries; i++)
                        {
                            int a = i < trns.size() ? trns[i] : 255;
                            // Weigh color by alpha: invisible pixels only need the right alpha.
                            long dr = long(plte[i * 3]) - p[0], dg = long(plte[i * 3 + 1]) - p[1], db = long(plte[i * 3 + 2]) - p[2];
                            long da = long(a) - p[3];
                            long distance = da * da * 4 + (dr * dr + dg * dg + db * db) * p[3] / 255;
                            if (distance < bestDistance)
                                bestDistance = distance, best = int(i);
                        }
                        it = nearest.emplace(key, uint8_t(best)).first;
                    }
                    raw.push_back(it->second);
                }
            }
            std::vector<uint8_t> zlib = { 0x78, 0x01 };
            for (size_t pos = 0; pos < raw.size() || pos == 0;)
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
            AppendBE32(header, uint32_t(image.w));
            AppendBE32(header, uint32_t(image.h));
            header.insert(header.end(), { 8, uint8_t(indexed ? 3 : 6), 0, 0, 0 });
            WritePngChunk(png, "IHDR", header);
            if (indexed)
            {
                WritePngChunk(png, "PLTE", plte);
                if (!trns.empty())
                    WritePngChunk(png, "tRNS", trns);
            }
            WritePngChunk(png, "IDAT", zlib);
            WritePngChunk(png, "IEND", {});
            return png;
        }

        struct Rect
        {
            int x0, y0, x1, y1;
        };

        Rect CellRect(const Glyph& glyph, const Image& page)
        {
            Rect r{ int(std::lround(glyph.u0 * page.w)), int(std::lround((1.0f - glyph.v1) * page.h)),
                    int(std::lround(glyph.u1 * page.w)), int(std::lround((1.0f - glyph.v0) * page.h)) };
            r.x0 = std::clamp(r.x0, 0, page.w), r.x1 = std::clamp(r.x1, r.x0, page.w);
            r.y0 = std::clamp(r.y0, 0, page.h), r.y1 = std::clamp(r.y1, r.y0, page.h);
            return r;
        }

        Image Crop(const Image& page, const Rect& r)
        {
            Image cell;
            cell.w = r.x1 - r.x0, cell.h = r.y1 - r.y0;
            cell.rgba.resize(size_t(cell.w) * cell.h * 4);
            for (int y = 0; y < cell.h; y++)
                memcpy(cell.At(0, y), page.At(r.x0, r.y0 + y), size_t(cell.w) * 4);
            return cell;
        }

        // Crop of a glyph cell without the ink that continues into it from the row above: some
        // glyphs reach a few pixels below their own cell (Titans_Small's Â cell starts with a
        // dash of the glyph above, which was taken for the circumflex: Ê/Ô came out without it).
        Image CleanCrop(const Image& page, const Rect& r)
        {
            Image cell = Crop(page, r);
            auto inked = [](const uint8_t* p) { return p[3] > 16; };
            std::vector<std::pair<int, int>> stack;
            for (int x = 0; r.y0 > 0 && x < cell.w; x++)
                if (inked(page.At(r.x0 + x, r.y0 - 1)) && inked(cell.At(x, 0)))
                    stack.push_back({ x, 0 });
            while (!stack.empty())
            {
                auto [x, y] = stack.back();
                stack.pop_back();
                uint8_t* p = cell.At(x, y);
                if (!inked(p))
                    continue;
                memset(p, 0, 4);
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        if (x + dx >= 0 && x + dx < cell.w && y + dy >= 0 && y + dy < cell.h)
                            stack.push_back({ x + dx, y + dy });
            }
            return cell;
        }

        // The glyphs are white with the shape in alpha: the fill is opaque and the outline is a
        // ring of about half alpha around it, which the game colors differently.
        constexpr int ANY = 24, FILL = 200, DIFF = 24;

        int TopRow(const Image& cell, int threshold)
        {
            for (int y = 0; y < cell.h; y++)
                for (int x = 0; x < cell.w; x++)
                    if (cell.At(x, y)[3] > threshold)
                        return y;
            return cell.h;
        }

        // Horizontal center of the pixels above `threshold` in rows [y0, y1); -1 without any.
        float InkCenter(const Image& cell, int y0, int y1, int threshold)
        {
            int lo = cell.w, hi = -1;
            for (int y = std::max(y0, 0); y < std::min(y1, cell.h); y++)
                for (int x = 0; x < cell.w; x++)
                    if (cell.At(x, y)[3] > threshold)
                        lo = std::min(lo, x), hi = std::max(hi, x);
            return hi < 0 ? -1.0f : (lo + hi) / 2.0f;
        }

        // Steps (8 neighbours, through pixels with any alpha) from `seeds` to each pixel of `cell`,
        // up to `limit`; -1 beyond it.
        std::vector<int> Distances(const Image& cell, const std::vector<uint8_t>& seeds, int limit)
        {
            std::vector<int> distance(seeds.size(), -1);
            std::vector<size_t> queue;
            for (size_t at = 0; at < seeds.size(); at++)
                if (seeds[at])
                    distance[at] = 0, queue.push_back(at);
            for (size_t head = 0; head < queue.size(); head++)
            {
                size_t at = queue[head];
                if (distance[at] >= limit)
                    continue;
                int x = int(at % cell.w), y = int(at / cell.w);
                for (int ny = y - 1; ny <= y + 1; ny++)
                    for (int nx = x - 1; nx <= x + 1; nx++)
                    {
                        size_t next = size_t(ny) * cell.w + nx;
                        if (nx >= 0 && nx < cell.w && ny >= 0 && ny < cell.h && distance[next] < 0 && cell.At(nx, ny)[3])
                            distance[next] = distance[at] + 1, queue.push_back(next);
                    }
            }
            return distance;
        }

        // `base` with the accent of `accent` on top. The accent's fill is the opaque part `accent`
        // has and `accentBase` (aligned on their bodies) does not, above the body (specks smaller
        // than a quarter of the biggest piece dropped: bits of neighbouring cells); its outline is
        // every pixel of `accent` within an outline width of that fill and not nearer to the
        // body's fill. Taking whole rows above the body, or only what differs from the base
        // letter, cut the tilde: its outline runs into the N's. The accent keeps its distance to
        // the body (shifted by the difference between the base tops) and is merged by the larger
        // alpha, so outline and fill stay as the font draws them.
        bool Compose(const Image& base, const Image& accent, const Image& accentBase, Image& out)
        {
            if (base.h != accent.h || accent.h != accentBase.h || !base.w || !accent.w || !accentBase.w)
                return false;
            int h = accent.h, w = accent.w;
            int bodyTop = TopRow(accentBase, ANY), fillTop = TopRow(accentBase, FILL);
            int outline = std::max(2, fillTop - bodyTop + 1);
            float bodyCenter = InkCenter(accent, fillTop, h, ANY), baseBodyCenter = InkCenter(accentBase, fillTop, h, ANY);
            if (bodyCenter < 0 || baseBodyCenter < 0 || fillTop >= h)
                return false;
            int shift = int(std::lround(bodyCenter - baseBodyCenter));
            std::vector<uint8_t> fill(size_t(w) * h, 0);
            for (int y = 0; y < std::min(h, fillTop + outline); y++)
            {
                for (int x = 0; x < w; x++)
                {
                    int bx = x - shift;
                    int a = accent.At(x, y)[3], b = bx >= 0 && bx < accentBase.w ? accentBase.At(bx, y)[3] : 0;
                    fill[size_t(y) * w + x] = a > FILL && a > b + DIFF;
                }
            }
            // Connected pieces of the fill (8 neighbours); keep the big ones.
            std::vector<int> piece(fill.size(), -1);
            std::vector<size_t> sizes;
            for (size_t start = 0; start < fill.size(); start++)
            {
                if (!fill[start] || piece[start] >= 0)
                    continue;
                int id = int(sizes.size());
                size_t count = 0;
                std::vector<size_t> stack{ start };
                piece[start] = id;
                while (!stack.empty())
                {
                    size_t at = stack.back();
                    stack.pop_back();
                    count++;
                    int x = int(at % w), y = int(at / w);
                    for (int ny = y - 1; ny <= y + 1; ny++)
                        for (int nx = x - 1; nx <= x + 1; nx++)
                        {
                            size_t next = size_t(ny) * w + nx;
                            if (nx >= 0 && nx < w && ny >= 0 && ny < h && fill[next] && piece[next] < 0)
                                piece[next] = id, stack.push_back(next);
                        }
                }
                sizes.push_back(count);
            }
            if (sizes.empty())
                return false;
            size_t biggest = *std::max_element(sizes.begin(), sizes.end());
            std::vector<uint8_t> body(fill.size(), 0);
            for (size_t at = 0; at < fill.size(); at++)
            {
                if (fill[at] && sizes[piece[at]] * 4 < biggest)
                    fill[at] = 0;
                body[at] = !fill[at] && accent.rgba[at * 4 + 3] > FILL;
            }
            std::vector<int> toAccent = Distances(accent, fill, outline + 1), toBody = Distances(accent, body, outline + 1);
            std::vector<uint8_t> mask(fill.size(), 0);
            int top = h, left = w, right = -1;
            for (size_t at = 0; at < fill.size(); at++)
            {
                if (toAccent[at] < 0 || (toBody[at] >= 0 && toBody[at] < toAccent[at]))
                    continue;
                mask[at] = accent.rgba[at * 4 + 3];
                int x = int(at % w), y = int(at / w);
                top = std::min(top, y), left = std::min(left, x), right = std::max(right, x);
            }
            float baseCenter = InkCenter(base, 0, base.h, ANY);
            if (right < 0 || baseCenter < 0)
                return false;
            int dx = int(std::lround(baseCenter - (left + right) / 2.0f));
            int dy = std::max(TopRow(base, ANY) - bodyTop, -top);
            out = base;
            for (int y = 0; y < h; y++)
            {
                for (int x = 0; x < w; x++)
                {
                    uint8_t a = mask[size_t(y) * w + x];
                    int tx = x + dx, ty = y + dy;
                    if (!a || tx < 0 || tx >= out.w || ty < 0 || ty >= out.h)
                        continue;
                    uint8_t* d = out.At(tx, ty);
                    if (a > d[3])
                        memcpy(d, accent.At(x, y), 4);
                }
            }
            return true;
        }

        // Pascal-style name at `p` (length byte, then the characters): replaces the character
        // before ".bmp", or the last one, with `mark`.
        void MarkName(uint8_t* p, size_t limit, char mark)
        {
            size_t length = std::min<size_t>(p[0], limit ? limit - 1 : 0);
            std::string name(reinterpret_cast<const char*>(p + 1), strnlen(reinterpret_cast<const char*>(p + 1), length));
            if (name.empty())
                return;
            std::string lower = name;
            for (char& c : lower)
                c = char(tolower(uint8_t(c)));
            size_t dot = lower.rfind(".bmp");
            size_t at = dot != std::string::npos && dot > 0 ? dot - 1 : name.size() - 1;
            p[1 + at] = uint8_t(mark);
        }

        // A copy of texture chunk [data + chunk.offset, ...) with `png` as its image; with `mark`,
        // its names are marked with it (a new page needs names of its own).
        std::vector<uint8_t> CloneTexture(const uint8_t* data, const Chunk& texture, const std::vector<uint8_t>& png, char mark)
        {
            std::vector<uint8_t> out(data + texture.offset, data + texture.offset + texture.header);
            if (mark)
                MarkName(&out[12], out.size() - 12, mark);
            for (const Chunk& child : Children(data, texture.offset + texture.header, texture.offset + texture.size))
            {
                size_t start = out.size();
                if (child.id != IMAGE)
                {
                    out.insert(out.end(), data + child.offset, data + child.offset + child.size);
                    continue;
                }
                out.insert(out.end(), data + child.offset, data + child.offset + child.header);
                if (mark)
                    MarkName(&out[start + 12], child.header - 12, mark);
                for (const Chunk& part : Children(data, child.offset + child.header, child.offset + child.size))
                {
                    if (part.id != IMAGE_DATA)
                    {
                        out.insert(out.end(), data + part.offset, data + part.offset + part.size);
                        continue;
                    }
                    uint32_t size = uint32_t(16 + png.size());
                    AppendLE32(out, IMAGE_DATA);
                    AppendLE32(out, size);
                    AppendLE32(out, size);
                    AppendLE32(out, uint32_t(png.size()));
                    out.insert(out.end(), png.begin(), png.end());
                }
                StoreLE32(&out[start + 8], uint32_t(out.size() - start));
            }
            StoreLE32(&out[8], uint32_t(out.size()));
            return out;
        }

        // PNG bytes of texture chunk `texture`: the first 0x19002 under it.
        bool TexturePng(const uint8_t* data, const Chunk& texture, const uint8_t*& png, size_t& size)
        {
            for (const Chunk& child : Children(data, texture.offset + texture.header, texture.offset + texture.size))
            {
                if (child.id != IMAGE)
                    continue;
                for (const Chunk& part : Children(data, child.offset + child.header, child.offset + child.size))
                {
                    if (part.id == IMAGE_DATA && part.header >= 16)
                    {
                        size = LoadLE32(data + part.offset + 12);
                        png = data + part.offset + 16;
                        return part.offset + 16 + size <= part.offset + part.size;
                    }
                }
            }
            return false;
        }
    }

    bool DecodePng(const uint8_t* data, size_t size, int& width, int& height, std::vector<uint8_t>& rgba)
    {
        Image image;
        if (!DecodePng(data, size, image))
            return false;
        width = image.w, height = image.h;
        rgba = std::move(image.rgba);
        return true;
    }

    bool ParseRecipe(const std::u16string& text, Recipe& recipe)
    {
        std::u16string s;
        for (char16_t c : text)
            if (c != u' ' && c != u'\t')
                s += c;
        if ((s.size() != 7 && s.size() != 9) || s[1] != u'=' || s[3] != u'+' || s[5] != u'-' || (s.size() == 9 && s[7] != u'@'))
            return false;
        recipe = { s[0], s[2], s[4], s[6], char16_t(s.size() == 9 ? s[8] : 0) };
        return true;
    }

    int ComposeFont(const uint8_t* data, size_t size, const std::vector<Recipe>& recipes, std::vector<uint8_t>& out, std::string& log)
    {
        auto copy = [&] {
            out.insert(out.end(), data, data + size);
            return 0;
        };
        uint32_t header = LoadLE32(data + 4);
        if (size < 12 || header < 16 || header > size)
            return copy();
        std::string fontName(reinterpret_cast<const char*>(data + 17), strnlen(reinterpret_cast<const char*>(data + 17), std::min<size_t>(data[16], header - 17)));
        std::vector<Chunk> children = Children(data, header, size);
        std::vector<Chunk> textures;
        const Chunk* glyphChunk = nullptr;
        for (const Chunk& child : children)
        {
            if (child.id == TEXTURE)
                textures.push_back(child);
            else if (child.id == GLYPHS)
                glyphChunk = &child;
        }
        uint32_t textureCount = LoadLE32(data + header - 4);
        if (!glyphChunk || textures.empty() || textureCount != textures.size() || glyphChunk->header < 16)
            return copy();

        std::map<uint32_t, Glyph> glyphs;  // by code
        uint32_t count = LoadLE32(data + glyphChunk->offset + 12);
        if (16 + size_t(count) * 40 > glyphChunk->size)
            return copy();
        for (uint32_t i = 0; i < count; i++)
        {
            Glyph g;
            memcpy(&g, data + glyphChunk->offset + 16 + i * 40, 40);
            glyphs[g.code] = g;
        }
        auto usable = [&](char16_t code) {
            auto it = glyphs.find(code);
            return it != glyphs.end() && !it->second.Empty() && it->second.texture < textures.size() ? &it->second : nullptr;
        };

        // Pages are decoded on demand.
        std::map<uint32_t, Image> pages;
        auto page = [&](uint32_t texture) -> const Image* {
            auto it = pages.find(texture);
            if (it != pages.end())
                return it->second.w ? &it->second : nullptr;
            Image& image = pages[texture];
            const uint8_t* png;
            size_t pngSize;
            if (!TexturePng(data, textures[texture], png, pngSize) || !DecodePng(png, pngSize, image))
                image = Image{};
            return image.w ? &image : nullptr;
        };

        struct Composed
        {
            Recipe recipe;
            Glyph base;
            Image cell;
        };
        std::vector<Composed> composed;
        std::string skipped;
        for (const Recipe& recipe : recipes)
        {
            auto existing = glyphs.find(recipe.target);
            if (existing != glyphs.end() && !existing->second.Empty())
                continue;  // the font has it
            const Glyph *base = usable(recipe.base), *accent = usable(recipe.accent), *accentBase = usable(recipe.accentBase);
            const Image *basePage = base ? page(base->texture) : nullptr, *accentPage = accent ? page(accent->texture) : nullptr,
                        *accentBasePage = accentBase ? page(accentBase->texture) : nullptr;
            Composed c{ recipe, base ? *base : Glyph{} };
            if (!basePage || !accentPage || !accentBasePage ||
                !Compose(CleanCrop(*basePage, CellRect(*base, *basePage)), CleanCrop(*accentPage, CellRect(*accent, *accentPage)),
                         CleanCrop(*accentBasePage, CellRect(*accentBase, *accentBasePage)), c.cell))
            {
                skipped += plat::NarrowUtf8(std::wstring(1, wchar_t(recipe.target)));
                continue;
            }
            composed.push_back(std::move(c));
        }
        if (composed.empty())
        {
            if (!skipped.empty())
                log += fontName + ": cannot compose " + skipped + "; ";
            return copy();
        }

        auto utf8 = [](char16_t c) {
            return plat::NarrowUtf8(std::wstring(1, wchar_t(c)));
        };
        auto palette = [&](uint32_t texture, std::vector<uint8_t>& plte, std::vector<uint8_t>& trns) {
            const uint8_t* png;
            size_t pngSize;
            if (TexturePng(data, textures[texture], png, pngSize))
                plte = PngChunk(png, pngSize, "PLTE"), trns = PngChunk(png, pngSize, "tRNS");
        };

        std::map<uint32_t, std::vector<uint8_t>> replacedPages;  // texture -> new PNG
        std::vector<uint8_t> newPage;                            // PNG of an added page, or empty
        uint32_t templateIndex = composed[0].base.texture;
        int added = 0;
        std::string names, where;
        if (textures.size() < MAX_TEXTURES)
        {
            // A new page with the size and palette of the page of the first base glyph.
            const Image& templatePage = pages[templateIndex];
            Image sheet;
            sheet.w = templatePage.w, sheet.h = templatePage.h;
            sheet.rgba.assign(size_t(sheet.w) * sheet.h * 4, 0);
            uint32_t newTexture = uint32_t(textures.size());
            int x = 1, y = 1, rowHeight = 0;
            for (Composed& c : composed)
            {
                if (x + c.cell.w + 1 > sheet.w)
                    x = 1, y += rowHeight + 1, rowHeight = 0;
                if (y + c.cell.h + 1 > sheet.h || c.cell.w + 2 > sheet.w)
                {
                    skipped += utf8(c.recipe.target);
                    continue;
                }
                for (int row = 0; row < c.cell.h; row++)
                    memcpy(sheet.At(x, y + row), c.cell.At(0, row), size_t(c.cell.w) * 4);
                Glyph g = c.base;
                g.texture = newTexture;
                g.u0 = float(x) / sheet.w, g.u1 = float(x + c.cell.w) / sheet.w;
                g.v1 = 1.0f - float(y) / sheet.h, g.v0 = 1.0f - float(y + c.cell.h) / sheet.h;
                g.width = float(c.cell.w);
                g.code = c.recipe.target;
                glyphs[g.code] = g;
                names += utf8(c.recipe.target);
                x += c.cell.w + 1;
                rowHeight = std::max(rowHeight, c.cell.h);
                added++;
            }
            if (added)
            {
                std::vector<uint8_t> plte, trns;
                palette(templateIndex, plte, trns);
                newPage = EncodePng(sheet, plte, trns);
                where = "on new page " + std::to_string(newTexture);
            }
        }
        else
        {
            // The font is at the engine's page limit (Titans_Large has 32 pages; a 33rd one left
            // the glyphs empty and made the menu text flicker): each glyph takes the cell of its
            // recipe's donor, a glyph no text of the language uses.
            std::map<uint32_t, Image> edited;
            for (Composed& c : composed)
            {
                const Glyph* donor = c.recipe.donor ? usable(c.recipe.donor) : nullptr;
                const Image* donorPage = donor ? page(donor->texture) : nullptr;
                if (!donorPage)
                {
                    skipped += utf8(c.recipe.target);
                    continue;
                }
                Image& target = edited.emplace(donor->texture, *donorPage).first->second;
                Rect r = CellRect(*donor, target);
                int width = r.x1 - r.x0, ox = (width - c.cell.w) / 2;
                for (int y = r.y0; y < r.y1; y++)
                    memset(target.At(r.x0, y), 0, size_t(width) * 4);
                for (int y = 0; y < c.cell.h && r.y0 + y < r.y1; y++)
                    for (int x = std::max(0, -ox); x < c.cell.w && ox + x < width; x++)
                        memcpy(target.At(r.x0 + ox + x, r.y0 + y), c.cell.At(x, y), 4);
                Glyph g = c.base;
                g.texture = donor->texture;
                g.u0 = donor->u0, g.v0 = donor->v0, g.u1 = donor->u1, g.v1 = donor->v1;
                g.width = float(width);
                g.code = c.recipe.target;
                glyphs[g.code] = g;
                names += utf8(c.recipe.target);
                where += (where.empty() ? "in the cells of " : "") + utf8(c.recipe.donor);
                added++;
            }
            for (auto& [texture, image] : edited)
            {
                std::vector<uint8_t> plte, trns;
                palette(texture, plte, trns);
                replacedPages[texture] = EncodePng(image, plte, trns);
            }
        }
        if (!added)
        {
            log += fontName + ": cannot compose " + skipped + "; ";
            return copy();
        }

        // Header (one more texture with a new page), the children with the replaced pages, the
        // new page after the last texture and the new glyph list.
        size_t start = out.size();
        out.insert(out.end(), data, data + header);
        if (!newPage.empty())
            StoreLE32(&out[start + header - 4], textureCount + 1);
        for (const Chunk& child : children)
        {
            if (child.id == GLYPHS)
            {
                uint32_t bytes = uint32_t(16 + glyphs.size() * 40);
                AppendLE32(out, GLYPHS);
                AppendLE32(out, bytes);
                AppendLE32(out, bytes);
                AppendLE32(out, uint32_t(glyphs.size()));
                for (auto& [code, g] : glyphs)
                {
                    uint8_t record[40];
                    memcpy(record, &g, 40);
                    out.insert(out.end(), record, record + 40);
                }
                continue;
            }
            auto index = std::find_if(textures.begin(), textures.end(), [&](const Chunk& t) { return t.offset == child.offset; });
            auto replaced = index == textures.end() ? replacedPages.end() : replacedPages.find(uint32_t(index - textures.begin()));
            if (replaced != replacedPages.end())
            {
                std::vector<uint8_t> texture = CloneTexture(data, child, replaced->second, 0);
                out.insert(out.end(), texture.begin(), texture.end());
            }
            else
                out.insert(out.end(), data + child.offset, data + child.offset + child.size);
            if (!newPage.empty() && child.offset == textures.back().offset)
            {
                std::vector<uint8_t> texture = CloneTexture(data, textures[templateIndex], newPage, 'P');
                out.insert(out.end(), texture.begin(), texture.end());
            }
        }
        StoreLE32(&out[start + 8], uint32_t(out.size() - start));
        log += fontName + ": " + names + " composed " + where + "; ";
        if (!skipped.empty())
            log += fontName + ": cannot compose " + skipped + "; ";
        return added;
    }
}
