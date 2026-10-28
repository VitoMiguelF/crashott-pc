// "Definitive Edition" under the logo of the title screen (title_logo.h; runtime/NOTES.md, "Logo
// Definitive Edition").
// Rewrites of default.rcf entries, found by content and left alone when the content is not what
// they expect (another version of the game, or a mod that changed that part):
//
// 1. The "frontend" text bibles of package\cdd70a8c.p3d and package\7efdcd91.p3d (the ones the
//    frontend reads) get a new key in every language. Keys are name hashes: h = (h * 64 + c) mod
//    the bible's modulo (0x1003F) over the name's bytes (sub_822F2170); a language keeps its hashes
//    sorted (the lookup, sub_822F2360, is a binary search), so the new one is inserted in order,
//    with its offset, and the string goes at the end of the buffer.
// 2. package\cdd70a8c.p3d, page GameStart_Title.pag (PRESS START; 640x480, y up, boxes by their
//    bottom-left corner): the logo (sprite TitleLogo) moves up a little and a text item, a copy of
//    PressStart in the large menu font (Titans_Large) and a gold colour, shows the new key between
//    the logo and PRESS START. Only the title screen: the main menu stays as it was (the user's
//    choice). Seen while a logo copy was tried on the main menu: with the byte after a sprite's
//    box at 0 (as in the game's data) the image is drawn at a fixed scale from the box's top-left
//    corner, whatever its width and height; at 1 it shrinks towards the box.
//
// CRASH_TITLE_LOGO=off turns all of it off; CRASH_TITLE_LOGO="key=value;..." overrides the layout
// below for tuning (keys: see g_layout).

