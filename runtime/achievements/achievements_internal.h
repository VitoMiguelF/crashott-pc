#pragma once

// Shared by achievements.cpp (data, unlocking, input) and achievements_draw.cpp (render thread).

#include "achievements.h"
#include "../settings/menu_art.h"
#include "../settings/ui_text.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace achievements::detail
{
    struct Achievement
    {
        uint16_t id = 0;
        uint16_t gamerscore = 0;
        bool secret = false;           // the SPA hides it until unlocked (no "show unachieved" flag)
        std::string name;              // the label
        std::string description;       // shown once unlocked
        std::string howTo;             // shown while locked ("Complete Temple Episode")
        settings::menu_art::Image icon, iconLocked;  // premultiplied; iconLocked is grey and darker
    };

    struct Data
    {
        bool ok = false;
        std::vector<Achievement> list;  // the SPA's order
        int totalScore = 0;
    };
    // Read from the loaded image once (after LoadXex); `ok` false without an SPA.
    const Data& GetData();
    // --achievements-preview: GetData in `language` (the SPA's texts), until called with nullptr.
    void SetPreviewLanguage(const settings::UiLanguage* language);

    // Unlocked id -> "YYYY-MM-DD HH:MM:SS" (local time); `version` changes with each unlock.
    std::map<uint16_t, std::string> Unlocked(uint64_t* version = nullptr);

    // The next achievement to announce (the popup takes them one at a time); false if none.
    bool TakePopup(uint16_t& id);

    // The screen, as the game thread leaves it for the drawing.
    struct Visual
    {
        bool open = false;
        int selected = 0;
        bool operator==(const Visual& o) const { return open == o.open && selected == o.selected; }
    };
    Visual CurrentVisual();
    // Where the hint is drawn (main menu, pause), and whether to draw it now.
    bool HintWanted();

    // Where the mouse can point, in the 1280 x 720 image (made when drawn).
    enum class Zone { Cell, Back, Hint };
    struct HitZone
    {
        float x0, y0, x1, y1;
        Zone zone;
        int index;
    };
    void SetZones(Zone kind, const std::vector<HitZone>& zones);  // replaces those of `kind`'s group

    constexpr int COLUMNS = 8;
}
