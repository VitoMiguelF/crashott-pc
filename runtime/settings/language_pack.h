#pragma once

// Language packs: translations of the game's texts that show up as extra languages in the
// settings screen (README, "Tradução PT-BR"; runtime/NOTES.md). A pack is a folder
//
//   languages\<id>\language.toml       id, name, base language, archive and text packages
//   languages\<id>\texts\<bible>.txt   "0xHASH = text" lines, as tools/texts.py exports them
//
// The game runs in the base language (its XConfig language, text letter and voice archive, e.g.
// English with english.rcf), and the text bibles of the base letter in the listed packages are
// rebuilt with the pack's texts when the game opens the archive (rcf_overlay::RegisterEntryRewrite).
// The rewrite works on the entry as the game would otherwise see it, so mods that replace those
// packages keep their other changes; for the keys a pack translates, the pack wins. Keys missing
// from the pack keep the base language's text.
//
// A "[package\x.p3d package\y.p3d]" line in a text file starts lines that apply only to those
// packages (the same key can hold a different text in another copy of a bible); "[]" goes back to
// lines for every package.

#include <filesystem>

namespace settings
{
    // Adds the packs found in `dir` to Languages(). Call once, before Load().
    void LoadLanguagePacks(const std::filesystem::path& dir);

    // When the startup language is a pack, reads its texts and registers the rewrites of its
    // packages. Call after Load() and RegisterFrontendPatches(), before the game opens the archive.
    void RegisterLanguagePack();
}
