// HD AI Remaster (graphics.hd_remaster, off by default): the AI-remastered mod, used from where it
// was built: the whole game (<data>\remaster_full, its README) when present, else the level 1 one
// (<data>\remaster_l1); <data> is paths::Data() (data\ next to the exe, private\ in a development build). The mod folder holds hd_textures\ (host-side HD pack,
// gpu/hd_textures.h) and rcf\ (the 256-colour textures and the subdivided models, served by
// fs/rcf_overlay.cpp); with the setting on it is appended to the mod list at startup, so both read
// it like any other mod. Off, nothing of it is opened.

#include "settings.h"
#include "../diagnostics.h"
#include "../fs/vfs.h"
#include "../paths.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace fs = std::filesystem;

namespace settings
{
    namespace
    {
        // The whole game (remaster_full\README.md), then the "MOD FINAL" of
        // remaster_l1\README.md (level 1: textures + models), both under paths::Data().
        constexpr const char* REMASTER_SUBDIRS[] = { "remaster_full/mods/remaster_full", "remaster_l1/mods_final/remaster_l1_final" };

        bool HasData(const fs::path& dir)
        {
            std::error_code error;
            return fs::is_directory(dir / "rcf", error) || fs::is_directory(dir / "hd_textures", error);
        }

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
            return text;
        }

        bool SameFolder(const fs::path& a, const fs::path& b)
        {
            std::error_code error;
            return fs::equivalent(a, b, error) && !error;
        }
    }

    const fs::path& HdRemasterDir()
    {
        static const fs::path dir = [] {
            if (const char* custom = getenv("CRASH_HD_REMASTER_DIR"); custom && *custom)
                return fs::path(custom).make_preferred();
            for (const char* subdir : REMASTER_SUBDIRS)
            {
                fs::path candidate = (paths::Data() / subdir).make_preferred();
                if (HasData(candidate))
                    return candidate;
            }
            return (paths::Data() / REMASTER_SUBDIRS[0]).make_preferred();
        }();
        return dir;
    }

    bool HdRemasterAvailable()
    {
        static const bool available = HasData(HdRemasterDir());
        return available;
    }

    bool HdRemasterWholeGame()
    {
        return HdRemasterAvailable() && HdRemasterDir().filename() != "remaster_l1_final";
    }

    void EnableHdRemaster()
    {
        const fs::path& dir = HdRemasterDir();
        std::string name = dir.filename().string();
        // A copy in the mods folder is loaded whatever the setting says: say so.
        bool listed = false;
        for (const vfs::ModInfo& mod : vfs::Mods())
        {
            bool same = SameFolder(mod.root, dir);
            listed |= same;
            if (!AtStartup().hdRemaster && (same || Lower(mod.name).find("remaster") != std::string::npos))
                Log("[remaster] WARNING: the mod \"%s\" (%s) is in the mods folder and is loaded even with graphics.hd_remaster = false",
                    mod.name.c_str(), mod.root.string().c_str());
        }
        if (!AtStartup().hdRemaster)
            return;
        if (!HdRemasterAvailable())
        {
            Log("[remaster] graphics.hd_remaster = true, but %s has no remaster data: playing without it", dir.string().c_str());
            return;
        }
        if (listed)
        {
            Log("[remaster] HD AI Remaster: %s is already in the mod list", dir.string().c_str());
            return;
        }
        Log("[remaster] HD AI Remaster on: mod %s", dir.string().c_str());
        vfs::AddMod({ name, dir });
    }
}
