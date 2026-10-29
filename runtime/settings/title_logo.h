#pragma once

// "Definitive Edition" under the Crash of the Titans logo (runtime/NOTES.md, "Logo Definitive
// Edition"): built-in rewrites of default.rcf entries, applied as the game opens the archive.
// - The frontend text bibles get a new key, frontend / FE_Definitive_Edition_Text, with the same
//   text in every language (a product subtitle).
// - The title page (PRESS START) gets a text item with that key under its logo. The main menu,
//   which has no logo, is left as it was.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace settings
{
    // Called by RegisterFrontendPatches, before the language pack's rewrites (which keep the new
    // key). CRASH_TITLE_LOGO=off skips it.
    void RegisterTitleLogoPatches();

    // The same insertion for another key of the "frontend" bible (pc_frontend.cpp: MULTIPLAYER):
    // `textOf` gives each language's text by its letter. For an entry rewrite of the packages
    // FrontendBiblePackages lists.
    bool AddFrontendTextKey(std::vector<uint8_t>& data, const char* key, const char16_t* (*textOf)(char letter));
    const char* const* FrontendBiblePackages(size_t& count);

    // Reading texts of the "frontend" bible by their key names (e.g. Episode_L1_E1_Name, the HUB's episode
    // titles) in the startup language's letter, as the game shows them (after a language pack's rewrite: call
    // after RegisterLanguagePack). FrontendText: UTF-8, empty until the game has read the bible (the frontend
    // loads it at startup) or for a key it lacks. Any thread.
    void CaptureFrontendTexts(const std::vector<std::string>& keys);
    std::string FrontendText(const std::string& key);
}
