#pragma once

// Wide (and 16:10) screens (settings [video] aspect / resolution / hud_16_9; runtime/NOTES.md
// "Ultrawide"). The game draws for 16:9 at 1280x720 and keeps doing so: nothing in guest memory
// changes size. Instead:
//
// - The renderer's host render targets and resolves are Stretch() times wider than 16:9 would
//   make them (their guest sizes stay; every guest x maps to Stretch() times more host pixels).
//   The screen-space passes (bloom, motion blur, DOF, the lighting remaster, DLSS/FSR) work in
//   normalized coordinates, so they follow.
// - The camera sees more (camera/hor_plus.cpp): after pure3d::Camera::SetFOV the camera's
//   effective horizontal FOV, aspect and side planes are widened to Aspect() with the vertical
//   FOV of 16:9 (Hor+); narrower screens (16:10) keep the horizontal FOV and see more above and
//   below. Culling uses the same planes, so the sides are not culled.
// - 2D (draws with W = 1 into the 1280-wide 1x targets: the HUD, the menus, the fades) is put
//   back in a 16:9 area in the middle by the vertex shader epilogue (HudTransform): x scaled around
//   the centre, except vertices at the screen's edges (full-screen backgrounds and fades still
//   cover the screen; the frontend backgrounds zoom to fill it). With hud_16_9 off, elements in
//   the outer thirds keep their distance to the nearest screen edge instead.
// - The Bink videos (pixel shader bd65c6f2d6f2d6d4) stay 16:9 between black bars.
// - Our own overlays (settings screen, subtitles, prompts, console) lay out in the 16:9 area
//   (HudArea).

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

    // The HUD in the 16:9 area (settings video.hud_16_9) or its corners at the screen's edges.
    bool HudCentered();
}
