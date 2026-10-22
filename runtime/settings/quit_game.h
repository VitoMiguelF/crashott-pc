#pragma once

// EXIT GAME, the main menu's fifth item (added by settings/pc_frontend.cpp; frontend_hooks.cpp
// sees it chosen): closes the game like a PC game, with exit code 0.
//
// RequestQuit (the game thread that ran the menu's exit) saves settings.toml and stops an input
// recording in progress at once. The render thread finishes the job at the end of its next
// present, where it owns the device: GPU idle, out of exclusive fullscreen, upscalers released,
// then FinishQuit (audio output stopped, ExitProcess(0)). Without a renderer, or if no present
// comes within 3 s, FinishQuit runs from here / a helper thread instead.
namespace settings
{
    void RequestQuit();
    bool QuitRequested();
    [[noreturn]] void FinishQuit();
}
