#pragma once

// Graphics options that change how particular game shaders draw (settings.h, [graphics]): small
// edits of the translated HLSL of those shaders (identified by their microcode hash, the names
// in userdata\shaderdump\), driven by per-draw root constants at b3 (xe_effects), so the options
// apply live without recompiling anything.
//
//   motion blur     ps 1577d3c304364818 (shaders\motionblurfp): both velocity maps are scaled
//                   around their neutral value (0x80) and Params.zw (the camera blur) by the same
//                   factor; 0 gives back the unblurred frame.
//   depth of field  ps 45775928e9ebd556 (shaders\DOF): MaxRadius (c5) is scaled; 0 makes every
//                   tap read the centre texel.
//   shadow softness ps fdf408078ea760bb (shaders\shadowshaderfp): the 9 UV offsets of the stencil
//                   PCF (c2-c10) are scaled; with soft shadows on (xe_jitter.z, lighting.h) the
//                   PCF is replaced by a rotated 16-tap disk of 4-texel stencil tests.
//
// xe_effects also carries the sub-pixel jitter of the temporal upscalers (upscaler.h), which the
// vertex shader epilogue adds to perspective 3D draws.

#include "shader_translator.h"

#include <cstdint>
#include <string>

namespace gpu::graphics
{
    // Must match xe_effects in shader_translator.cpp.
    struct EffectConstants
    {
        float motionBlur = 1.0f;      // velocity scale of the motion blur pass
        float depthOfField = 1.0f;    // blur radius scale of the depth of field pass
        float shadowSoftness = 1.0f;  // filter radius scale of the stencil shadow pass
        float textureLodScale = 1.0f; // gradient scale of mipmapped fetches (upscaling: render / output)
        float jitter[4] = {};         // xy: sub-pixel offset in NDC added to perspective draws;
                                      // z: soft shadows (lighting remaster, > 0.5 = on); w: unused
        float hud[4] = {};            // wide screens (widescreen.h): xy scale of 2D draws, z mode (0 = none), w anchor shift
        float hudTarget[4] = {};      // xy: guest size of the target's NDC, zw: the screen (1280x720)
    };
    constexpr uint32_t EFFECT_CONSTANT_DWORDS = sizeof(EffectConstants) / 4;

    // Edits the HLSL of the game shaders the options apply to. Returns the name of the patch (for
    // the log), or nullptr if the shader has none or the code wasn't where it was expected.
    const char* PatchShader(ShaderStage stage, uint64_t hash, std::string& hlsl);
}
