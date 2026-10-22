#pragma once

// The settings screen: opened from the main menu's SETTINGS item (the patched CALIBRATION entry,
// frontend_hooks.cpp), drawn by the presenter over the game (gpu/d3d12_renderer.cpp) and driven by
// controller 0 / the keyboard. While it is open the game reads a neutral controller, and after it
// closes it keeps reading one until every button is released. Changes apply as they are made,
// except the internal resolution and the upscaler, which the renderer applies when the screen
// closes, and the options the game reads at boot (marked with the game's "rotate" icon): leaving
// with one of those changed asks "restart the game now?" (A restarts it by itself, settings.h
// Restart; B leaves them for the next launch). Closing writes userdata\settings.toml.
//
// SETTINGS leads to the PS2 version's options screen, emptied (frontend_patches.cpp): the game
// plays its own transition to it and back, and the settings are drawn over its cloth background.
// It looks like one of the game's menus: the game's fonts, icons and frames (menu_art.h), the
// options split into tabs (AUDIO, VIDEO, GRAPHICS, GAME, SUBTITLES) switched with LB / RB (Q / E),
// in English or, with Portuguese (BR) chosen as the language, in Portuguese. It is drawn on the
// CPU at the window's resolution and redrawn only when something changes.

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <filesystem>

namespace settings::menu
{
    // Starts reading the game's art for the screen (after vfs::Init).
    void Init();

    // Set by the renderer once it can draw the screen; without it RequestOpen is ignored (e.g.
    // CRASH_NO_RENDER), so the game's input is never captured by an invisible screen.
    void SetAvailable(bool available);

    // From the game thread (the hook): opens the screen at the next frame. `fromMainMenu` false
    // (opened during a game) adds "progress that was not saved will be lost" to the restart question.
    void RequestOpen(bool fromMainMenu = true);

    bool IsOpen();

    // The frontend fight tree's current node, from its conditions (sub_820F9BA8, called every frame
    // for the screen shown; frontend_hooks.cpp). Game thread.
    void NoteFrontendNode(uint32_t node);
    // The game must see a neutral controller (screen open, or buttons still held after closing).
    bool BlocksGameInput();

    // Buttons the game must see pressed on top of that: B for a moment after closing, so that it
    // leaves the options screen (where SETTINGS leads) with its own transition. After a restart,
    // START until the main menu (settings::IntroSkipButtons).
    uint16_t GameButtons();

    // Render thread, once per presented frame, over the back buffer (`target`, RENDER_TARGET):
    // opens/closes, reads input, applies changes and draws the screen while it is open.
    void Draw(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE target, uint32_t width, uint32_t height);

    // --settings-preview <dir>: writes every tab in English and Portuguese at a few window sizes
    // to PNG files (over a flat background) without starting the game. Returns the exit code.
    int Preview(const std::filesystem::path& directory);
}
