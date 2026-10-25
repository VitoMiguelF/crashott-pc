#pragma once
// Where the runtime finds everything, resolved once at startup (paths::Init) relative to the exe,
// so the game runs from any folder without paths baked in at build time:
//
//   <exe folder>\CrashRecomp.exe   + the FFmpeg / DLSS / FidelityFX DLLs next to it
//   <exe folder>\game\             the game dump (default.xex, *.rcf), optional: see Game()
//   <exe folder>\mods\             mods (fs/vfs.h)
//   <exe folder>\userdata\         saves, settings.toml, shader cache
//   <exe folder>\languages\        language packs (settings/language_pack.h)
//   <exe folder>\data\             optional data derived from the user's game: subtitles\english.txt,
//                                  symbols\functions.csv, remaster_full\ / remaster_l1\ (HD AI Remaster)
//
// Installed by the single-file installer (tools\make_dist.py --installed): the exe is
// CrashOTTDE.exe and only game\, mods\ and userdata\ are next to it; languages\ and data\ are in the
// CrashRecomp\ subfolder (with the project the game was built from).
//
// Development build (build\CrashRecomp.exe, no languages\ next to it and the project's
// CMakeLists.txt one level up): languages\ of the project, data = private\, and mods\ / userdata\
// of dist\ (the player's folder made by tools\make_dist.py) or, without dist\, of the project.
//
// Each one can be given on the command line: --game --mods --userdata --languages --data, plus
// --xex <default.xex> (default: <game>\default.xex) and --image <image.bin> (default: none, the
// image is decrypted from the XEX at startup).

#include <filesystem>

namespace paths
{
    // Parses the path arguments and resolves the defaults. Call first thing in main. The game
    // folder is: --game, else <exe>\game if it has default.xex, else [paths] game of
    // <userdata>\settings.toml, else a folder picker (main saves the choice there, see
    // GameChosenNow). Returns false if there is no usable game folder.
    // With `askForGame` false (the launcher) there is no picker and no error box: Game() stays
    // empty when no folder is found, and Init still returns true.
    bool Init(int argc, char** argv, bool askForGame = true);

    const std::filesystem::path& ExeDir();
    const std::filesystem::path& Game();
    const std::filesystem::path& Mods();
    const std::filesystem::path& UserData();
    const std::filesystem::path& Languages();
    const std::filesystem::path& Data();
    const std::filesystem::path& Xex();
    const std::filesystem::path& Image();  // empty: decrypt the XEX (kernel/xex_image.h)

    // True when the game folder came from the folder picker: settings.toml should remember it.
    bool GameChosenNow();
    // Whether `folder` holds a game dump (default.xex and default.rcf).
    bool IsGameFolder(const std::filesystem::path& folder);
    // The folder picker ("choose the game folder"), until a game folder is chosen; empty if cancelled.
    std::filesystem::path PickGameFolder(void* owner = nullptr);
}
