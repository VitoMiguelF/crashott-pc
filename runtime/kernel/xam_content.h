#pragma once

// The saved games (content type 1) of the local profile, from the runtime's side
// (xam_content_imports.cpp): the save screens (settings/save_screens.cpp) look at the slots and
// delete one. The packages are folders in userdata\content (runtime/NOTES.md, "Onde ficam os saves").

#include <string>

namespace xam::content
{
    // Whether the saved game `fileName` (the XCONTENT_DATA file name, e.g. "CrashOTT GameSlot 1")
    // exists; `displayName` gets its display name (from its .header), when asked.
    bool SavedGameExists(const std::string& fileName, std::string* displayName = nullptr);

    // Deletes the saved game `fileName` the way XamContentDelete does (its folder, .header and
    // .thumbnail). For when the game's own delete cannot be called; true if it is gone.
    bool DeleteSavedGame(const std::string& fileName);
}
