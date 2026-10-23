#pragma once

// SMAA 1x (thirdparty/smaa, MIT): post-process anti-aliasing of the final image, settings
// video.antialiasing = "smaa". Luma edge detection, preset HIGH, three passes (edges, blending
// weights, neighbourhood blending) at the size of the image, with the official AreaTex/SearchTex
// lookup textures. Runs on the command processor thread, from renderer::Swap.
//
// Three passes of the rendering hardware interface (rhi.h): edge and weight targets of its own,
// the lookup textures uploaded once.

#include "rhi.h"

#include <cstdint>

namespace gpu::smaa
{
    // Compiles the shaders and uploads the lookup textures. Returns false (and logs why) if SMAA
    // can't be used.
    bool Init(rhi::Device* device);
    bool Ready();

    // Anti-aliases `source` (RGBA8, width x height) into `destination` (a render target of the same size).
    void Apply(rhi::Texture* source, rhi::Texture* destination, uint32_t width, uint32_t height, uint64_t frame);
}
