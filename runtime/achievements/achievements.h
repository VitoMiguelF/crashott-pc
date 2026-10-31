#pragma once

// The game's own Xbox 360 achievements (achievements.cpp, achievements_draw.cpp; README
// "Conquistas", docs/internals/achievements.md).
//
// Data: the title's XDBF / SPA resource ("565507E4" in default.xex's resource header 0x000002FF),
// read from the loaded image at run time: names, descriptions, gamerscore and the 64 x 64 PNG icons,
// in the game's language when the SPA has it (English, French, German, Spanish, Italian), else in
// English. Nothing of it is copied anywhere.
//
// Unlocking: the game awards an achievement with XUserWriteAchievements (sub_822649A0 in the title,
// from CObjectiveManager::vf10), which is XMsgStartIORequest(app 0xFB, message 0x000B0008,
// { count, XUSER_ACHIEVEMENT* }) to XAM: kernel/xmsg_imports.cpp hands it to WriteMessage. The game
// never reads them back (no XamUserCreateAchievementEnumerator / XUserReadAchievements import).
// Unlocked achievements are universal, kept in <userdata>/achievements.toml with the date and time,
// not in the saves: once earned they stay, in every slot and in NEW GAME.
//
// Shown: a popup in the Xbox 360 style (icon, "Achievement unlocked", gamerscore and name, at the
// bottom of the screen for a few seconds, with the menus' select sound); the achievements screen
// (every icon in a grid, the unlocked ones in colour, the selected one's name, description and date,
// X of Y and the gamerscore) in the game's message box, opened with BACK on the main menu, the HUB
// and the pause (the keyboard's BACK key, Tab by default, on the main menu and the pause; a click or
// a tap on the hint the main menu and the pause show). The HUB panel of an online session keeps
// its BACK (the lobby).

#include "../kernel/xinput_types.h"

#include <cstdint>
#include <filesystem>
#include <string>

struct PPCContext;

namespace achievements
{
    // XMsg app 0xFB (XGI), message 0x000B0008 (XUserWriteAchievements): unlocks what `buffer` lists.
    // Game thread.
    void WriteMessage(uint32_t buffer);

    // Unlocks achievement `id` (the SPA's id) as the game would; false if unknown or already
    // unlocked. Any thread.
    bool Unlock(uint32_t id, const char* how);
    // The developer console's "achievement" (dev_console.cpp), game thread: the game's own award
    // path (CObjectiveManager's queue, sub_8220A708), as if the player had earned it.
    bool AwardThroughGame(PPCContext& ctx, uint8_t* base, uint32_t id);
    // "id name (unlocked date)" lines, for the console.
    std::string List();

    // Controller 0 as the game is about to see it (kernel/xam_input_imports.cpp, after the cheat
    // menu): opens the screen with BACK, drives it while open and hides the pad from the game.
    void FilterGameInput(XINPUT_GAMEPAD& pad, bool poll);
    // The game must see nothing pressed (other players' pads, the menus' mouse).
    bool BlocksGameInput();
    bool IsOpen();

    // Game thread (the script update and the frontend's conditions): the sounds.
    void RunPending(PPCContext& ctx, uint8_t* base);

    // Render thread, once per presented frame: the hint, the screen and the popup.
    void Draw(uint32_t width, uint32_t height);

    // --achievements-preview <dir>: the screen and the popup in every language, as PNG files, from
    // the dump's SPA, without starting the game. Returns the exit code.
    int Preview(const std::filesystem::path& directory);
}
