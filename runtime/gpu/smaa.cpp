#include "smaa.h"
#include "../diagnostics.h"

#include <cstring>
#include <string>

#include "../../thirdparty/smaa/AreaTex.h"
#include "../../thirdparty/smaa/SearchTex.h"
#include "../../thirdparty/smaa/SMAA.hlsl.inc"

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

        bool g_ready = false;
        rhi::Device* g_device = nullptr;
        rhi::Pipeline* g_edgesPipeline = nullptr;
        rhi::Pipeline* g_weightsPipeline = nullptr;
        rhi::Pipeline* g_blendPipeline = nullptr;
        rhi::Texture* g_areaTex = nullptr;
        rhi::Texture* g_searchTex = nullptr;
        rhi::Texture* g_edges = nullptr;
        rhi::Texture* g_weights = nullptr;
        uint32_t g_width = 0, g_height = 0;

        rhi::Pipeline* CreatePipeline(const char* pass, DXGI_FORMAT format)
        {
            std::string body = std::string(PREFIX) + SMAA_HLSL_SOURCE + SUFFIX;
            std::string define = std::string("#define ") + pass + " 1\n";
            rhi::PassPipelineDesc desc;
            desc.name = std::string("smaa_") + pass;
            desc.vs = "#define SMAA_STAGE_VS 1\n" + define + body;
            desc.ps = define + body;
            desc.targets[0] = format;
            desc.textureCount = 3;
            desc.constantBytes = 16;
            // Linear (s0) and point (s1) clamp samplers.
            desc.samplers[0].minFilter = desc.samplers[0].magFilter = rhi::Filter::Linear;
            desc.samplerCount = 2;
            return g_device->CreatePassPipeline(desc);
        }

        bool EnsureTargets(uint32_t width, uint32_t height)
        {
            if (g_edges && g_width == width && g_height == height)
                return true;
            for (rhi::Texture** target : { &g_edges, &g_weights })
            {
                if (*target)
                    g_device->DestroyTexture(*target);
                *target = nullptr;
            }
            rhi::TextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.usage = rhi::USAGE_SAMPLED | rhi::USAGE_RENDER_TARGET;
            desc.format = DXGI_FORMAT_R8G8_UNORM;
            desc.name = "smaa edges";
            g_edges = g_device->CreateTexture(desc);
            desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.name = "smaa weights";
            g_weights = g_device->CreateTexture(desc);
            if (!g_edges || !g_weights)
            {
                Log("[gpu] SMAA: could not create %ux%u targets", width, height);
                return false;
            }
            g_width = width;
            g_height = height;
            return true;
        }

        rhi::Texture* CreateLookup(uint32_t width, uint32_t height, uint32_t pitch, DXGI_FORMAT format, const unsigned char* bytes, const char* name)
        {
            rhi::TextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.format = format;
            desc.name = name;
            rhi::Texture* texture = g_device->CreateTexture(desc);
            if (!texture)
                return nullptr;
            rhi::Footprint footprint = rhi::LevelFootprint(format, width, height, 0);
            rhi::Device::Upload upload = g_device->AllocateUpload(footprint.size, rhi::UPLOAD_PLACEMENT_ALIGNMENT);
            if (!upload.cpu)
                return nullptr;
            for (uint32_t y = 0; y < height; y++)
                memcpy(upload.cpu + size_t(y) * footprint.rowPitch, bytes + size_t(y) * pitch, footprint.rowBytes);
            g_device->CopyUploadToTexture(texture, 0, upload.offset, footprint.rowPitch);
            return texture;
        }
    }

    bool Init(rhi::Device* device)
    {
        g_device = device;
        g_edgesPipeline = CreatePipeline("SMAA_PASS_EDGES", DXGI_FORMAT_R8G8_UNORM);
        g_weightsPipeline = CreatePipeline("SMAA_PASS_WEIGHTS", DXGI_FORMAT_R8G8B8A8_UNORM);
        g_blendPipeline = CreatePipeline("SMAA_PASS_BLEND", DXGI_FORMAT_R8G8B8A8_UNORM);
        if (!g_edgesPipeline || !g_weightsPipeline || !g_blendPipeline)
        {
            Log("[gpu] SMAA: could not compile the shaders");
            return false;
        }
        // Lookup textures, copied once.
        g_areaTex = CreateLookup(AREATEX_WIDTH, AREATEX_HEIGHT, AREATEX_PITCH, DXGI_FORMAT_R8G8_UNORM, areaTexBytes, "smaa area");
        g_searchTex = CreateLookup(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, SEARCHTEX_PITCH, DXGI_FORMAT_R8_UNORM, searchTexBytes, "smaa search");
        if (!g_areaTex || !g_searchTex)
            return false;
        g_ready = true;
        Log("[gpu] SMAA ready (SMAA 1x, preset high)");
        return true;
    }

    bool Ready()
    {
        return g_ready;
    }

    void Apply(rhi::Texture* source, rhi::Texture* destination, uint32_t width, uint32_t height, uint64_t)
    {
        if (!g_ready || !EnsureTargets(width, height))
            return;
        float metrics[4] = { 1.0f / float(width), 1.0f / float(height), float(width), float(height) };
        rhi::PassDraw pass;
        pass.viewport = { 0.0f, 0.0f, float(width), float(height) };
        pass.scissor = { 0, 0, int32_t(width), int32_t(height) };
        pass.constants = metrics;
        pass.constantBytes = sizeof(metrics);
        pass.textureCount = 3;
        const float zero[4] = {};

        g_device->ClearColor(g_edges, zero, nullptr);
        pass.pipeline = g_edgesPipeline;
        pass.targets[0] = g_edges;
        pass.textures[0] = pass.textures[1] = pass.textures[2] = { source };
        g_device->DrawPass(pass);

        g_device->ClearColor(g_weights, zero, nullptr);
        pass.pipeline = g_weightsPipeline;
        pass.targets[0] = g_weights;
        pass.textures[0] = { g_edges };
        pass.textures[1] = { g_areaTex };
        pass.textures[2] = { g_searchTex };
        g_device->DrawPass(pass);

        pass.pipeline = g_blendPipeline;
        pass.targets[0] = destination;
        pass.textures[0] = { source };
        pass.textures[1] = { g_weights };
        pass.textures[2] = { source };
        g_device->DrawPass(pass);
    }
}
