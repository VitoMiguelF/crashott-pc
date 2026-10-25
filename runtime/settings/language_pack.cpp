#include "language_pack.h"
#include "language_font.h"
#include "settings.h"
#include "../diagnostics.h"
#include "../fs/rcf_overlay.h"

#include "../platform/text.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace settings
{
    namespace
    {
        struct Pack
        {
            std::string id, name, base, archive = "default", folder, help1, help2;
            fs::path root;
            std::vector<std::string> packages;  // lowercase, '\'-separated entry names
            std::vector<language_font::Recipe> glyphs;  // composed into the fonts of the packages
        };

        // Stable addresses: Languages() points into these strings.
        std::deque<Pack> g_packs;

        // Texts of one bible: lines for every package, and lines for some packages only.
        struct BibleTexts
        {
            std::unordered_map<uint32_t, std::u16string> common;
            std::map<std::string, std::unordered_map<uint32_t, std::u16string>> byPackage;
        };
        std::map<std::string, BibleTexts> g_texts;  // by bible name ("frontend")

        std::string Trim(std::string text)
        {
            size_t begin = text.find_first_not_of(" \t\r\n");
            size_t end = text.find_last_not_of(" \t\r\n");
            return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
        }

        std::string Lower(std::string text)
        {
            for (char& c : text)
                c = char(std::tolower(uint8_t(c)));
            return text;
        }

        std::string EntryName(std::string name)
        {
            std::replace(name.begin(), name.end(), '/', '\\');
            return Lower(Trim(name));
        }

        // The quoted strings of a value: "a" or ["a", "b"].
        std::vector<std::string> Strings(const std::string& value)
        {
            std::vector<std::string> out;
            for (size_t pos = value.find('"'); pos != std::string::npos; pos = value.find('"', pos + 1))
            {
                size_t close = value.find('"', pos + 1);
                if (close == std::string::npos)
                    break;
                out.push_back(value.substr(pos + 1, close - pos - 1));
                pos = close;
            }
            return out;
        }

        std::u16string Utf16(const std::string& utf8)
        {
            return plat::Utf16FromUtf8(utf8);
        }

        // language.toml: key = "value" lines; packages = [...] may span lines.
        bool ReadPack(const fs::path& file, Pack& pack)
        {
            std::ifstream in(file);
            if (!in)
                return false;
            std::string line, key, value;
            while (std::getline(in, line))
            {
                std::string trimmed = Trim(line);
                if (key.empty())
                {
                    if (trimmed.empty() || trimmed[0] == '#')
                        continue;
                    size_t equals = trimmed.find('=');
                    if (equals == std::string::npos)
                        continue;
                    key = Lower(Trim(trimmed.substr(0, equals)));
                    value = Trim(trimmed.substr(equals + 1));
                }
                else
                {
                    value += " " + trimmed;
                }
                // A '#' outside quotes ends the value; an open '[' continues on the next line.
                bool quoted = false;
                for (size_t i = 0; i < value.size(); i++)
                {
                    if (value[i] == '"')
                        quoted = !quoted;
                    else if (value[i] == '#' && !quoted)
                    {
                        value = Trim(value.substr(0, i));
                        break;
                    }
                }
                if (!value.empty() && value[0] == '[' && value.find(']') == std::string::npos)
                    continue;
                std::vector<std::string> strings = Strings(value);
                std::string first = strings.empty() ? std::string() : strings[0];
                if (key == "id")
                    pack.id = Lower(first);
                else if (key == "name")
                    pack.name = first;
                else if (key == "base")
                    pack.base = Lower(first);
                else if (key == "archive")
                    pack.archive = Lower(first);
                else if (key == "help")
                {
                    pack.help1 = strings.size() > 0 ? strings[0] : "";
                    pack.help2 = strings.size() > 1 ? strings[1] : "";
                }
                else if (key == "packages")
                {
                    for (const std::string& name : strings)
                        pack.packages.push_back(EntryName(name));
                }
                else if (key == "glyphs")
                {
                    for (const std::string& text : strings)
                    {
                        language_font::Recipe recipe;
                        if (language_font::ParseRecipe(Utf16(text), recipe))
                            pack.glyphs.push_back(recipe);
                        else
                            Log("[language] %s: glyph recipe \"%s\" is not \"x = base + accent - accent base\" (ignored)", file.string().c_str(), text.c_str());
                    }
                }
                key.clear();
            }
            return true;
        }

        // tools/texts.py escapes: \n \r \t \\ and \uXXXX (one UTF-16 unit).
        std::u16string Unescape(const std::u16string& text)
        {
            std::u16string out;
            for (size_t i = 0; i < text.size(); i++)
            {
                if (text[i] != u'\\' || i + 1 >= text.size())
                {
                    out += text[i];
                    continue;
                }
                char16_t next = text[i + 1];
                if (next == u'n' || next == u'r' || next == u't' || next == u'\\')
                {
                    out += next == u'n' ? u'\n' : next == u'r' ? u'\r' : next == u't' ? u'\t' : u'\\';
                    i++;
                }
                else if (next == u'u' && i + 6 <= text.size() &&
                         std::all_of(text.begin() + i + 2, text.begin() + i + 6, [](char16_t c) { return c < 128 && isxdigit(int(c)); }))
                {
                    std::string hex(text.begin() + i + 2, text.begin() + i + 6);
                    out += char16_t(strtoul(hex.c_str(), nullptr, 16));
                    i += 5;
                }
                else
                    out += text[i];
            }
            return out;
        }

        // texts\<bible>.txt -> g_texts[bible]. Returns the number of texts read.
        size_t ReadTexts(const fs::path& file, BibleTexts& texts)
        {
            std::ifstream in(file, std::ios::binary);
            std::string line;
            std::vector<std::unordered_map<uint32_t, std::u16string>*> section{ &texts.common };
            size_t count = 0;
            int number = 0;
            while (std::getline(in, line))
            {
                number++;
                if (number == 1 && line.size() >= 3 && memcmp(line.data(), "\xEF\xBB\xBF", 3) == 0)
                    line.erase(0, 3);  // UTF-8 BOM
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                std::string trimmed = Trim(line);
                if (trimmed.empty() || trimmed[0] == '#')
                    continue;
                if (trimmed[0] == '[' && trimmed.back() == ']')
                {
                    std::string list = trimmed.substr(1, trimmed.size() - 2);
                    std::replace(list.begin(), list.end(), ',', ' ');
                    std::istringstream names(list);
                    std::string name;
                    section.clear();
                    while (names >> name)
                        section.push_back(&texts.byPackage[EntryName(name)]);
                    if (section.empty())
                        section.push_back(&texts.common);
                    continue;
                }
                size_t separator = line.find(" = ");
                char* end = nullptr;
                uint32_t hash = separator == std::string::npos ? 0 : uint32_t(strtoul(Trim(line.substr(0, separator)).c_str(), &end, 16));
                if (separator == std::string::npos || !end || *end)
                {
                    Log("[language] %s:%d: expected 0xHASH = text (line ignored)", file.filename().string().c_str(), number);
                    continue;
                }
                std::u16string text = Unescape(Utf16(line.substr(separator + 3)));
                for (auto* target : section)
                    (*target)[hash] = text;
                count++;
            }
            return count;
        }

        // Pure3D (little-endian): u32 id, u32 header size, u32 total size; children follow the
        // header. Text bibles (tools/texts.py): 0x1800D = name + one 0x1800E child per language:
        // pascal name, u8 letter, u32 count, u32 hash modulo, u32 buffer size, u32 hashes[count],
        // u32 offsets[count] (bytes into the buffer), NUL-terminated UTF-16LE strings.
        constexpr uint32_t BIBLE = 0x1800D, LANGUAGE = 0x1800E, FONT = 0x22000;

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

        struct Rewrite
        {
            const std::vector<uint8_t>& data;
            const std::string& package;
            char letter;
            const std::vector<language_font::Recipe>& glyphs;
            int languages = 0;  // language chunks rebuilt
            size_t texts = 0;   // texts replaced
            int composed = 0;   // glyphs added to fonts
            std::string fonts;  // what was composed, for the log
        };

        // Appends language chunk [offset, offset + size) of `bible` with the pack's texts.
        void RebuildLanguage(Rewrite& r, size_t offset, size_t size, const BibleTexts& texts, std::vector<uint8_t>& out)
        {
            const uint8_t* data = r.data.data();
            size_t end = offset + size;
            size_t pos = offset + 12;
            size_t fields = pos + 1 + data[pos];
            if (LoadLE32(data + offset + 4) != size || fields + 13 > end || char(data[fields]) != r.letter)
            {
                out.insert(out.end(), data + offset, data + end);
                return;
            }
            uint32_t count = LoadLE32(data + fields + 1), modulo = LoadLE32(data + fields + 5), bufferSize = LoadLE32(data + fields + 9);
            size_t hashes = fields + 13, offsets = hashes + size_t(count) * 4, buffer = offsets + size_t(count) * 4;
            if (buffer + bufferSize > end)
            {
                out.insert(out.end(), data + offset, data + end);
                return;
            }
            auto onlyHere = texts.byPackage.find(r.package);
            std::vector<std::u16string> strings(count);
            size_t replaced = 0;
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t hash = LoadLE32(data + hashes + i * 4);
                const std::u16string* text = nullptr;
                if (onlyHere != texts.byPackage.end())
                {
                    auto it = onlyHere->second.find(hash);
                    if (it != onlyHere->second.end())
                        text = &it->second;
                }
                if (!text)
                {
                    auto it = texts.common.find(hash);
                    if (it != texts.common.end())
                        text = &it->second;
                }
                if (text)
                {
                    strings[i] = *text;
                    replaced++;
                    continue;
                }
                uint32_t start = LoadLE32(data + offsets + i * 4);
                for (size_t at = start; at + 1 < bufferSize; at += 2)
                {
                    char16_t c = char16_t(data[buffer + at] | (data[buffer + at + 1] << 8));
                    if (!c)
                        break;
                    strings[i] += c;
                }
            }
            if (!replaced)
            {
                out.insert(out.end(), data + offset, data + end);
                return;
            }
            // New buffer: each distinct string once.
            std::vector<uint8_t> newBuffer;
            std::vector<uint32_t> newOffsets(count);
            std::map<std::u16string, uint32_t> written;
            for (uint32_t i = 0; i < count; i++)
            {
                auto [it, added] = written.emplace(strings[i], uint32_t(newBuffer.size()));
                if (added)
                {
                    for (char16_t c : strings[i])
                        newBuffer.push_back(uint8_t(c)), newBuffer.push_back(uint8_t(c >> 8));
                    newBuffer.push_back(0), newBuffer.push_back(0);
                }
                newOffsets[i] = it->second;
            }
            size_t start = out.size();
            out.insert(out.end(), data + offset, data + fields + 1);  // header, pascal name, letter
            AppendLE32(out, count);
            AppendLE32(out, modulo);
            AppendLE32(out, uint32_t(newBuffer.size()));
            out.insert(out.end(), data + hashes, data + offsets);
            for (uint32_t value : newOffsets)
                AppendLE32(out, value);
            out.insert(out.end(), newBuffer.begin(), newBuffer.end());
            uint32_t newSize = uint32_t(out.size() - start);
            StoreLE32(&out[start + 4], newSize);
            StoreLE32(&out[start + 8], newSize);
            r.languages++;
            r.texts += replaced;
        }

        // Appends the chunks in [begin, end) (children of a bible named `bible`, or "").
        void RewriteChunks(Rewrite& r, size_t begin, size_t end, const BibleTexts* bible, std::vector<uint8_t>& out)
        {
            const uint8_t* data = r.data.data();
            size_t offset = begin;
            while (offset + 12 <= end)
            {
                uint32_t id = LoadLE32(data + offset), headerSize = LoadLE32(data + offset + 4), totalSize = LoadLE32(data + offset + 8);
                if (totalSize < 12 || headerSize < 12 || headerSize > totalSize || offset + totalSize > end)
                    break;
                if (id == LANGUAGE && bible)
                    RebuildLanguage(r, offset, totalSize, *bible, out);
                else if (id == FONT && !r.glyphs.empty())
                    r.composed += language_font::ComposeFont(data + offset, totalSize, r.glyphs, out, r.fonts);
                else
                {
                    const BibleTexts* child = nullptr;
                    if (id == BIBLE && offset + 13 <= end)
                    {
                        uint8_t length = data[offset + 12];
                        std::string name(reinterpret_cast<const char*>(data + offset + 13),
                                         strnlen(reinterpret_cast<const char*>(data + offset + 13), std::min<size_t>(length, end - offset - 13)));
                        auto it = g_texts.find(Lower(name));
                        child = it == g_texts.end() ? nullptr : &it->second;
                    }
                    size_t start = out.size();
                    out.insert(out.end(), data + offset, data + offset + headerSize);
                    RewriteChunks(r, offset + headerSize, offset + totalSize, child, out);
                    StoreLE32(&out[start + 8], uint32_t(out.size() - start));
                }
                offset += totalSize;
            }
            out.insert(out.end(), data + offset, data + end);  // anything that isn't a chunk
        }

        bool RewritePackage(std::vector<uint8_t>& data, const std::string& package, char letter, const std::vector<language_font::Recipe>& glyphs)
        {
            if (data.size() < 12)
                return false;
            Rewrite r{ data, package, letter, glyphs };
            std::vector<uint8_t> out(data.begin(), data.begin() + 12);  // P3D file header
            RewriteChunks(r, 12, data.size(), nullptr, out);
            if (!r.fonts.empty())
                Log("[language] %s fonts: %s", package.c_str(), r.fonts.c_str());
            if (!r.languages && !r.composed)
                return false;
            StoreLE32(&out[8], uint32_t(int64_t(LoadLE32(&data[8])) + int64_t(out.size()) - int64_t(data.size())));
            Log("[language] %s: %zu text(s) in %d bible language chunk(s), %d glyph(s) composed", package.c_str(), r.texts, r.languages,
                r.composed);
            data = std::move(out);
            return true;
        }
    }

    void LoadLanguagePacks(const fs::path& dir)
    {
        std::error_code error;
        if (!fs::is_directory(dir, error))
            return;
        std::vector<fs::path> folders;
        for (auto& item : fs::directory_iterator(dir, error))
        {
            if (item.is_directory(error) && fs::exists(item.path() / "language.toml", error) && item.path().filename().wstring()[0] != L'_')
                folders.push_back(item.path());
        }
        std::sort(folders.begin(), folders.end());
        for (const fs::path& folder : folders)
        {
            Pack pack;
            pack.root = folder;
            pack.folder = reinterpret_cast<const char*>(folder.u8string().c_str());
            if (!ReadPack(folder / "language.toml", pack) || pack.id.empty() || pack.name.empty() || pack.packages.empty())
            {
                Log("[language] %s\\language.toml: needs id, name and packages; pack ignored", folder.string().c_str());
                continue;
            }
            if (pack.base.empty())
                pack.base = "english";
            const Language base = LanguageById(pack.base);  // a copy: AddLanguage grows the list
            if (pack.base != base.id || base.pack)
            {
                Log("[language] %s: unknown base language \"%s\"; pack ignored", pack.id.c_str(), pack.base.c_str());
                continue;
            }
            Pack& stored = g_packs.emplace_back(std::move(pack));
            Language language = base;
            language.id = stored.id.c_str();
            language.name = stored.name.c_str();
            language.pack = stored.folder.c_str();
            language.help[0] = stored.help1.empty() ? nullptr : stored.help1.c_str();
            language.help[1] = stored.help2.empty() ? nullptr : stored.help2.c_str();
            if (!AddLanguage(language))
            {
                Log("[language] %s: id already used by another language; pack ignored", stored.id.c_str());
                g_packs.pop_back();
                continue;
            }
            Log("[language] pack %s (%s): base %s, %zu package(s) in %s.rcf", stored.id.c_str(), stored.name.c_str(), base.id,
                stored.packages.size(), stored.archive.c_str());
        }
    }

    void RegisterLanguagePack()
    {
        const Language& language = StartupLanguage();
        if (!language.pack)
            return;
        auto pack = std::find_if(g_packs.begin(), g_packs.end(), [&](const Pack& p) { return p.id == language.id; });
        if (pack == g_packs.end())
            return;
        std::error_code error;
        size_t total = 0;
        for (auto& item : fs::directory_iterator(pack->root / "texts", error))
        {
            if (!item.is_regular_file(error) || Lower(item.path().extension().string()) != ".txt")
                continue;
            std::string bible = Lower(item.path().stem().string());
            total += ReadTexts(item.path(), g_texts[bible]);
        }
        if (!total)
        {
            Log("[language] %s: no texts in %s\\texts; the game shows %s", pack->id.c_str(), pack->root.string().c_str(), pack->base.c_str());
            return;
        }
        char letter = language.letter;
        const std::vector<language_font::Recipe>* glyphs = &pack->glyphs;
        for (const std::string& package : pack->packages)
        {
            rcf_overlay::RegisterEntryRewrite(pack->archive, package, "idioma " + pack->id, [package, letter, glyphs](std::vector<uint8_t>& data) {
                return RewritePackage(data, package, letter, *glyphs);
            });
        }
        Log("[language] %s: %zu text(s) from %zu bible file(s), texts of letter %c rewritten in %zu package(s); voices %s",
            pack->id.c_str(), total, g_texts.size(), letter, pack->packages.size(), language.voiceRcf);
    }
}
