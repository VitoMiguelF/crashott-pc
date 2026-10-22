#pragma once

// Draws the current subtitle (subtitles.h) over the game image; called by the presenter
// (gpu/d3d12_renderer.cpp, Swap) after the game image and before the settings screen, the
// screenshots and the performance overlay, so screenshots include it.

#include <windows.h>
#include <d3d12.h>

#include <cstdint>

namespace subtitles
{
    // `target` is the back buffer (RENDER_TARGET state). Does nothing with subtitles off.
    void Draw(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE target, uint32_t width, uint32_t height);
}
