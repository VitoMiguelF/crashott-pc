#include "smaa.h"
#include "../diagnostics.h"

#include <cstring>
#include <deque>
#include <utility>

#include "../../thirdparty/smaa/AreaTex.h"
#include "../../thirdparty/smaa/SearchTex.h"
#include "../../thirdparty/smaa/SMAA.hlsl.inc"

using Microsoft::WRL::ComPtr;

namespace gpu::smaa
{
    namespace
    {
        // SMAA's porting layer (SMAA_CUSTOM_SL) for plain SM 5.1 shaders: samplers bound to
        // registers instead of effect-framework state blocks.
        const char* PREFIX = R"(
cbuffer SmaaConstants : register(b0) { float4 smaa_rt_metrics; };
#define SMAA_RT_METRICS smaa_rt_metrics
#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
)";

        // One full-screen triangle per pass; the pass and stage come from defines.
        const char* SUFFIX = R"(
Texture2D t0 : register(t0);
Texture2D t1 : register(t1);
Texture2D t2 : register(t2);
struct SmaaVertex
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
    float2 pixcoord : TEXCOORD1;
    float4 offset0 : TEXCOORD2;
    float4 offset1 : TEXCOORD3;
    float4 offset2 : TEXCOORD4;
};
#ifdef SMAA_STAGE_VS
SmaaVertex main(uint id : SV_VertexID)
{
    SmaaVertex v = (SmaaVertex)0;
    float2 uv = float2((id << 1) & 2, id & 2);
    v.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    v.texcoord = uv;
    float4 offset[3] = { float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0) };
#if defined(SMAA_PASS_EDGES)
    SMAAEdgeDetectionVS(v.texcoord, offset);
#elif defined(SMAA_PASS_WEIGHTS)
    SMAABlendingWeightCalculationVS(v.texcoord, v.pixcoord, offset);
#else
    SMAANeighborhoodBlendingVS(v.texcoord, offset[0]);
