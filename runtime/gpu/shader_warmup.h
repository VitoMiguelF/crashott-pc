#pragma once

// Shaders compiled ahead of use (docs/internals/native-gpu-plan.md, M1).
//
// The game's shaders are Xenos microcode: the pixel shaders and a few vertex shaders are files in
// <game>\shaders\*.out (or data in the executable), but most vertex shaders that reach the GPU are
// made at run time: Xbox D3D binds a vertex shader to each vertex declaration and rewrites its fetch
// instructions. So the full set can't be listed from the game files. The runtime records the
// microcode of every shader it translates (userdata\shadercache\ucode\, local, never shipped), and
// translates + compiles to SPIR-V, for both variants (720p and scaled: renderer.h), every shader it
// knows: the game's pixel shader files and the recorded ones.
//   - at every start, on a low-priority thread (StartBackground): a resolution change, a new
//     driver-independent cache or an update of the translator no longer compiles in the middle of
//     a level;
//   - CrashRecomp.exe --precompile-shaders: the same on every core, for the installer/launcher.

#include "rhi.h"
#include "shader_translator.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace gpu::warmup
{
    // A shader container (*.out): magic 0x102A1100 (pixel) / 0x102A1101 (vertex). The microcode
    // in host order. False if the file is not one.
    bool ReadShaderContainer(const std::filesystem::path& path, ShaderStage& stage, std::vector<uint32_t>& ucode);

    // The first time a shader is translated in this run (the renderer's thread).
    void Record(ShaderStage stage, uint64_t hash, const uint32_t* hostOrderUcode, uint32_t dwordCount);

    // Compiles what is missing from the cache on a low-priority thread. `device` (optional) gets
    // the variant in use (`scaled`) of each one to prepare its pipeline part (rhi::Device::PrewarmShader).
    void StartBackground(const std::filesystem::path& shaderCache, rhi::Device* device = nullptr, bool scaled = false);

    // --precompile-shaders: compiles what is missing on every core; the number of failures.
    int Precompile(const std::filesystem::path& shaderCache);
}