#include "title_logo.h"
#include "../diagnostics.h"
#include "../fs/rcf_overlay.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace settings
{
    namespace
    {
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

        void InsertLE32(std::vector<uint8_t>& data, size_t at, uint32_t value)
        {
            uint8_t bytes[4];
            StoreLE32(bytes, value);
            data.insert(data.begin() + at, bytes, bytes + 4);
        }

        constexpr const char* BIBLE_NAME = "frontend";
        constexpr const char* KEY_NAME = "FE_Definitive_Edition_Text";
        constexpr const char16_t* EDITION_TEXT = u"Definitive Edition";

        // Page units (640x480, y up); x, y = bottom-left corner of the box.
        struct Layout
        {
            const char* key;
            int value;
        };

        Layout g_layout[] = {
            // The logo's box moves up; the text goes between it and PRESS START (50 to 91).
            { "logo_dy", 22 },
            { "text_x", 120 }, { "text_y", 92 }, { "text_w", 400 }, { "text_h", 41 },
            // The text's font (0 = Titans_Large, 1 = Titans_Small) and colour (0xAARRGGBB).
            { "font_small", 0 },
            { "colour", int(0xFFFFB428) },
        };

        int Value(const char* key)
        {
            for (const Layout& entry : g_layout)
                if (!strcmp(entry.key, key))
                    return entry.value;
            return 0;
        }

        void LoadOverrides()
        {
            const char* text = getenv("CRASH_TITLE_LOGO");
            if (!text || !strchr(text, '='))
                return;
            std::string list = text;
            for (size_t begin = 0; begin < list.size();)
            {
                size_t end = list.find(';', begin);
                if (end == std::string::npos)
                    end = list.size();
                std::string item = list.substr(begin, end - begin);
                size_t equals = item.find('=');
                for (Layout& entry : g_layout)
                    if (equals != std::string::npos && item.substr(0, equals) == entry.key)
                        entry.value = int(strtoll(item.c_str() + equals + 1, nullptr, 0));
                begin = end + 1;
            }
            Log("[pc] title logo: layout overrides \"%s\"", text);
        }

        // ---- Pure3D chunks: u32 id, u32 header size, u32 total size; named chunks start with a
        // length-prefixed name padded to 4 (the length byte counts the padding).

        std::string ChunkName(const std::vector<uint8_t>& data, size_t chunk)
        {
            uint8_t length = data[chunk + 12];
            if (chunk + 13 + length > data.size())
                return {};
            return std::string(reinterpret_cast<const char*>(&data[chunk + 13]), strnlen(reinterpret_cast<const char*>(&data[chunk + 13]), length));
        }

        std::vector<size_t> Children(const std::vector<uint8_t>& data, size_t chunk)
        {
            std::vector<size_t> children;
            size_t end = chunk + LoadLE32(&data[chunk + 8]);
            for (size_t child = chunk + LoadLE32(&data[chunk + 4]); child + 12 <= end;)
            {
                uint32_t size = LoadLE32(&data[child + 8]);
                if (size < 12 || child + size > end)
                    return {};
                children.push_back(child);
                child += size;
            }
            return children;
        }

        // Offsets of the chunks from the file's root to the first chunk `id` named `name` under
        // `chunk` (included).
        bool FindChunk(const std::vector<uint8_t>& data, size_t chunk, uint32_t id, const std::string& name, std::vector<size_t>& path)
        {
            if (chunk + 12 > data.size())
                return false;
            uint32_t header = LoadLE32(&data[chunk + 4]), total = LoadLE32(&data[chunk + 8]);
            if (header < 12 || total < header || chunk + total > data.size())
                return false;
            path.push_back(chunk);
            if (LoadLE32(&data[chunk]) == id && header > 12 && ChunkName(data, chunk) == name)
                return true;
            for (size_t child = chunk + header; child + 12 <= chunk + total;)
            {
                if (FindChunk(data, child, id, name, path))
                    return true;
                uint32_t size = LoadLE32(&data[child + 8]);
                if (size < 12)
                    break;
                child += size;
            }
            path.pop_back();
            return false;
        }

        std::vector<uint8_t> PascalName(const std::string& name)
        {
            std::vector<uint8_t> out(1 + ((name.size() + 3) & ~size_t(3)), 0);
            out[0] = uint8_t(out.size() - 1);
            memcpy(&out[1], name.data(), name.size());
            return out;
        }

        std::vector<uint8_t> MakeChunk(uint32_t id, const std::vector<uint8_t>& header, const std::vector<std::vector<uint8_t>>& children)
        {
            std::vector<uint8_t> chunk(12);
            chunk.insert(chunk.end(), header.begin(), header.end());
            StoreLE32(&chunk[0], id);
            StoreLE32(&chunk[4], uint32_t(chunk.size()));
            for (const auto& child : children)
                chunk.insert(chunk.end(), child.begin(), child.end());
            StoreLE32(&chunk[8], uint32_t(chunk.size()));
            return chunk;
        }

        // Header bytes after the chunk's name.
        std::vector<uint8_t> HeaderTail(const std::vector<uint8_t>& data, size_t chunk)
        {
            size_t begin = chunk + 13 + data[chunk + 12], end = chunk + LoadLE32(&data[chunk + 4]);
            return begin <= end ? std::vector<uint8_t>(data.begin() + begin, data.begin() + end) : std::vector<uint8_t>();
        }

        // Adds `delta` to the total size of the chunks in `path`.
        void Grow(std::vector<uint8_t>& data, const std::vector<size_t>& path, size_t delta)
        {
            for (size_t chunk : path)
                StoreLE32(&data[chunk + 8], LoadLE32(&data[chunk + 8]) + uint32_t(delta));
        }

        // ---- 1. The text key ---------------------------------------------------------------------

        constexpr uint32_t BIBLE = 0x1800D, LANGUAGE = 0x1800E;

        uint32_t KeyHash(const char* name, uint32_t modulo)
        {
            uint32_t hash = 0;
            for (const char* c = name; *c; c++)
                hash = ((hash << 6) + uint32_t(int32_t(int8_t(*c)))) % modulo;
            return hash;
        }

        struct LanguageChunk
        {
            size_t offset;
            std::vector<size_t> ancestors;
        };

        void FindLanguages(const std::vector<uint8_t>& data, size_t begin, size_t end, bool inBible, std::vector<size_t>& path,
                           std::vector<LanguageChunk>& found)
        {
            for (size_t offset = begin; offset + 12 <= end;)
            {
                uint32_t id = LoadLE32(&data[offset]), header = LoadLE32(&data[offset + 4]), total = LoadLE32(&data[offset + 8]);
                if (total < 12 || header < 12 || header > total || offset + total > end)
                    return;
                if (id == LANGUAGE && inBible && header == total)
                    found.push_back({ offset, path });
                bool bible = id == BIBLE && header > 12 && ChunkName(data, offset) == BIBLE_NAME;
                path.push_back(offset);
                FindLanguages(data, offset + header, offset + total, bible, path, found);
                path.pop_back();
                offset += total;
            }
        }

        bool AddKey(std::vector<uint8_t>& data, const char* keyName, const char16_t* (*textOf)(char letter), std::string& letters)
        {
            if (data.size() < 12)
                return false;
            std::vector<LanguageChunk> languages;
            std::vector<size_t> path = { 0 };
            FindLanguages(data, LoadLE32(&data[4]), std::min<size_t>(data.size(), LoadLE32(&data[8])), false, path, languages);
            // Last first: an insertion only moves the chunks after it.
            for (auto it = languages.rbegin(); it != languages.rend(); ++it)
            {
                size_t offset = it->offset, end = offset + LoadLE32(&data[offset + 8]);
                size_t fields = offset + 13 + data[offset + 12];
                if (fields + 13 > end)
                    continue;
                char letter = char(data[fields]);
                uint32_t count = LoadLE32(&data[fields + 1]), modulo = LoadLE32(&data[fields + 5]), bufferSize = LoadLE32(&data[fields + 9]);
                size_t hashes = fields + 13, offsets = hashes + size_t(count) * 4, buffer = offsets + size_t(count) * 4;
                if (modulo == 0 || buffer + bufferSize > end)
                    continue;
                uint32_t key = KeyHash(keyName, modulo);
                uint32_t index = 0;
                bool present = false;
                for (; index < count; index++)
                {
                    uint32_t hash = LoadLE32(&data[hashes + index * 4]);
                    present |= hash == key;
                    if (hash >= key)
                        break;
                }
                if (present)
                    continue;
                std::vector<uint8_t> text;
                for (const char16_t* c = textOf(letter); *c; c++)
                    text.push_back(uint8_t(*c)), text.push_back(uint8_t(*c >> 8));
                text.push_back(0), text.push_back(0);
                // Back to front, so that the earlier offsets stay valid.
                data.insert(data.begin() + buffer + bufferSize, text.begin(), text.end());
                InsertLE32(data, offsets + size_t(index) * 4, bufferSize);
                InsertLE32(data, hashes + size_t(index) * 4, key);
                StoreLE32(&data[fields + 1], count + 1);
                StoreLE32(&data[fields + 9], bufferSize + uint32_t(text.size()));
                size_t delta = text.size() + 8;
                StoreLE32(&data[offset + 4], LoadLE32(&data[offset + 4]) + uint32_t(delta));
                StoreLE32(&data[offset + 8], LoadLE32(&data[offset + 8]) + uint32_t(delta));
                Grow(data, it->ancestors, delta);
                letters.insert(letters.begin(), letter);
            }
            return !letters.empty();
        }

        bool AddEditionKey(std::vector<uint8_t>& data)
        {
            std::string letters;
            if (!AddKey(data, KEY_NAME, [](char) { return EDITION_TEXT; }, letters))
                return false;
            Log("[pc] title logo: %s / %s (0x%X) added to %zu language(s): %s", BIBLE_NAME, KEY_NAME, KeyHash(KEY_NAME, 0x1003F),
                letters.size(), letters.c_str());
            return true;
        }

        // ---- 2. The pages ------------------------------------------------------------------------

        constexpr uint32_t FE_PAGE = 0x18002, FE_LAYER = 0x18020, FE_SPRITE = 0x18022, FE_TEXT = 0x18023,
                           FE_TEXT_REF = 0x1800B;

        // Offsets in the header tail (after the name) of texts and sprites: u32 version, x, y,
        // width, height, one byte, u32 horizontal and vertical alignment, colour (B, G, R, A),
        // two u32; then a text's font name (a sprite's image count and names). Not 4-aligned
        // past the box: an earlier version of this code wrote the font at 40 and the game's
        // text loader crashed on the unknown font.
        constexpr size_t BOX_X = 4, BOX_Y = 8, BOX_W = 12, BOX_H = 16, COLOUR = 29, FONT = 41;

        void SetBox(std::vector<uint8_t>& tail, int x, int y, int w, int h)
        {
            StoreLE32(&tail[BOX_X], uint32_t(x));
            StoreLE32(&tail[BOX_Y], uint32_t(y));
            StoreLE32(&tail[BOX_W], uint32_t(w));
            StoreLE32(&tail[BOX_H], uint32_t(h));
        }

        // A text item showing the new key: a copy of `model` (a text chunk with one text
        // reference) in the chosen font and colour.
        std::vector<uint8_t> EditionText(const std::vector<uint8_t>& data, size_t model, const std::string& name, int x, int y, int w, int h)
        {
            std::vector<uint8_t> tail = HeaderTail(data, model);
            if (tail.size() < FONT + 1 || FONT + 1 + tail[FONT] > tail.size())
                return {};
            SetBox(tail, x, y, w, h);
            uint32_t argb = uint32_t(Value("colour"));
            tail[COLOUR] = uint8_t(argb), tail[COLOUR + 1] = uint8_t(argb >> 8), tail[COLOUR + 2] = uint8_t(argb >> 16), tail[COLOUR + 3] = uint8_t(argb >> 24);
            std::vector<uint8_t> font = PascalName(Value("font_small") ? "Titans_Small" : "Titans_Large");
            std::vector<uint8_t> after(tail.begin() + FONT + 1 + tail[FONT], tail.end());
            tail.resize(FONT);
            tail.insert(tail.end(), font.begin(), font.end());
            tail.insert(tail.end(), after.begin(), after.end());
            std::vector<uint8_t> header = PascalName(name);
            header.insert(header.end(), tail.begin(), tail.end());
            std::vector<uint8_t> refHeader = PascalName(BIBLE_NAME), key = PascalName(KEY_NAME);
            refHeader.insert(refHeader.end(), key.begin(), key.end());
            return MakeChunk(FE_TEXT, header, { MakeChunk(FE_TEXT_REF, refHeader, {}) });
        }

        // Path to the page's first layer, which holds `element`.
        bool PageLayer(const std::vector<uint8_t>& data, const char* page, uint32_t element, const char* elementName,
                       std::vector<size_t>& layerPath, size_t& found)
        {
            std::vector<size_t> pagePath;
            if (!FindChunk(data, 0, FE_PAGE, page, pagePath))
                return false;
            std::vector<size_t> elementPath;
            if (!FindChunk(data, pagePath.back(), element, elementName, elementPath) || elementPath.size() < 3 ||
                LoadLE32(&data[elementPath[elementPath.size() - 2]]) != FE_LAYER)
                return false;
            layerPath = pagePath;
            layerPath.insert(layerPath.end(), elementPath.begin() + 1, elementPath.end() - 1);
            found = elementPath.back();
            return true;
        }

        void Append(std::vector<uint8_t>& data, const std::vector<size_t>& layerPath, const std::vector<uint8_t>& chunk)
        {
            size_t end = layerPath.back() + LoadLE32(&data[layerPath.back() + 8]);
            data.insert(data.begin() + end, chunk.begin(), chunk.end());
            Grow(data, layerPath, chunk.size());
        }

        bool AddTitleText(std::vector<uint8_t>& data)
        {
            if (data.size() < 12)
                return false;
            std::vector<size_t> layer;
            size_t logo = 0, pressStart = 0;
            if (!PageLayer(data, "GameStart_Title.pag", FE_SPRITE, "TitleLogo", layer, logo) ||
                !PageLayer(data, "GameStart_Title.pag", FE_TEXT, "PressStart", layer, pressStart) ||
                Children(data, pressStart).size() != 1 || HeaderTail(data, logo).size() < COLOUR + 4)
            {
                Log("[pc] title logo: title page not as expected; Definitive Edition not added");
                return false;
            }
            std::vector<uint8_t> text = EditionText(data, pressStart, "DefinitiveEditionText", Value("text_x"), Value("text_y"),
                                                    Value("text_w"), Value("text_h"));
            if (text.empty())
                return false;
            size_t logoY = logo + 13 + data[logo + 12] + BOX_Y;
            StoreLE32(&data[logoY], LoadLE32(&data[logoY]) + uint32_t(Value("logo_dy")));
            Append(data, layer, text);  // drawn after the logo and PRESS START
            Log("[pc] title logo: Definitive Edition under the logo of the title screen (logo moved up %d)", Value("logo_dy"));
            return true;
        }

        const char* const BIBLE_PACKAGES[] = { "package\\cdd70a8c.p3d", "package\\7efdcd91.p3d" };
    }

    bool AddFrontendTextKey(std::vector<uint8_t>& data, const char* key, const char16_t* (*textOf)(char letter))
    {
        std::string letters;
        if (!AddKey(data, key, textOf, letters))
            return false;
        Log("[pc] frontend: %s / %s (0x%X) added to %zu language(s): %s", BIBLE_NAME, key, KeyHash(key, 0x1003F), letters.size(),
            letters.c_str());
        return true;
    }

    const char* const* FrontendBiblePackages(size_t& count)
    {
        count = sizeof(BIBLE_PACKAGES) / sizeof(BIBLE_PACKAGES[0]);
        return BIBLE_PACKAGES;
    }

    void RegisterTitleLogoPatches()
    {
        if (const char* setting = getenv("CRASH_TITLE_LOGO"); setting && !strcmp(setting, "off"))
        {
            Log("[pc] title logo: off (CRASH_TITLE_LOGO=off)");
            return;
        }
        LoadOverrides();
        for (const char* package : BIBLE_PACKAGES)
            rcf_overlay::RegisterEntryRewrite("default", package, "texto Definitive Edition", AddEditionKey);
        rcf_overlay::RegisterEntryRewrite("default", "package\\cdd70a8c.p3d", "logo Definitive Edition", AddTitleText);
    }
}
