#pragma once

// Draws the current subtitle (subtitles.h) over the game image; called by the presenter
// (gpu/xenos_renderer.cpp, Swap) after the game image and before the settings screen, the
// screenshots and the performance overlay, so screenshots include it.

#include <windows.h>

#include <cstdint>

namespace subtitles
{
    // Draws into the back buffer of this present (gpu::overlay::BeginPresent). Does nothing with subtitles off.
    void Draw(uint32_t width, uint32_t height);
}
