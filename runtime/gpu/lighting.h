#pragma once

// Lighting remaster (settings [lighting], all off by default): screen-space ambient occlusion,
// one-bounce indirect light and contact shadows, applied to the finished 3D scene before the game
// draws its HUD and post-processing over it. Nothing of the game's data or drawing changes: the
// renderer calls Apply where the temporal upscaler runs (xenos_renderer.cpp, "Temporal
// upscaling"): before the first 2D draw that reads this frame's scene color resolve, at the
// scene's resolution, before DLSS/FSR.
//
// Inputs: the scene color and depth resolves (depth inverted, z = -ndc + 1), the camera
// (view-projection recovered from the scene vertex shaders' constants) and the motion vectors of
// the temporal pass.
//
// Contact shadows point straight up (world +y): the game's shadow volumes are extruded on the CPU
// from a light that no shader constant gives, and the directional lights of its lit shaders
// (LightDirection0..3, c9-c16) are off for most draws and do not match those shadows, so a
// shadow towards them would contradict the game's own. Vertical rays give the grounding shadow
// under characters, props and overhangs, which never disagrees with them.
//
// Passes (rhi.h pass pipelines and targets of its own, like smaa.*):
//   prep       view depth + normal reconstructed from depth, at the AO resolution (half or full)
//   gather     visibility bitmask (Therrien et al. 2023, "screen space indirect lighting with
//              visibility bitmask"): per slice, the sectors of the hemisphere hidden by the depth
//              buffer -> ambient occlusion, and the colour of the samples that hide them -> indirect
//              light; contact shadows: a short ray march upwards in the depth buffer
//   temporal   accumulation with the motion vectors, rejected where the depth disagrees
//   blur       depth / normal aware, horizontal + vertical
//   composite  scene colour * AO * contact shadow + scene colour * indirect, bilateral upsampling
//              from half resolution; the sky and far depth are left alone
//
// The game's own shadows are stencil volumes with a 9-tap filter (graphics_options.h); the "soft
// shadows" option replaces that filter by a wider rotated disk (xe_jitter.z, graphics_options.cpp).

#include "rhi.h"

#include <array>
#include <cstdint>

namespace gpu::lighting
{
    bool Init(rhi::Device* device);

    // Which passes the current settings ask for (read every frame, so they apply live).
    struct Options
    {
        bool ao = false, gi = false, contactShadows = false;
        bool fullResolution = false;
        float aoStrength = 0.0f, giIntensity = 0.0f;
        uint32_t slices = 2, steps = 6;
        bool Any() const { return ao || gi || contactShadows; }
    };
    // The options of the settings, or none while F8 has the remaster switched off (not saved).
    Options CurrentOptions();
    bool SoftShadows();
    // F8: switches the whole remaster (soft shadows included) off and back on, for comparing.
    // Returns true if it is now on.
    bool ToggleBypass();
    bool Ready();

    struct Frame
    {
        rhi::Texture* color = nullptr;        // the scene colour resolve (read, then written)
        rhi::Texture* depth = nullptr;        // R32_FLOAT window depth, same size
        rhi::Texture* motion = nullptr;       // R16G16_FLOAT, uv offset to last frame (optional)
        uint32_t width = 0, height = 0;       // scene pixels
        std::array<double, 16> viewProj{};    // world -> clip, column-major m[col * 4 + row]
        float zScale = 1.0f, zOffset = 0.0f;  // window depth = ndc z * zScale + zOffset
        bool reset = false;                   // camera cut or the scene was not drawn last frame: no history
        uint64_t frame = 0;                   // renderer frame (descriptor slots, noise)
    };

    // Records the passes. Returns false if nothing was done.
    bool Apply(const Frame& frame, const Options& options);

    // GPU time of the passes, averaged since the last call (ms), and frames counted.
    double TakeGpuMs(uint32_t& frames);
}
