#include "ui_text.h"
#include "settings.h"

namespace settings
{
    UiLanguage UiLanguageOf(const std::string& languageId)
    {
        if (languageId.rfind("pt", 0) == 0)
            return UiLanguage::Portuguese;
        // The game's languages and the packs' base ones, by the text bible letter.
        switch (LanguageById(languageId).letter)
        {
        case 'F': return UiLanguage::French;
        case 'G': return UiLanguage::German;
        case 'S': return UiLanguage::Spanish;
        case 'I': return UiLanguage::Italian;
        case 'D': return UiLanguage::Dutch;
        default: return UiLanguage::English;
        }
    }

    UiLanguage CurrentUiLanguage()
    {
        return UiLanguageOf(Get().language);
    }

    const char* Tr(const UiText& text, UiLanguage language)
    {
        const char* pick = nullptr;
        switch (language)
        {
        case UiLanguage::English: pick = text.en; break;
        case UiLanguage::Portuguese: pick = text.pt; break;
        case UiLanguage::French: pick = text.fr; break;
        case UiLanguage::German: pick = text.de; break;
        case UiLanguage::Spanish: pick = text.es; break;
        case UiLanguage::Italian: pick = text.it; break;
        case UiLanguage::Dutch: pick = text.nl; break;
        }
        return pick ? pick : text.en;
    }
}
