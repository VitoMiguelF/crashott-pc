#pragma once

// Host renderer (xenos_renderer.cpp, on Direct3D 12 or Vulkan through rhi.h) driven by the command
// processor thread.
//
// The command processor keeps applying register writes to the register file as before; at each
// draw packet it calls Draw, which reads the render state from the registers, translates the
// current shaders, uploads vertex/index/texture data from guest memory and records a host draw.
// Draws with RB_MODECONTROL = copy are resolves (EDRAM -> texture copies and clears). XE_SWAP
// presents the resolved front buffer in the window.
//
// All functions except Init are called on the command processor thread only.

#include "shader_translator.h"

#include <cstdint>
#include <filesystem>

namespace gpu
{
    struct DrawPacket
    {
        uint32_t drawInitiator;          // VGT_DRAW_INITIATOR
        uint32_t indexBase;              // VGT_DMA_BASE (GPU address), DMA source only
        uint32_t indexSize;              // VGT_DMA_SIZE, DMA source only
        const uint32_t* immediateIndices; // guest pointer to big-endian dwords, immediate source only
        uint32_t immediateDwords;
    };

    namespace renderer
    {
        // Creates the window and the Vulkan device. Without one (no Vulkan 1.3 driver, no
        // dxcompiler.dll with SPIR-V) it logs why, tells the player in a message box and exits.
        bool Init();
        bool IsActive();

        // Translates and compiles every shader container (*.out) in `folder` (the game's shaders\
        // folder): a check of the microcode translator without running the game. Returns the
        // number of failures.
        int TestShaders(const std::filesystem::path& folder);

        void LoadShader(ShaderStage stage, const uint32_t* hostOrderUcode, uint32_t dwordCount);
        // The same, returning the shader for BindShader: the command processor keeps it per
        // microcode address and binds it again while the memory there is unchanged, instead of
        // hashing the microcode (FNV-1a, the shader's key) at each of the ~2 loads per draw.
        void* LoadShaderHandle(ShaderStage stage, const uint32_t* hostOrderUcode, uint32_t dwordCount);
        void BindShader(ShaderStage stage, void* shader);
        void Draw(const DrawPacket& packet);
        // frontBuffer: GPU (physical) address; fetchConstant: the 6 dwords VdSwap was given
        // (host order), or null.
        void Swap(uint32_t frontBuffer, uint32_t width, uint32_t height, const uint32_t* fetchConstant);

        // The internal resolution or the upscaler changed in the settings (settings::RendererDiffers):
        // the renderer rebuilds its render targets at the new sizes and switches the upscaler at
        // the end of the next frame. Any thread.
        void RequestReconfigure();

        void LogStats();
    }
}
