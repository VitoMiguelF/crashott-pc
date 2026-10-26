#pragma once

#include <functional>

// Android only (android_main_android.cpp, docs/internals/android_port.md). SDL3's Java glue calls
// SDL_main on its own native thread, and SDL wants the window and its events there: SDL_main
// keeps that thread for the window (gpu/window_sdl.cpp) and runs the game on another one.
namespace plat::android
{
    // Runs `job` on the SDL_main thread (the window's loop never returns, so it is the last one).
    void RunOnMainThread(std::function<void()> job);
}
