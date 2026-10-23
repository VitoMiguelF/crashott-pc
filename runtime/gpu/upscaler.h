#pragma once

// Temporal upscaling / anti-aliasing of the 3D scene (settings [graphics] upscaler):
//
//   - NVIDIA DLSS Super Resolution / DLAA, through the NGX SDK (thirdparty/dlss: nvsdk_ngx_s.lib,
//     nvngx_dlss.dll next to the exe). NVIDIA RTX GPUs only.
//   - AMD FSR 3.1 upscaler, through the FidelityFX API (thirdparty/fsr: the signed
//     amd_fidelityfx_loader_dx12.dll / amd_fidelityfx_upscaler_dx12.dll, loaded at run time). Any
//     GPU with Shader Model 6.2.
//
// Upscaling only: no frame generation. The renderer (xenos_renderer.cpp, "Temporal upscaling")
// jitters the scene's projection, builds the motion vectors and calls Evaluate between the 3D
// scene and the game's 2D passes (post-processing and HUD), which then draw at the output size.

#include "rhi.h"
#include "../settings/settings.h"

#include <cstdint>
#include <string>

namespace gpu::upscaler
{
    // Whether that upscaler can run on this machine (its DLL was found and the GPU and driver
    // support it). Known once the renderer has started; false before.
    bool Available(settings::Upscaler upscaler);

    // Renderer startup: probes DLSS and FSR on `device` (for Available) and chooses `wanted` if
    // it is available. `dataDir`: where NGX may write its logs.
    void Init(rhi::Device* device, settings::Upscaler wanted, const std::wstring& dataDir);
    // Switches to `wanted` (Off if it isn't available) and releases the other one's feature or
    // context. Only with the GPU idle (the renderer's Reconfigure).
    void Select(settings::Upscaler wanted);
    void Shutdown();

    // The upscaler that runs (Off if none was wanted or the wanted one isn't available).
    settings::Upscaler Active();
    // Name and version for the log and the overlay, e.g. "DLSS 310.9.16" or "FSR 3.1.5".
    const std::string& ActiveName();

    // Sub-pixel jitter of frame `index` (Halton 2,3), in render pixels within [-0.5, 0.5], with the
    // phase count both SDKs recommend for that scale.
    void Jitter(uint64_t index, uint32_t renderWidth, uint32_t outputWidth, float& x, float& y);

    struct Frame
    {
        // Inputs (render size, top-left sub-rectangle of the textures).
        rhi::Texture* color = nullptr;   // RGBA8, gamma-encoded (the game renders in LDR)
        rhi::Texture* depth = nullptr;   // R32_FLOAT, the scene's depth buffer values
        rhi::Texture* motion = nullptr;  // RG16_FLOAT, current -> previous position in UV units
        // Output (output size, USAGE_STORAGE).
        rhi::Texture* output = nullptr;  // RGBA8_UNORM
        uint32_t renderWidth = 0, renderHeight = 0;
        uint32_t outputWidth = 0, outputHeight = 0;
        float jitterX = 0.0f, jitterY = 0.0f;  // render pixels, as applied to the projection
        bool reset = false;                    // discontinuity: history must be dropped
        bool depthInverted = false;            // near = 1, far = 0
        float sharpness = 0.0f;                // 0..1 (FSR's RCAS)
        float frameTimeMs = 33.3f;
        float cameraNear = 0.1f, cameraFar = 1000.0f, fovY = 1.0f;
        settings::UpscalerQuality quality = settings::UpscalerQuality::Native;
    };
    // Records the upscaling of one frame on the backend's command list. False if it failed (the
    // output was not written).
    bool Evaluate(const Frame& frame);
}
