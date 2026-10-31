#pragma once

// Texts of the runtime's own screens outside the SETTINGS screen (whose names and help are the
// English / PT-BR pairs of options.h): the performance overlay, the developer console and the
// notices over the game. They follow game.language like the game's texts: one text for each of the
// game's six languages (settings.cpp) and PT-BR; another language pack of languages\ shows its
// base language's (language_pack.h), and a pack whose id starts with "pt" the PT-BR one.

#include <cstdint>
#include <string>

namespace settings
{
    enum class UiLanguage : uint8_t { English, Portuguese, French, German, Spanish, Italian, Dutch };

    // The texts' language for a game.language value ("english", "pt-br", ...).
    UiLanguage UiLanguageOf(const std::string& languageId);
    // game.language now (the settings screen changes it before the restart that applies it).
    UiLanguage CurrentUiLanguage();

    struct UiText
    {
        const char* en;
        const char* pt;
        const char* fr;
        const char* de;
        const char* es;
        const char* it;
        const char* nl;
    };
    // The text in `language` (UTF-8; the overlay's font only has upper case ASCII).
    const char* Tr(const UiText& text, UiLanguage language);
    inline const char* Tr(const UiText& text) { return Tr(text, CurrentUiLanguage()); }
}
