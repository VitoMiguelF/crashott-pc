#pragma once

// SMAA 1x (thirdparty/smaa, MIT): post-process anti-aliasing of the final image, settings
// video.antialiasing = "smaa". Luma edge detection, preset HIGH, three passes (edges, blending
// weights, neighbourhood blending) at the size of the image, with the official AreaTex/SearchTex
// lookup textures. Runs on the command processor thread, from renderer::Swap.
//
// It keeps its own root signature, PSOs, render targets and a small shader-visible descriptor
// heap, so it doesn't touch the renderer's state (the renderer re-binds its heaps afterwards).

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <string>

namespace gpu::smaa
{
    using CompileFunction = std::function<Microsoft::WRL::ComPtr<ID3DBlob>(const std::string& source, const char* target, const char* name)>;

    // Compiles the shaders and records the upload of the lookup textures on `list`. Returns false
    // (and logs why) if SMAA can't be used.
    bool Init(ID3D12Device* device, ID3D12GraphicsCommandList* list, const CompileFunction& compile);
    bool Ready();

    // Anti-aliases `source` (RGBA8, in a shader resource state, width x height) into
    // `destination` (an RTV of the same size, in RENDER_TARGET state). `frame` is the swap number
    // (descriptor and resize bookkeeping: at most 3 frames are in flight).
    void Apply(ID3D12GraphicsCommandList* list, ID3D12Resource* source, D3D12_CPU_DESCRIPTOR_HANDLE destination, uint32_t width,
               uint32_t height, uint64_t frame);
}
