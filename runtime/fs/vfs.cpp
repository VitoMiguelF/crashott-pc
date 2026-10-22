#include "vfs.h"
#include "../diagnostics.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <fstream>
#include <mutex>
#include <unordered_set>

namespace fs = std::filesystem;

namespace vfs
{
    struct Mount
    {
        const char* display;  // device name used in logs
        fs::path root;
        bool writable;
        bool overlay;         // mods apply
    };
}

namespace
{
    vfs::Mount g_game{ "D:", {}, false, true };
    vfs::Mount g_hdd{ "hdd:", {}, true, false };
    vfs::Mount g_cache{ "cache:", {}, true, false };

    struct Prefix
    {
        const char* text;  // lowercase, without the trailing separator
        vfs::Mount* mount;
    };

    // Longest prefixes first. "\??\" (the DOS devices directory, RootDirectory 0xFFFFFFFD) is
    // stripped before matching.
    const Prefix PREFIXES[] = {
        { "\\device\\harddisk0\\partition1", &g_hdd },
        { "\\device\\harddisk0\\cache0", &g_cache },
        { "\\device\\cdrom0", &g_game },
        { "game:", &g_game },
        { "dvd:", &g_game },
        { "d:", &g_game },
        { "hdd:", &g_hdd },
        { "e:", &g_hdd },
        { "cache:", &g_cache },
        { "z:", &g_cache },
    };

    // Content package mounts. Entries are never freed (open files keep a Mount pointer); an
    // unmounted root just stops resolving.
    struct ContentMount
    {
        std::string prefix;   // lowercase root name with the colon, e.g. "save:"
        std::string display;  // as the title wrote it, with the colon
        vfs::Mount mount;
        bool active;
    };

    std::mutex g_contentMutex;
    std::deque<ContentMount> g_contentMounts;

    using Mod = vfs::ModInfo;

    std::vector<Mod> g_mods;

    std::string Lower(std::string_view text)
    {
        std::string result(text);
        for (char& c : result)
            c = char(std::tolower(uint8_t(c)));
        return result;
    }

    // Splits on '\' and '/', resolving "." and ".."; appends to `components`.
    bool AppendComponents(std::vector<std::string>& components, std::string_view path)
    {
        size_t start = 0;
        while (start <= path.size())
        {
            size_t end = path.find_first_of("\\/", start);
            if (end == std::string_view::npos)
                end = path.size();

            std::string_view part = path.substr(start, end - start);
            if (part == "..")
            {
                if (components.empty())
                    return false;
                components.pop_back();
            }
            else if (!part.empty() && part != ".")
            {
                components.emplace_back(part);
            }
            start = end + 1;
        }
        return true;
    }

    std::string Join(const std::vector<std::string>& components)
    {
        std::string result;
        for (auto& component : components)
        {
            if (!result.empty())
                result += '\\';
            result += component;
        }
        return result;
    }

    fs::path HostPath(const fs::path& root, const std::string& relative)
    {
        if (relative.empty())
            return root;
        std::string native = relative;
        return root / fs::path(native).make_preferred();
    }

    bool Exists(const fs::path& path)
    {
        std::error_code error;
        return fs::exists(path, error);
    }

    void LoadMods(const fs::path& modsDir)
    {
        g_mods.clear();
        std::error_code error;
        if (modsDir.empty() || !fs::is_directory(modsDir, error))
            return;

        std::vector<std::string> folders;
        for (auto& entry : fs::directory_iterator(modsDir, error))
        {
            std::string name = entry.path().filename().string();
            if (entry.is_directory(error) && !name.empty() && name[0] != '_' && name[0] != '.')
                folders.push_back(name);
        }
        std::sort(folders.begin(), folders.end(), [](auto& a, auto& b) { return Lower(a) < Lower(b); });

        std::vector<std::string> order;
        std::ifstream list(modsDir / "load_order.txt");
        for (std::string line; std::getline(list, line);)
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            size_t first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line[first] == '#')
                continue;
            line = line.substr(first);

            auto it = std::find_if(folders.begin(), folders.end(), [&](auto& folder) { return Lower(folder) == Lower(line); });
            if (it == folders.end())
            {
                Log("[fs] load_order.txt names \"%s\", which is not an enabled mod folder", line.c_str());
                continue;
            }
            order.push_back(*it);
            folders.erase(it);
        }
        order.insert(order.end(), folders.begin(), folders.end());

        for (auto& name : order)
            g_mods.push_back({ name, modsDir / name });
    }
}

namespace vfs
{
    namespace
    {
        fs::path g_userDataDir;
    }

    const fs::path& UserDataDir()
    {
        return g_userDataDir;
    }

    const std::vector<ModInfo>& Mods()
    {
        return g_mods;
    }

    void AddMod(const ModInfo& mod)
    {
        g_mods.push_back(mod);
        Log("[fs]   %zu. %s (%s)", g_mods.size(), mod.name.c_str(), mod.root.string().c_str());
    }

    void Init(const Config& config)
    {
        g_userDataDir = config.userDataDir;
        g_game.root = config.gameRoot;
        g_hdd.root = config.userDataDir / "hdd";
        g_cache.root = config.userDataDir / "cache";
        LoadMods(config.modsDir);

        std::error_code error;
        Log("[fs] game folder: %s%s", g_game.root.string().c_str(), fs::is_directory(g_game.root, error) ? "" : " (MISSING)");
        Log("[fs] mods folder: %s, %zu mod(s)", config.modsDir.string().c_str(), g_mods.size());
        for (size_t i = 0; i < g_mods.size(); i++)
            Log("[fs]   %zu. %s", i + 1, g_mods[i].name.c_str());
        Log("[fs] user data: %s", config.userDataDir.string().c_str());
    }

