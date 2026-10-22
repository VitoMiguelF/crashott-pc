#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Virtual file system behind the Nt*File imports.
//
// Guest paths name a device ("D:\shaders\x.out", "game:\...", "\Device\Cdrom0\...") and map to a
// mount:
//   game      D:, game:, dvd:, \Device\Cdrom0     the game folder, read-only, with the mod overlay
//   hdd       E:, hdd:, \Device\Harddisk0\Partition1   userdata\hdd, writable
//   cache     cache:, Z:, \Device\Harddisk0\Cache0     userdata\cache, writable
//   content   "<root>:" while a content package is open (XamContentCreateEx), writable, on its
//             folder under userdata\content (see kernel/xam_content_imports.cpp)
// Paths are case-insensitive (as on the host) and normalized ('/' and '\', "." and "..").
//
// Mod overlay (game mount only): every subfolder of the mods folder is a mod laid out like the
// game folder. A file is taken from the first mod in load order that has it, otherwise from the
// game folder; directory listings are the union. Load order: the names listed in
// mods\load_order.txt (one per line, '#' comments), then the remaining folders alphabetically.
// Folders whose name starts with '_' are disabled.
//
// The game folder is never written: the runtime opens it read-only and refuses creation, writes
// and deletion there.

namespace vfs
{
    struct Config
    {
        std::filesystem::path gameRoot;
        std::filesystem::path modsDir;
        std::filesystem::path userDataDir;
    };

    void Init(const Config& config);

    // The writable user data folder (--userdata), also used for runtime caches.
    const std::filesystem::path& UserDataDir();

    struct ModInfo
    {
        std::string name;
        std::filesystem::path root;
    };

    // Enabled mods in load order (first one wins).
    const std::vector<ModInfo>& Mods();
    // Appends a mod that lives outside the mods folder (last in load order). Only during startup,
    // before anything reads Mods() (settings::EnableHdRemaster).
    void AddMod(const ModInfo& mod);

    struct Mount;

    struct VirtualPath
    {
        const Mount* mount{};
        std::string relative;  // normalized, '\'-separated, no leading/trailing separator; "" = mount root

        // "D:\shaders\x.out"-style name for logs.
        std::string Display() const;
        bool Writable() const;
    };

    // Parses an absolute guest path, or a path relative to `directory` when given.
    std::optional<VirtualPath> Parse(std::string_view guestPath, const VirtualPath* directory = nullptr);

    struct Location
    {
        std::filesystem::path host;
        std::string source;  // "game", "mod <name>" or "userdata"
    };

    // Where an existing file or directory lives (first mod that has it, then the game folder),
    // or nullopt.
    std::optional<Location> Locate(const VirtualPath& path);

    // Host path for creating/writing (writable mounts only).
    std::filesystem::path WritableHostPath(const VirtualPath& path);

    struct DirectoryEntry
    {
        std::string name;
        std::filesystem::path host;
    };

    // Union of the directory's entries over the mods and the game folder (first one wins).
    std::vector<DirectoryEntry> List(const VirtualPath& directory);

    // Content packages: makes "<rootName>:\..." resolve to `host` (writable) until unmounted.
    // Remounting a root name replaces its target. Returns false for an invalid name.
    bool MountContent(std::string_view rootName, const std::filesystem::path& host);
    void UnmountContent(std::string_view rootName);

    // Case-insensitive wildcard match ('*' and '?'), as used by NtQueryDirectoryFile.
    bool MatchWildcard(std::string_view pattern, std::string_view name);
}
