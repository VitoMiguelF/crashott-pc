#pragma once

#include <cstdint>

// The frontend as a PC game (runtime/NOTES.md, "Frontend de PC"): an EXIT GAME item on the main
// menu, no storage device button (Y) on any screen, and the texts that mention the Xbox 360, its
// storage devices or its Guide button reworded for the PC in every language. Built-in patches of
// default.rcf entries (rcf_overlay), applied as the game opens the archive; game files untouched.
// The menu's conditions are native code: frontend_hooks.cpp uses QuitExit and IsStorageDeviceExit.
namespace settings
{
    // Called by RegisterFrontendPatches (after its own patches of the same entries).
    void RegisterPcFrontendPatches();

    // Menu index of EXIT GAME on the main menu (StartMenu of GameStart_Start_Xenon), as the
    // game's conditions see it (frontend_hooks.cpp maps the real ones, with MULTIPLAYER, back).
    constexpr int QUIT_MENU_INDEX = 4;
    // MULTIPLAYER's real index (between LOAD GAME and CREDITS), when MultiplayerItemAdded.
    constexpr int MULTIPLAYER_MENU_INDEX = 2;
    bool MultiplayerItemAdded();

    // The Frontend fight tree exit that EXIT GAME takes, or -1 when the patches did not apply.
    int QuitExit();

    // Whether `exit` is one that opens the storage device selector (Y on the main menu and on the
    // save slot screens).
    bool IsStorageDeviceExit(int exit);
}