    std::string VirtualPath::Display() const
    {
        return std::string(mount ? mount->display : "?:") + "\\" + relative;
    }

    bool VirtualPath::Writable() const
    {
        return mount && mount->writable;
    }

    std::optional<VirtualPath> Parse(std::string_view guestPath, const VirtualPath* directory)
    {
        std::vector<std::string> components;
        VirtualPath result;

        std::string lower = Lower(guestPath);
        std::string_view rest = guestPath;
        if (lower.starts_with("\\??\\"))
        {
            rest.remove_prefix(4);
            lower.erase(0, 4);
        }

        for (auto& prefix : PREFIXES)
        {
            std::string_view text = prefix.text;
            if (lower.starts_with(text) && (lower.size() == text.size() || lower[text.size()] == '\\' || lower[text.size()] == '/'))
            {
                result.mount = prefix.mount;
                rest.remove_prefix(text.size());
                break;
            }
        }

        if (!result.mount)
        {
            std::lock_guard lock(g_contentMutex);
            for (auto& content : g_contentMounts)
            {
                std::string_view text = content.prefix;
                if (content.active && lower.starts_with(text) &&
                    (lower.size() == text.size() || lower[text.size()] == '\\' || lower[text.size()] == '/'))
                {
                    result.mount = &content.mount;
                    rest.remove_prefix(text.size());
                    break;
                }
            }
        }

        if (!result.mount)
        {
            // Relative to an open directory.
            if (!directory || !directory->mount || (!rest.empty() && (rest[0] == '\\' || rest[0] == '/')))
                return std::nullopt;
            result.mount = directory->mount;
            AppendComponents(components, directory->relative);
        }

        if (!AppendComponents(components, rest))
            return std::nullopt;

        result.relative = Join(components);
        return result;
    }

    std::optional<Location> Locate(const VirtualPath& path)
    {
        if (!path.mount)
            return std::nullopt;

        if (path.mount->overlay)
        {
            for (auto& mod : g_mods)
            {
                fs::path host = HostPath(mod.root, path.relative);
                if (Exists(host))
                    return Location{ host, "mod " + mod.name };
            }
        }

        fs::path host = HostPath(path.mount->root, path.relative);
        if (Exists(host))
            return Location{ host, path.mount->writable ? "userdata" : "game" };

        // The writable mounts' roots exist as soon as something is written there.
        if (path.mount->writable && path.relative.empty())
            return Location{ host, "userdata" };

        return std::nullopt;
    }

    fs::path WritableHostPath(const VirtualPath& path)
    {
        return HostPath(path.mount->root, path.relative);
    }

    std::vector<DirectoryEntry> List(const VirtualPath& directory)
    {
        std::vector<DirectoryEntry> entries;
        std::unordered_set<std::string> seen;
        std::error_code error;

        auto addFrom = [&](const fs::path& root)
        {
            fs::path host = HostPath(root, directory.relative);
            if (!fs::is_directory(host, error))
                return;
            for (auto& entry : fs::directory_iterator(host, error))
            {
                std::string name = entry.path().filename().string();
                if (seen.insert(Lower(name)).second)
                    entries.push_back({ name, entry.path() });
            }
        };

        if (directory.mount->overlay)
        {
            for (auto& mod : g_mods)
                addFrom(mod.root);
        }
        addFrom(directory.mount->root);
        return entries;
    }

    bool MountContent(std::string_view rootName, const fs::path& host)
    {
        if (rootName.empty() || rootName.size() > 32 || rootName.find_first_of(":\\/") != std::string_view::npos)
            return false;
        std::string prefix = Lower(rootName) + ":";

        std::lock_guard lock(g_contentMutex);
        ContentMount* target = nullptr;
        for (auto& content : g_contentMounts)
        {
            if (content.prefix == prefix)
                target = &content;
        }
        if (!target)
        {
            target = &g_contentMounts.emplace_back();
            target->prefix = prefix;
            target->display = std::string(rootName) + ":";
            target->mount.display = target->display.c_str();
        }
        target->mount.root = host;
        target->mount.writable = true;
        target->mount.overlay = false;
        target->active = true;
        return true;
    }

    void UnmountContent(std::string_view rootName)
    {
        std::string prefix = Lower(rootName) + ":";
        std::lock_guard lock(g_contentMutex);
        for (auto& content : g_contentMounts)
        {
            if (content.prefix == prefix)
                content.active = false;
        }
    }

    bool MatchWildcard(std::string_view pattern, std::string_view name)
    {
        if (pattern.empty() || pattern == "*" || pattern == "*.*")
            return true;

        // Iterative '*' backtracking match.
        size_t p = 0, n = 0, star = std::string_view::npos, mark = 0;
        while (n < name.size())
        {
            if (p < pattern.size() && (pattern[p] == '?' || std::tolower(uint8_t(pattern[p])) == std::tolower(uint8_t(name[n]))))
            {
                p++;
                n++;
            }
            else if (p < pattern.size() && pattern[p] == '*')
            {
                star = p++;
                mark = n;
            }
            else if (star != std::string_view::npos)
            {
                p = star + 1;
                n = ++mark;
            }
            else
            {
                return false;
            }
        }
        while (p < pattern.size() && (pattern[p] == '*' || pattern[p] == '.' && p + 1 == pattern.size()))
            p++;
        return p == pattern.size();
    }
}