#endif
    v.offset0 = offset[0];
    v.offset1 = offset[1];
    v.offset2 = offset[2];
    return v;
}
#else
#if defined(SMAA_PASS_EDGES)
float2 main(SmaaVertex v) : SV_Target
{
    float4 offset[3] = { v.offset0, v.offset1, v.offset2 };
    return SMAALumaEdgeDetectionPS(v.texcoord, offset, t0);
}
#elif defined(SMAA_PASS_WEIGHTS)
float4 main(SmaaVertex v) : SV_Target
{
    float4 offset[3] = { v.offset0, v.offset1, v.offset2 };
    return SMAABlendingWeightCalculationPS(v.texcoord, v.pixcoord, offset, t0, t1, t2, float4(0.0, 0.0, 0.0, 0.0));
}
#else
float4 main(SmaaVertex v) : SV_Target
{
    return SMAANeighborhoodBlendingPS(v.texcoord, v.offset0, t0, t1);
}
#endif
#endif
)";

        constexpr uint32_t FRAME_SLOTS = 3;           // frames in flight (renderer keeps at most 2 queued)
        constexpr uint32_t DESCRIPTORS_PER_FRAME = 9; // 3 passes x 3 SRVs

        struct Target
        {
            ComPtr<ID3D12Resource> resource;
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        };

        bool g_ready = false;
        ID3D12Device* g_device = nullptr;
        ComPtr<ID3D12RootSignature> g_rootSignature;
        ComPtr<ID3D12PipelineState> g_edgesPipeline, g_weightsPipeline, g_blendPipeline;
        ComPtr<ID3D12DescriptorHeap> g_srvHeap, g_rtvHeap;
        uint32_t g_srvSize = 0, g_rtvSize = 0;
        ComPtr<ID3D12Resource> g_areaTex, g_searchTex, g_upload;
        Target g_edges, g_weights;
        uint32_t g_width = 0, g_height = 0;
        // Resources of older sizes, released once no frame in flight can use them.
        std::deque<std::pair<uint64_t, ComPtr<ID3D12Resource>>> g_retired;

        void Transition(ID3D12GraphicsCommandList* list, Target& target, D3D12_RESOURCE_STATES state)
        {
            if (target.state == state)
                return;
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = target.resource.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = target.state;
            barrier.Transition.StateAfter = state;
            list->ResourceBarrier(1, &barrier);
            target.state = state;
        }

        ComPtr<ID3D12Resource> CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Flags = flags;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_CLEAR_VALUE clear{};
            clear.Format = format;
            ComPtr<ID3D12Resource> resource;
            if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                         (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? &clear : nullptr, IID_PPV_ARGS(&resource))))
                return nullptr;
            return resource;
        }

        ComPtr<ID3D12PipelineState> CreatePipeline(const CompileFunction& compile, const char* pass, DXGI_FORMAT format)
        {
            std::string body = std::string(PREFIX) + SMAA_HLSL_SOURCE + SUFFIX;
            std::string define = std::string("#define ") + pass + " 1\n";
            ComPtr<ID3DBlob> vs = compile("#define SMAA_STAGE_VS 1\n" + define + body, "vs_5_1", (std::string("smaa_vs_") + pass).c_str());
            ComPtr<ID3DBlob> ps = compile(define + body, "ps_5_1", (std::string("smaa_ps_") + pass).c_str());
            if (!vs || !ps)
                return nullptr;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = g_rootSignature.Get();
            desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
            desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = format;
            desc.SampleDesc.Count = 1;
            ComPtr<ID3D12PipelineState> pipeline;
            if (FAILED(g_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))))
                return nullptr;
            return pipeline;
        }

        bool EnsureTargets(uint32_t width, uint32_t height, uint64_t frame)
        {
            while (!g_retired.empty() && g_retired.front().first + FRAME_SLOTS <= frame)
                g_retired.pop_front();
            if (g_edges.resource && g_width == width && g_height == height)
                return true;
            for (Target* target : { &g_edges, &g_weights })
            {
                if (target->resource)
                    g_retired.push_back({ frame, target->resource });
                *target = Target{};
            }
            g_edges.resource = CreateTexture(width, height, DXGI_FORMAT_R8G8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
            g_weights.resource = CreateTexture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (!g_edges.resource || !g_weights.resource)
            {
                Log("[gpu] SMAA: could not create %ux%u targets", width, height);
                g_edges = g_weights = Target{};
                return false;
            }
            for (uint32_t i = 0; i < 2; i++)
            {
                Target& target = i ? g_weights : g_edges;
                target.rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
                target.rtv.ptr += size_t(i) * g_rtvSize;
                g_device->CreateRenderTargetView(target.resource.Get(), nullptr, target.rtv);
            }
            g_width = width;
            g_height = height;
            return true;
        }
    }

    bool Init(ID3D12Device* device, ID3D12GraphicsCommandList* list, const CompileFunction& compile)
    {
        g_device = device;

        // Root signature: SRV table t0-t2, metrics at b0, linear (s0) and point (s1) clamp samplers.
        D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable = { 1, &range };
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants = { 0, 0, 4 };
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        for (uint32_t i = 0; i < 2; i++)
        {
            samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;
            samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
            samplers[i].ShaderRegister = i;
            samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_ROOT_SIGNATURE_DESC rootDesc{ 2, parameters, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> blob, errors;
        if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature))))
        {
            Log("[gpu] SMAA: could not create the root signature");
            return false;
        }

        g_edgesPipeline = CreatePipeline(compile, "SMAA_PASS_EDGES", DXGI_FORMAT_R8G8_UNORM);
        g_weightsPipeline = CreatePipeline(compile, "SMAA_PASS_WEIGHTS", DXGI_FORMAT_R8G8B8A8_UNORM);
        g_blendPipeline = CreatePipeline(compile, "SMAA_PASS_BLEND", DXGI_FORMAT_R8G8B8A8_UNORM);
        if (!g_edgesPipeline || !g_weightsPipeline || !g_blendPipeline)
        {
            Log("[gpu] SMAA: could not compile the shaders");
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC srvHeap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, FRAME_SLOTS * DESCRIPTORS_PER_FRAME, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeap{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
        if (FAILED(device->CreateDescriptorHeap(&srvHeap, IID_PPV_ARGS(&g_srvHeap))) || FAILED(device->CreateDescriptorHeap(&rtvHeap, IID_PPV_ARGS(&g_rtvHeap))))
            return false;
        g_srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g_rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        // Lookup textures: one upload buffer holding both, copied once.
        g_areaTex = CreateTexture(AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        g_searchTex = CreateTexture(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!g_areaTex || !g_searchTex)
            return false;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2];
        UINT rows[2];
        UINT64 rowBytes[2], sizes[2];
        D3D12_RESOURCE_DESC descs[2] = { g_areaTex->GetDesc(), g_searchTex->GetDesc() };
        uint64_t total = 0;
        for (int i = 0; i < 2; i++)
        {
            device->GetCopyableFootprints(&descs[i], 0, 1, total, &footprints[i], &rows[i], &rowBytes[i], &sizes[i]);
            total = (footprints[i].Offset + sizes[i] + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) / D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        }
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = total;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
        if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_upload))))
            return false;
        uint8_t* mapped = nullptr;
        D3D12_RANGE noRead{ 0, 0 };
        g_upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
        const unsigned char* sources[2] = { areaTexBytes, searchTexBytes };
        const uint32_t pitches[2] = { AREATEX_PITCH, SEARCHTEX_PITCH };
        for (int i = 0; i < 2; i++)
        {
            for (UINT y = 0; y < rows[i]; y++)
                memcpy(mapped + footprints[i].Offset + size_t(y) * footprints[i].Footprint.RowPitch, sources[i] + size_t(y) * pitches[i], size_t(rowBytes[i]));
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = g_upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprints[i];
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = i == 0 ? g_areaTex.Get() : g_searchTex.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = destination.pResource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            list->ResourceBarrier(1, &barrier);
        }
        g_upload->Unmap(0, nullptr);
        g_ready = true;
        Log("[gpu] SMAA ready (SMAA 1x, preset high)");
        return true;
    }

    bool Ready()
    {
        return g_ready;
    }

    void Apply(ID3D12GraphicsCommandList* list, ID3D12Resource* source, D3D12_CPU_DESCRIPTOR_HANDLE destination, uint32_t width,
               uint32_t height, uint64_t frame)
    {
        if (!g_ready || !EnsureTargets(width, height, frame))
            return;
        uint32_t base = uint32_t(frame % FRAME_SLOTS) * DESCRIPTORS_PER_FRAME;
        auto cpu = [&](uint32_t index) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += size_t(base + index) * g_srvSize;
            return handle;
        };
        auto gpu = [&](uint32_t index) {
            D3D12_GPU_DESCRIPTOR_HANDLE handle = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
            handle.ptr += uint64_t(base + index) * g_srvSize;
            return handle;
        };
        D3D12_SHADER_RESOURCE_VIEW_DESC colorView{};
        colorView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        colorView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        colorView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        colorView.Texture2D.MipLevels = 1;
        // Pass tables: [0..2] edges (color), [3..5] weights (edges, area, search), [6..8] blend (color, weights).
        g_device->CreateShaderResourceView(source, &colorView, cpu(0));
        g_device->CreateShaderResourceView(source, &colorView, cpu(1));
        g_device->CreateShaderResourceView(source, &colorView, cpu(2));
        g_device->CreateShaderResourceView(g_edges.resource.Get(), nullptr, cpu(3));
        g_device->CreateShaderResourceView(g_areaTex.Get(), nullptr, cpu(4));
        g_device->CreateShaderResourceView(g_searchTex.Get(), nullptr, cpu(5));
        g_device->CreateShaderResourceView(source, &colorView, cpu(6));
        g_device->CreateShaderResourceView(g_weights.resource.Get(), nullptr, cpu(7));
        g_device->CreateShaderResourceView(source, &colorView, cpu(8));

        ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(g_rootSignature.Get());
        float metrics[4] = { 1.0f / float(width), 1.0f / float(height), float(width), float(height) };
        list->SetGraphicsRoot32BitConstants(1, 4, metrics, 0);
        D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
        D3D12_RECT scissor{ 0, 0, LONG(width), LONG(height) };
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const float zero[4] = {};

        Transition(list, g_edges, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ClearRenderTargetView(g_edges.rtv, zero, 0, nullptr);
        list->OMSetRenderTargets(1, &g_edges.rtv, FALSE, nullptr);
        list->SetPipelineState(g_edgesPipeline.Get());
        list->SetGraphicsRootDescriptorTable(0, gpu(0));
        list->DrawInstanced(3, 1, 0, 0);
        Transition(list, g_edges, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        Transition(list, g_weights, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ClearRenderTargetView(g_weights.rtv, zero, 0, nullptr);
        list->OMSetRenderTargets(1, &g_weights.rtv, FALSE, nullptr);
        list->SetPipelineState(g_weightsPipeline.Get());
        list->SetGraphicsRootDescriptorTable(0, gpu(3));
        list->DrawInstanced(3, 1, 0, 0);
        Transition(list, g_weights, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        list->OMSetRenderTargets(1, &destination, FALSE, nullptr);
        list->SetPipelineState(g_blendPipeline.Get());
        list->SetGraphicsRootDescriptorTable(0, gpu(6));
        list->DrawInstanced(3, 1, 0, 0);
    }
}
