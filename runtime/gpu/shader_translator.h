#pragma once

// Xenos shader microcode -> HLSL (Shader Model 5.1, compiled with FXC at run time).
//
// The microcode comes from PM4_IM_LOAD_IMMEDIATE packets (big-endian dwords, already swapped to
// host order by the caller). Instruction formats and ALU/fetch semantics follow Xenia's ucode.h
// and shader_interpreter.cc (BSD license); the HLSL generation is our own.
//
// Generated shaders share one interface with the renderer (see xenos_renderer.cpp):
//   b0          system constants (viewport transform, flags, alpha test, bool/loop constants,
//               vertex buffer bases, texture swizzles)
//   b1 / b2     float constants of the vertex / pixel shader (256 float4 each)
//   t0-t31      textures (one per texture fetch constant), s0-s31 samplers
//   t0 space1   vertex data (raw buffer; vertex fetch reads are byte-swapped on upload)
//   b3          graphics option constants (graphics_options.h: effect scales, upscaler jitter)
// Vertex shader output = pixel shader input = 16 float4 interpolators (TEXCOORD0-15) + position.

#include <cstdint>
#include <string>
#include <vector>

namespace gpu
{
    enum class ShaderStage : uint32_t
    {
        Vertex = 0,
        Pixel = 1,
    };

    // One per vertex fetch constant used by a vertex shader.
    struct VertexFetchBinding
    {
        uint32_t fetchConstant = 0;  // 0-95
        uint32_t strideDwords = 0;   // from the vfetch_full instruction
        int32_t minOffsetDwords = 0; // smallest element offset in the vertex
        int32_t maxEndDwords = 0;    // largest element offset + element size
        // The fetch index is the unmodified vertex index (r0.x): the uploaded range can be
        // limited to the indices the draw uses.
        bool indexedByVertexIndex = true;
    };

    // One per texture fetch constant used.
    struct TextureBinding
    {
        uint32_t fetchConstant = 0; // 0-31
        uint32_t dimension = 1;     // tfetch dimension: 0 1D, 1 2D, 2 3D/stacked, 3 cube
        // Filter overrides from the (first) tfetch instruction; 3 = use the fetch constant.
        uint32_t magFilter = 3, minFilter = 3, mipFilter = 3, anisoFilter = 7;
    };

    struct TranslatedShader
    {
        ShaderStage stage = ShaderStage::Vertex;
        uint64_t hash = 0;
        std::string hlsl;
        std::vector<VertexFetchBinding> vertexFetches;
        std::vector<TextureBinding> textures;
        uint32_t colorTargetsWritten = 0; // pixel shader: bit per oC#
        bool writesDepth = false;
        bool hasKill = false;
        bool hasMemExport = false;
        bool usesControlFlowMachine = false;
        uint32_t instructionCount = 0;
    };

    uint64_t HashUcode(ShaderStage stage, const uint32_t* ucode, uint32_t dwordCount);
    // Internal resolution above 720p (settings video.resolution): xe_system gains xe_render_scale
    // (x = guest pixels per host pixel) and xe_tex_size_scale (guest texels per host texel of each
    // fetch constant: below 1 for resolve targets), used for unnormalized coordinates, texel offsets,
    // getWeights and the pixel position of param gen. Set before translating; at 1x the HLSL is
    // unchanged.
    void SetRenderScaled(bool scaled);
    // Whether texture fetches may sample with implicit derivatives where the whole quad runs them
    // (rhi::Device::SupportsImplicitDerivatives; on by default). CRASH_SHADER_DERIVATIVES
    // (gradients / linear / implicit) overrides it. Set before translating.
    void SetImplicitDerivatives(bool enabled);
    TranslatedShader TranslateShader(ShaderStage stage, const uint32_t* ucode, uint32_t dwordCount);
    // The same for a given variant instead of the current one (any thread: shader_warmup.cpp).
    TranslatedShader TranslateShader(ShaderStage stage, const uint32_t* ucode, uint32_t dwordCount, bool renderScaled);
}
