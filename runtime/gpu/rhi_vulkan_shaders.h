#pragma once

// HLSL -> SPIR-V with DXC for the Vulkan backend (see rhi_vulkan_shaders.cpp for the binding maps).

#include "rhi.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gpu::rhi::vulkan
{
    // Descriptor sets of the Draw layout.
    constexpr uint32_t DRAW_SET_TEXTURES = 0;   // push descriptors
    constexpr uint32_t DRAW_SET_CONSTANTS = 1;
    constexpr uint32_t DRAW_SET_SAMPLERS = 2;

    // Compiles (or reads from <cache>\<hash>.spv) `hlsl` for `stage` with the registers of `layout`.
    // False (logged, the HLSL written to shaderdump\) if it doesn't compile.
    bool CompileSpirv(Stage stage, const std::string& hlsl, const char* name, Layout layout, const std::filesystem::path& cache,
                      std::vector<uint32_t>& spirv, uint64_t* compiles = nullptr, uint64_t* failures = nullptr);

    // dxcompiler.dll loads and generates SPIR-V (the Windows SDK's copy has no SPIR-V backend).
    bool CompilerAvailable();
}
