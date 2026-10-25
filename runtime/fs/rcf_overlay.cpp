#include "rcf_overlay.h"
#include "../diagnostics.h"

#include "../platform/file.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace rcf_overlay
{
    constexpr char MAGIC[] = "ATG CORE CEMENT LIBRARY";
    constexpr uint64_t ALIGNMENT = 0x800;
    // The game's own reader (core::XenonDrive::Read) seeks with SetFilePointer(file, offset, NULL,
    // FILE_BEGIN), which sign-extends the offset: it can't reach 2 GB. fs/xenon_drive_hooks.cpp
    // makes it unsigned; directory offsets are u32, so 4 GB is the hard end.
    constexpr uint64_t SIGNED_LIMIT = 0x80000000ull;
    constexpr uint64_t UNSIGNED_LIMIT = 0x100000000ull;

    enum class Placement
    {
        InPlace,  // over the entry's own original slot
        Freed,    // in space freed by other replaced entries
        Appended, // past the original end
    };

    struct Replacement
    {
        uint64_t offset{};  // in the virtual archive
        uint64_t size{};
        plat::File file;                     // a mod's file, or
        std::vector<uint8_t> data;           // the entry rebuilt by rewrites/patches (built-ins)
        std::string entry, source;           // for the log
        uint32_t index{};                    // directory record
        uint32_t originalOffset{}, originalSize{};
        Placement placement = Placement::Appended;
        std::unique_ptr<std::atomic<bool>> read = std::make_unique<std::atomic<bool>>(false);
    };

    struct BytePatch
    {
        uint64_t offset{};  // in the archive
        std::vector<uint8_t> bytes;
    };

    class Archive
    {
    public:
        uint64_t baseSize{};
        uint64_t size{};
        uint32_t directoryOffset{};
        std::vector<uint8_t> directory;         // patched copy of the original directory
        std::vector<Replacement> replacements;  // sorted by offset, never overlapping each other
        std::vector<BytePatch> patches;         // built-in patches of entries left in place, sorted
    };

    namespace
    {
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

        uint32_t LoadLE32(const uint8_t* p)
        {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }

        uint64_t AlignUp(uint64_t value)
        {
            return (value + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        }

        std::string Lower(std::string text)
        {
            for (char& c : text)
                c = char(std::tolower(uint8_t(c)));
            return text;
        }

        std::string Gb(uint64_t bytes)
        {
            char text[32];
            snprintf(text, sizeof(text), "%.2f GB", double(bytes) / double(1ull << 30));
            return text;
        }

        bool ReadAt(const plat::File& file, uint64_t offset, void* data, uint32_t length, uint32_t& transferred)
        {
            size_t read = 0;
            bool ok = file.ReadAt(offset, data, length, read);
            transferred = uint32_t(read);
            return ok;
        }

        bool ReadExact(const plat::File& file, uint64_t offset, void* data, uint32_t length)
        {
            return file.ReadExactAt(offset, data, length);
        }

        struct Index
        {
            std::unordered_map<std::string, uint32_t> byName;  // lowercase name -> directory record
            uint32_t namesOffset{}, namesSize{};
        };

        // Directory index of each entry, keyed by lowercase name ("package\7efdcd91.p3d").
        // Names are listed in data-offset order, so the i-th name is the entry with the i-th
        // smallest offset (tools/rcf.py).
        bool ReadIndex(const plat::File& file, Archive& archive, Index& index)
        {
            uint8_t header[0x3C];
            if (!ReadExact(file, 0, header, sizeof(header)) || memcmp(header, MAGIC, sizeof(MAGIC) - 1) != 0)
                return false;
            archive.directoryOffset = LoadBE32(header + 0x24);
            uint32_t directorySize = LoadBE32(header + 0x28);
            index.namesOffset = LoadBE32(header + 0x2C);
            index.namesSize = LoadBE32(header + 0x30);
            uint32_t count = LoadBE32(header + 0x38);
            if (directorySize != count * 12 || index.namesSize < 12)
                return false;

            archive.directory.resize(directorySize);
            std::vector<uint8_t> names(index.namesSize);
            if (!ReadExact(file, archive.directoryOffset, archive.directory.data(), directorySize) ||
                !ReadExact(file, index.namesOffset, names.data(), index.namesSize))
                return false;

            std::vector<uint32_t> byOffset(count);
            for (uint32_t i = 0; i < count; i++)
                byOffset[i] = i;
            std::sort(byOffset.begin(), byOffset.end(), [&](uint32_t a, uint32_t b) {
                return LoadBE32(&archive.directory[a * 12 + 4]) < LoadBE32(&archive.directory[b * 12 + 4]);
            });

            size_t pos = 12;
            for (uint32_t i = 0; i < count; i++)
            {
                if (pos + 12 > names.size())
                    return false;
                uint32_t length = LoadLE32(&names[pos + 8]);
                pos += 12;
                if (pos + length > names.size())
                    return false;
                std::string name(reinterpret_cast<const char*>(&names[pos]), strnlen(reinterpret_cast<const char*>(&names[pos]), length));
                index.byName.emplace(Lower(name), byOffset[i]);
                pos += length + 3 + 4;  // padding, timestamp
            }
            return true;
        }

        struct BuiltinPatch
        {
            std::string archive, entry, description;
            EntryPatcher patcher;
            bool resizes = false;  // RegisterEntryRewrite
        };

        std::mutex g_builtinMutex;
        std::vector<BuiltinPatch> g_builtins;

        // Applies the built-in patches and rewrites of `stem` to the entries in `index`, as the
        // virtual archive serves them: the original data, or the mod's file for an entry a mod
        // replaces (`replaced`: directory record -> its replacement). Each entry goes through its
        // patchers in registration order. A replaced entry, or a resized result, becomes an
        // in-memory replacement (laid out with the others afterwards); an original entry patched
        // at its own size becomes byte patches at its original offset.
        void ApplyBuiltins(const plat::File& file, const std::string& stem, const std::string& display, Archive& archive, const Index& index,
                           std::unordered_map<uint32_t, size_t>& replaced)
        {
            std::lock_guard lock(g_builtinMutex);
            std::vector<std::string> entries;  // in order of first registration
            for (const BuiltinPatch& builtin : g_builtins)
            {
                if (builtin.archive == stem && std::find(entries.begin(), entries.end(), builtin.entry) == entries.end())
                    entries.push_back(builtin.entry);
            }
            for (const std::string& entry : entries)
            {
                auto it = index.byName.find(entry);
                if (it == index.byName.end())
                {
                    Log("[fs] rcf overlay: %s has no %s; its built-in patches are skipped", display.c_str(), entry.c_str());
                    continue;
                }
                const uint8_t* record = &archive.directory[it->second * 12];
                uint32_t offset = LoadBE32(record + 4), size = LoadBE32(record + 8);
                auto mod = replaced.find(it->second);
                Replacement* modReplacement = mod != replaced.end() ? &archive.replacements[mod->second] : nullptr;
                std::vector<uint8_t> original;
                bool read = false;
                std::string source;  // " (mod X)" for a replaced entry
                if (modReplacement)
                {
                    original.resize(size_t(modReplacement->size));
                    read = modReplacement->file.IsOpen()
                               ? ReadExact(modReplacement->file, 0, original.data(), uint32_t(original.size()))
                               : (original = modReplacement->data, true);
                    source = " (" + modReplacement->source + ")";
                }
                else
                {
                    original.resize(size);
                    read = uint64_t(offset) + size <= archive.baseSize && ReadExact(file, offset, original.data(), size);
                }
                if (!read)
                {
                    Log("[fs] rcf overlay: could not read %s %s%s; its built-in patches are skipped", display.c_str(), entry.c_str(), source.c_str());
                    continue;
                }

                std::vector<uint8_t> patched = original;
                std::string applied;  // descriptions, for the served entry's log
                for (const BuiltinPatch& builtin : g_builtins)
                {
                    if (builtin.archive != stem || builtin.entry != entry)
                        continue;
                    std::vector<uint8_t> next = patched;
                    if (!builtin.patcher(next) || (!builtin.resizes && next.size() != patched.size()))
                    {
                        Log("[fs] rcf overlay: %s %s%s: built-in patch \"%s\" not applied (nothing to change or unexpected contents)", display.c_str(),
                            entry.c_str(), source.c_str(), builtin.description.c_str());
                        continue;
                    }
                    if (builtin.resizes)
                        Log("[fs] rcf overlay: %s %s%s: rewrite \"%s\" (%zu -> %zu bytes)", display.c_str(), entry.c_str(), source.c_str(),
                            builtin.description.c_str(), patched.size(), next.size());
                    patched = std::move(next);
                    applied += (applied.empty() ? "" : ", ") + builtin.description;
                }
                if (applied.empty())
                    continue;

                if (modReplacement || patched.size() != original.size())
                {
                    if (patched.size() > 0xFFFFFFFFull)
                    {
                        Log("[fs] rcf overlay: %s %s: rewritten entry is too big for the archive; left unchanged", display.c_str(), entry.c_str());
                        continue;
                    }
                    if (!modReplacement)
                    {
                        replaced.emplace(it->second, archive.replacements.size());
                        modReplacement = &archive.replacements.emplace_back();
                        modReplacement->entry = entry;
                        modReplacement->index = it->second;
                        modReplacement->originalOffset = offset;
                        modReplacement->originalSize = size;
                    }
                    else if (modReplacement->file.IsOpen())
                    {
                        modReplacement->file.Close();
                    }
                    if (patched.size() == original.size())
                        Log("[fs] rcf overlay: %s %s%s: built-in patch(es) \"%s\"", display.c_str(), entry.c_str(), source.c_str(), applied.c_str());
                    modReplacement->size = patched.size();
                    modReplacement->data = std::move(patched);
                    modReplacement->source = applied + source;
                    continue;
                }
                // Changed runs (runs closer than 16 bytes are merged).
                size_t changed = 0, runs = 0;
                for (size_t i = 0; i < size;)
                {
                    if (patched[i] == original[i])
                    {
                        i++;
                        continue;
                    }
                    size_t runEnd = i + 1, same = 0;
                    for (size_t j = runEnd; j < size && same < 16; j++)
                    {
                        if (patched[j] != original[j])
                            runEnd = j + 1, same = 0;
                        else
                            same++;
                    }
                    archive.patches.push_back({ uint64_t(offset) + i, std::vector<uint8_t>(patched.begin() + i, patched.begin() + runEnd) });
                    changed += runEnd - i;
                    runs++;
                    i = runEnd;
                }
                Log("[fs] rcf overlay: %s %s%s: built-in patch(es) \"%s\" (%zu bytes in %zu run(s))", display.c_str(), entry.c_str(),
                    source.c_str(), applied.c_str(), changed, runs);
            }
            std::sort(archive.patches.begin(), archive.patches.end(), [](const BytePatch& a, const BytePatch& b) { return a.offset < b.offset; });
        }

        bool HasBuiltins(const std::string& stem)
        {
            std::lock_guard lock(g_builtinMutex);
            return std::any_of(g_builtins.begin(), g_builtins.end(), [&](const BuiltinPatch& b) { return b.archive == stem; });
        }

        // Places every replacement and points the directory at it. The original file stays in
        // place except where it held replaced entries: the header (first 0x800 bytes), directory,
        // name table and every entry that is not replaced are kept, and the rest of the original
        // span is free. Each replacement goes, in this order of preference:
        //   1. over its own original slot, when it fits in the free run that starts there;
        //   2. in the free run (inside the original span) it fits best, biggest entries first;
        //   3. after the original end, packed.
        // So the virtual archive only grows by what the freed space can't take. Returns its size.
        uint64_t Layout(Archive& archive, const Index& index, uint64_t& tail)
        {
            // Kept ranges of the original, then the free runs between them.
            std::vector<std::pair<uint64_t, uint64_t>> kept;
            kept.emplace_back(0, ALIGNMENT);
            kept.emplace_back(archive.directoryOffset, uint64_t(archive.directoryOffset) + archive.directory.size());
            kept.emplace_back(index.namesOffset, uint64_t(index.namesOffset) + index.namesSize);
            std::vector<bool> isReplaced(archive.directory.size() / 12, false);
            for (const Replacement& r : archive.replacements)
                isReplaced[r.index] = true;
            for (size_t i = 0; i < isReplaced.size(); i++)
            {
                if (isReplaced[i])
                    continue;
                uint64_t offset = LoadBE32(&archive.directory[i * 12 + 4]), size = LoadBE32(&archive.directory[i * 12 + 8]);
                if (size)
                    kept.emplace_back(offset, offset + size);
            }
            std::sort(kept.begin(), kept.end());
            uint64_t spanEnd = AlignUp(archive.baseSize);
            std::vector<std::pair<uint64_t, uint64_t>> free;  // [begin, end), sorted, aligned begins
            uint64_t cursor = 0;
            for (auto& [lo, hi] : kept)
            {
                uint64_t begin = AlignUp(cursor);
                if (lo > begin)
                    free.emplace_back(begin, std::min(lo, spanEnd));
                cursor = std::max(cursor, hi);
            }
            if (AlignUp(cursor) < spanEnd)
                free.emplace_back(AlignUp(cursor), spanEnd);
            uint64_t freedBytes = 0;
            for (auto& [lo, hi] : free)
                freedBytes += hi - lo;

            auto take = [&](size_t run, uint64_t at, uint64_t size) {
                auto [lo, hi] = free[run];
                uint64_t end = std::min(AlignUp(at + size), hi);
                free.erase(free.begin() + run);
                if (end < hi)
                    free.insert(free.begin() + run, { end, hi });
                if (lo < at)
                    free.insert(free.begin() + run, { lo, at });
            };
            std::vector<Replacement*> pending;
            for (Replacement& r : archive.replacements)
            {
                auto run = std::find_if(free.begin(), free.end(), [&](auto& f) { return f.first <= r.originalOffset && r.originalOffset < f.second; });
                if (run != free.end() && r.originalOffset % ALIGNMENT == 0 && r.originalOffset + r.size <= run->second)
                {
                    r.offset = r.originalOffset;
                    r.placement = Placement::InPlace;
                    take(size_t(run - free.begin()), r.offset, r.size);
                }
                else
                    pending.push_back(&r);
            }
            std::stable_sort(pending.begin(), pending.end(), [](const Replacement* a, const Replacement* b) { return a->size > b->size; });
            for (Replacement* r : pending)
            {
                size_t best = free.size();
                for (size_t i = 0; i < free.size(); i++)
                {
                    uint64_t room = free[i].second - free[i].first;
                    if (room >= r->size && (best == free.size() || room < free[best].second - free[best].first))
                        best = i;
                }
                if (best != free.size())
                {
                    r->offset = free[best].first;
                    r->placement = Placement::Freed;
                    take(best, r->offset, r->size);
                }
                else
                {
                    r->offset = tail;
                    r->placement = Placement::Appended;
                    tail = AlignUp(tail + r->size);
                }
            }
            std::sort(archive.replacements.begin(), archive.replacements.end(),
                      [](const Replacement& a, const Replacement& b) { return a.offset < b.offset; });
            uint64_t used = 0;
            for (auto& [lo, hi] : free)
                used += hi - lo;
            return freedBytes - used;  // freed bytes that now hold replacements
        }

        std::shared_ptr<const Archive> Build(const vfs::VirtualPath& path, const fs::path& host)
        {
            // Mod folders that hold replacements for this archive, in load order.
            std::string stem = Lower(fs::path(path.relative).stem().string());
            std::vector<std::pair<std::string, fs::path>> folders;
            std::error_code error;
            for (auto& mod : vfs::Mods())
            {
                fs::path folder = mod.root / "rcf" / stem;
                if (fs::is_directory(folder, error))
                    folders.emplace_back(mod.name, folder);
            }
            bool builtins = HasBuiltins(stem);
            if (folders.empty() && !builtins)
                return nullptr;

            plat::File file;
            if (!file.Open(host))
                return nullptr;
            auto archive = std::make_shared<Archive>();
            Index index;
            uint64_t size = file.Size();
            bool ok = ReadIndex(file, *archive, index);
            if (!ok)
            {
                Log("[fs] rcf overlay: %s is not a readable RCF archive, mod entries ignored", path.Display().c_str());
                return nullptr;
            }
            archive->baseSize = size;
            std::string display = path.Display();

            // First mod in load order wins for each entry.
            std::map<std::string, std::pair<std::string, fs::path>> chosen;  // entry name -> (mod, file)
            for (auto& [modName, folder] : folders)
            {
                for (auto it = fs::recursive_directory_iterator(folder, error); !error && it != fs::recursive_directory_iterator(); it.increment(error))
                {
                    if (!it->is_regular_file(error))
                        continue;
                    std::string entry = Lower(fs::relative(it->path(), folder, error).string());
                    std::replace(entry.begin(), entry.end(), '/', '\\');
                    if (!index.byName.count(entry))
                    {
                        Log("[fs] rcf overlay: mod %s has %s, which is not an entry of %s (ignored: only existing entries can be replaced)",
                            modName.c_str(), entry.c_str(), display.c_str());
                        continue;
                    }
                    chosen.emplace(entry, std::make_pair(modName, it->path()));
                }
            }
            std::unordered_map<uint32_t, size_t> replaced;  // directory record -> archive->replacements index
            for (auto& [entry, source] : chosen)
            {
                Replacement replacement;
                uint64_t fileSize = replacement.file.Open(source.second) ? replacement.file.Size() : 0;
                if (!replacement.file.IsOpen() || fileSize > 0xFFFFFFFFull)
                {
                    Log("[fs] rcf overlay: could not use %s (mod %s)", source.second.string().c_str(), source.first.c_str());
                    continue;
                }
                replacement.index = index.byName[entry];
                const uint8_t* record = &archive->directory[replacement.index * 12];
                replacement.originalOffset = LoadBE32(record + 4);
                replacement.originalSize = LoadBE32(record + 8);
                replacement.size = fileSize;
                replacement.entry = entry;
                replacement.source = "mod " + source.first;
                replaced.emplace(replacement.index, archive->replacements.size());
                archive->replacements.push_back(std::move(replacement));
            }
            // After the mod files, so that a replaced entry is patched as the mod has it.
            if (builtins)
                ApplyBuiltins(file, stem, display, *archive, index, replaced);
            file.Close();
            if (archive->replacements.empty() && archive->patches.empty())
                return nullptr;

            // Entries that would end past 4 GB can't be addressed by the u32 directory: they are
            // dropped (the original entry is served, so its slot must stay) and the layout redone.
            uint64_t reused = 0;
            for (;;)
            {
                uint64_t tail = AlignUp(archive->baseSize);
                reused = Layout(*archive, index, tail);
                auto over = std::stable_partition(archive->replacements.begin(), archive->replacements.end(),
                                                  [](const Replacement& r) { return r.offset + r.size <= UNSIGNED_LIMIT; });
                if (over == archive->replacements.end())
                    break;
                for (auto it = over; it != archive->replacements.end(); ++it)
                {
                    Log("[fs] rcf overlay: WARNING: %s %s (%s, %llu bytes) does not fit in the 4 GB the archive format can address; the "
                        "original entry is served", display.c_str(), it->entry.c_str(), it->source.c_str(), (unsigned long long)it->size);
                    it->file.Close();
                }
                archive->replacements.erase(over, archive->replacements.end());
            }
            uint64_t total = 0, overSigned = 0;
            size_t counts[3] = {};
            for (auto it = archive->replacements.begin(); it != archive->replacements.end(); ++it)
            {
                uint8_t* record = &archive->directory[it->index * 12];
                StoreBE32(record + 4, uint32_t(it->offset));
                StoreBE32(record + 8, uint32_t(it->size));
                static const char* where[] = { "in place", "in freed space", "appended" };
                Log("[fs] rcf overlay: %s %s <- %s (%llu bytes, original %u; %s at 0x%llX)", display.c_str(), it->entry.c_str(),
                    it->source.c_str(), (unsigned long long)it->size, it->originalSize, where[int(it->placement)], (unsigned long long)it->offset);
                counts[int(it->placement)]++;
                total += it->size;
                if (it->offset + it->size > SIGNED_LIMIT)
                    overSigned++;
            }
            uint64_t end = archive->baseSize;
            for (const Replacement& r : archive->replacements)
                end = std::max(end, r.offset + r.size);
            archive->size = archive->replacements.empty() ? archive->baseSize : std::max(archive->baseSize, AlignUp(end));

            if (!archive->replacements.empty())
            {
                Log("[fs] rcf overlay: %s virtual size %s (0x%llX; original %s): %zu replaced entries, %s; %zu in place, %zu in freed space "
                    "(%s reused), %zu appended",
                    display.c_str(), Gb(archive->size).c_str(), (unsigned long long)archive->size, Gb(archive->baseSize).c_str(),
                    archive->replacements.size(), Gb(total).c_str(), counts[0], counts[1], Gb(reused).c_str(), counts[2]);
                if (overSigned)
                    Log("[fs] rcf overlay: WARNING: %s does not fit in 2 GB: %zu replaced entries lie past 2 GB. The game's own reader can't "
                        "seek there (signed 32-bit SetFilePointer); the runtime's XenonDrive::Read hook serves them (unsigned, up to 4 GB).",
                        display.c_str(), overSigned);
            }
            return archive;
        }

        std::mutex g_mutex;
        std::map<std::wstring, std::shared_ptr<const Archive>> g_cache;  // by lowercase host path
    }

    void RegisterBuiltinPatch(const std::string& archive, const std::string& entry, const std::string& description, EntryPatcher patcher)
    {
        std::lock_guard lock(g_builtinMutex);
        g_builtins.push_back({ Lower(archive), Lower(entry), description, std::move(patcher), false });
    }

    void RegisterEntryRewrite(const std::string& archive, const std::string& entry, const std::string& description, EntryPatcher rewriter)
    {
        std::lock_guard lock(g_builtinMutex);
        g_builtins.push_back({ Lower(archive), Lower(entry), description, std::move(rewriter), true });
    }

    std::shared_ptr<const Archive> Open(const vfs::VirtualPath& path, const fs::path& host)
    {
        if (path.Writable() || Lower(fs::path(path.relative).extension().string()) != ".rcf")
            return nullptr;

        std::wstring key = host.wstring();
        for (wchar_t& c : key)
            c = wchar_t(towlower(c));
        std::lock_guard lock(g_mutex);
        auto it = g_cache.find(key);
        if (it == g_cache.end())
            it = g_cache.emplace(key, Build(path, host)).first;
        return it->second;
    }

    uint64_t Size(const Archive& archive)
    {
        return archive.size;
    }

    bool Read(const Archive& archive, const plat::File& baseFile, uint64_t offset, uint8_t* data, uint32_t length, uint32_t& transferred)
    {
        transferred = 0;
        if (offset >= archive.size)
            return true;
        length = uint32_t(std::min<uint64_t>(length, archive.size - offset));

        while (transferred < length)
        {
            uint64_t at = offset + transferred;
            uint8_t* out = data + transferred;
            uint32_t remaining = length - transferred;
            uint32_t chunk;

            // First replacement that ends after `at` (they are sorted and disjoint).
            auto next = std::partition_point(archive.replacements.begin(), archive.replacements.end(),
                                             [&](const Replacement& r) { return r.offset + r.size <= at; });
            if (next != archive.replacements.end() && at >= next->offset)
            {
                chunk = uint32_t(std::min<uint64_t>(remaining, next->offset + next->size - at));
                if (!next->file.IsOpen())
                    memcpy(out, next->data.data() + (at - next->offset), chunk);
                else
                {
                    uint32_t read;
                    if (!ReadAt(next->file, at - next->offset, out, chunk, read) || read != chunk)
                        return false;
                }
                if (!next->read->exchange(true))
                    Log("[fs] rcf overlay: the game reads %s (%s)", next->entry.c_str(), next->source.c_str());
            }
            else
            {
                uint64_t limit = next == archive.replacements.end() ? archive.size : next->offset;
                if (at < archive.baseSize)
                {
                    // The original (also between replacements placed inside it).
                    chunk = uint32_t(std::min<uint64_t>(remaining, std::min(limit, archive.baseSize) - at));
                    uint32_t read;
                    if (!ReadAt(baseFile, at, out, chunk, read) || read != chunk)
                        return false;
                    // The patched directory replaces the original one wherever the read overlaps it.
                    uint64_t dirBegin = archive.directoryOffset, dirEnd = dirBegin + archive.directory.size();
                    uint64_t lo = std::max(at, dirBegin), hi = std::min(at + chunk, dirEnd);
                    if (lo < hi)
                        memcpy(out + (lo - at), archive.directory.data() + (lo - dirBegin), size_t(hi - lo));
                }
                else
                {
                    // Alignment padding between appended replacements (or after the last one).
                    chunk = uint32_t(std::min<uint64_t>(remaining, limit - at));
                    memset(out, 0, chunk);
                }
            }
            // Built-in patches (of entries left in place).
            auto patch = std::upper_bound(archive.patches.begin(), archive.patches.end(), at,
                                          [](uint64_t value, const BytePatch& p) { return value < p.offset; });
            if (patch != archive.patches.begin())
                --patch;
            for (; patch != archive.patches.end() && patch->offset < at + chunk; ++patch)
            {
                uint64_t pLo = std::max(at, patch->offset), pHi = std::min(at + chunk, patch->offset + patch->bytes.size());
                if (pLo < pHi)
                    memcpy(out + (pLo - at), patch->bytes.data() + (pLo - patch->offset), size_t(pHi - pLo));
            }
            transferred += chunk;
        }
        return true;
    }
}
