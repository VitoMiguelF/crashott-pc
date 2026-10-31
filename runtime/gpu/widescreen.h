#pragma once

// Wide (and 16:10) screens (settings [video] aspect / resolution; runtime/NOTES.md
// "Ultrawide"; docs/internals/graphics.md). The game draws for 16:9 at 1280x720 and keeps doing
// so: nothing in guest memory changes size. Instead:
//
// - The renderer's host render targets and resolves are Stretch() times wider than 16:9 would
//   make them (their guest sizes stay; every guest x maps to Stretch() times more host pixels).
//   The screen-space passes (bloom, motion blur, DOF, the lighting remaster, DLSS/FSR) work in
//   normalized coordinates, so they follow.
// - The camera sees more (camera/hor_plus.cpp): pddi's SetProjection widens 16:9 perspective
//   projections to Aspect() with the vertical FOV of 16:9 (Hor+; 16:10 keeps the horizontal FOV
//   and sees more above and below), the culling's sphere test uses side planes widened the same
//   way, and the orthographic matrix (the HUD) keeps the game's own aspect.
// - 2D into the screen's 1x targets (1280 wide) is put back into the middle 16:9 area, per
//   triangle (HUD_GS in xenos_renderer.cpp; xe_hud_transform for points and lines; never D3D's
//   rectangle-list clears): scaled around the centre, except triangles spanning the screen's
//   width (fades, full-screen passes, bars), which keep its edges; backgrounds zoom to fill it. The frontend's sprites, drawn
//   through a pure3d camera (W = 0.5), are already placed by the projection; only their big
//   background sprites zoom. The HUD always stays in that area (the old hud_16_9 = false, HUD
//   corners at the screen's edges, is gone; the shaders' anchor bit 8 is never set).
// - The Bink videos (while cutscenes::VideoPlaying) stay 16:9 between black bars.
// - Our own overlays (settings screen, subtitles, prompts, console) lay out by the height, centred.

#include <cstdint>

namespace gpu::widescreen
{
    constexpr float BASE_ASPECT = 16.0f / 9.0f;

    // Width / height of the picture the renderer draws now (16:9 until the renderer sets it).
    float Aspect();
    void SetAspect(float aspect);  // renderer: Init, Reconfigure
    // Host width per guest width relative to 16:9 (Aspect() / BASE_ASPECT).
    inline float Stretch() { return Aspect() / BASE_ASPECT; }

    // The 16:9 area of a `width` x `height` picture (all of it at 16:9): x, y, width, height.
    void HudArea(uint32_t width, uint32_t height, float& x, float& y, float& areaWidth, float& areaHeight);
}
