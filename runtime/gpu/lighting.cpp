#include "lighting.h"
#include "../diagnostics.h"
#include "../settings/settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <vector>

namespace gpu::lighting
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        // One source, one entry point per pass (compiled with -D<PASS>).
        const char* LIGHTING_HLSL = R"(
cbuffer constants : register(b0)
{
    float4 proj;          // view -> clip: P00, P02, P11, P12 (clip.x = P00 x + P02 z, clip.y = P11 y + P12 z)
    float4 projZ;         // P22, P23, P32, P33 (clip.z = P22 z + P23, clip.w = P32 z + P33)
    float4 depthParams;   // window depth = ndc z * x + y; z: far fade start (view z); w: sky (view z beyond it)
    float4 sizes;         // AO target width, height; scene width, height
    float4 aoParams;      // radius, thickness (world units), AO strength, indirect light intensity
    float4 lightView;     // contact shadow ray direction (world up, in view space), strength (0: none)
    float4 shadowParams;  // ray length, thickness (world units), far fade end (view z), unused
    uint4 control;        // x: flags, y: frame, z: slices, w: steps per side
};
Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t2 : register(t2);
Texture2D<float4> t3 : register(t3);
Texture2D<float4> t4 : register(t4);
Texture2D<float4> t5 : register(t5);
Texture2D<float4> t6 : register(t6);
SamplerState pointClamp : register(s0);
SamplerState linearClamp : register(s1);

#define FLAG_AO 1u
#define FLAG_IL 2u
#define FLAG_SHADOW 4u
#define FLAG_TEMPORAL 8u
#define FLAG_HALF 16u
#define FLAG_SHOW_AO 32u
#define FLAG_SHOW_IL 64u
#define FLAG_SHOW_SHADOW 128u
#define FLAG_HISTORY 256u
#define FLAG_VERTICAL 512u
#define FLAG_SPLIT 1024u

static const float PI = 3.14159265;

// View z of a window depth; 0 for the sky / cleared depth and beyond the sky distance.
float ViewZ(float d)
{
    float ndcZ = (d - depthParams.y) / depthParams.x;
    float z = (projZ.y - ndcZ * projZ.w) / (ndcZ * projZ.z - projZ.x);
    return (z > 0.0 && z < depthParams.w) ? z : 0.0;
}

float3 ViewPos(float2 uv, float z)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float w = projZ.z * z + projZ.w;
    return float3((ndc.x * w - proj.y * z) / proj.x, (ndc.y * w - proj.w * z) / proj.z, z);
}

float2 ProjectUV(float3 v)
{
    float w = projZ.z * v.z + projZ.w;
    float2 ndc = float2(proj.x * v.x + proj.y * v.z, proj.z * v.y + proj.w * v.z) / w;
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float Scale() { return (control.x & FLAG_HALF) ? 2.0 : 1.0; }

// Scene uv of the scene pixel an AO texel stands for (its top-left one at half resolution).
float2 RepUV(int2 ap) { return (float2(ap) * Scale() + 0.5) / sizes.zw; }

// Interleaved gradient noise (Jimenez 2014), shifted every frame when accumulating.
float Ign(float2 p, uint frame)
{
    p += 5.588238 * float(frame & 63u);
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

float FastAcos(float x)
{
    float ax = min(abs(x), 1.0);
    float r = ((-0.0187293 * ax + 0.0742610) * ax - 0.2121144) * ax + 1.5707288;
    r *= sqrt(1.0 - ax);
    return x >= 0.0 ? r : PI - r;
}

// Bits of the 32 sectors of [0, 1] covered by [a, b].
uint SectorBits(float a, float b)
{
    a = saturate(a);
    b = saturate(b);
    uint start = min(uint(a * 32.0), 31u);
    uint count = uint(ceil((b - a) * 32.0));
    if (count == 0u)
        return 0u;
    uint bits = count >= 32u ? 0xFFFFFFFFu : ((1u << count) - 1u);
    return bits << start;
}

float FarFade(float z) { return 1.0 - smoothstep(depthParams.z, shadowParams.z, z); }

#ifdef PREP
// t0: scene depth. -> view z, view normal (from the depth of the neighbours on the flatter side).
float LoadZ(int2 p)
{
    p = clamp(p, int2(0, 0), int2(sizes.zw) - 1);
    return ViewZ(t0.Load(int3(p, 0)).x);
}
float3 PosAt(int2 p, float z) { return ViewPos((float2(p) + 0.5) / sizes.zw, z); }
// Cracks between meshes (1-2 pixels where what is behind, or the sky, shows through, e.g. the
// terrain seams of the village) would be deep trenches for the AO: take the depth of the sides.
float HealedZ(int2 p)
{
    float z = LoadZ(p);
    [unroll] for (int axis = 0; axis < 2; axis++)
    {
        int2 d = axis == 0 ? int2(1, 0) : int2(0, 1);
        float a1 = LoadZ(p - d), b1 = LoadZ(p + d), a2 = LoadZ(p - 2 * d), b2 = LoadZ(p + 2 * d);
        float2 pairs[3] = { float2(a1, b1), float2(a1, b2), float2(a2, b1) };
        [unroll] for (int i = 0; i < 3; i++)
        {
            float a = pairs[i].x, b = pairs[i].y, nearest = max(a, b);
            if (a > 0.0 && b > 0.0 && abs(a - b) < 0.02 * nearest + 0.03 && (z <= 0.0 || z - nearest > 0.003 * z + 0.01))
                return 0.5 * (a + b);
        }
    }
    return z;
}
struct Out { float z : SV_Target0; float4 n : SV_Target1; };
Out main(float4 position : SV_Position)
{
    int2 p = int2(position.xy) * int(Scale());
    Out o;
    float z = HealedZ(p);
    o.z = z;
    o.n = 0.0;
    if (z <= 0.0)
        return o;
    float3 c = PosAt(p, z);
    float zr = LoadZ(p + int2(1, 0)), zl = LoadZ(p - int2(1, 0)), zd = LoadZ(p + int2(0, 1)), zu = LoadZ(p - int2(0, 1));
    bool right = zr > 0.0 && (zl <= 0.0 || abs(zr - z) < abs(zl - z));
    bool down = zd > 0.0 && (zu <= 0.0 || abs(zd - z) < abs(zu - z));
    float3 dx = right ? PosAt(p + int2(1, 0), zr) - c : (zl > 0.0 ? c - PosAt(p - int2(1, 0), zl) : float3(1, 0, 0));
    float3 dy = down ? PosAt(p + int2(0, 1), zd) - c : (zu > 0.0 ? c - PosAt(p - int2(0, 1), zu) : float3(0, -1, 0));
    float3 n = cross(dx, dy);
    n = dot(n, n) > 1e-20 ? normalize(n) : -normalize(c);
    if (dot(n, c) > 0.0)
        n = -n;
    o.n = float4(n, 1.0);
    return o;
}
#endif

#ifdef GATHER
// t0: view z, t1: normal (AO resolution), t2: scene colour, t3: scene depth.
// -> (indirect light rgb, visibility), contact shadow visibility.
struct Out { float4 a : SV_Target0; float s : SV_Target1; };
Out main(float4 position : SV_Position)
{
    Out o;
    o.a = float4(0, 0, 0, 1);
    o.s = 1.0;
    int2 ap = int2(position.xy);
    float z = t0.Load(int3(ap, 0)).x;
    if (z <= 0.0)
        return o;
    float3 N = t1.Load(int3(ap, 0)).xyz;
    float3 P = ViewPos(RepUV(ap), z);
    float3 V = normalize(-P);
    float fade = FarFade(z);
    if (fade <= 0.0)
        return o;
    uint frame = (control.x & FLAG_TEMPORAL) ? control.y : 0u;
    float noiseSlice = Ign(float2(ap), frame);
    float noiseStep = Ign(float2(ap) + float2(19.0, 47.0), frame);

    float w = projZ.z * z + projZ.w;
    float pixelsPerUnit = proj.z * 0.5 * sizes.y / w;
    float radius = aoParams.x;
    float radiusPx = min(radius * pixelsPerUnit, 0.2 * sizes.y);
    if ((control.x & (FLAG_AO | FLAG_IL)) && radiusPx >= 2.0)
    {
        float3 indirect = 0.0;
        float visibility = 0.0;
        uint slices = control.z, steps = control.w;
        [loop] for (uint s = 0; s < slices; s++)
        {
            float phi = (float(s) + noiseSlice) * PI / float(slices);
            float2 omega = float2(cos(phi), sin(phi));
            float3 direction = float3(omega.x, -omega.y, 0.0);
            float3 ortho = direction - dot(direction, V) * V;
            float3 axis = cross(direction, V);
            float3 projN = N - axis * dot(N, axis) / max(dot(axis, axis), 1e-8);
            float cosN = saturate(dot(projN, V) / max(length(projN), 1e-5));
            float n = (dot(ortho, projN) >= 0.0 ? 1.0 : -1.0) * FastAcos(cosN);
            uint mask = 0u;
            [loop] for (uint side = 0; side < 2u; side++)
            {
                float sgn = side == 0u ? 1.0 : -1.0;
                [loop] for (uint k = 0; k < steps; k++)
                {
                    float t = (float(k) + frac(noiseStep + 0.5 * float(side))) / float(steps);
                    float offset = max(t * t * radiusPx, float(k) + 1.0);
                    int2 sp = int2(floor(float2(ap) + 0.5 + sgn * omega * offset));
                    if (any(sp < 0) || any(sp >= int2(sizes.xy)))
                        break;
                    float sz = t0.Load(int3(sp, 0)).x;
                    if (sz <= 0.0)
                        continue;
                    float3 delta = ViewPos(RepUV(sp), sz) - P;
                    float len = length(delta);
                    if (len < 1e-5 || len > 2.0 * radius)
                        continue;
                    // Small steps (terrain seams, decals) barely above the surface: no occluders.
                    if (dot(delta, N) < 0.07 + 0.004 * z)
                        continue;
                    float3 L = delta / len;
                    float front = FastAcos(dot(L, V));
                    float back = FastAcos(dot(normalize(delta - V * aoParams.y), V));
                    float a0 = sgn > 0.0 ? front : -back;
                    float a1 = sgn > 0.0 ? back : -front;
                    uint bits = SectorBits((a0 - n) / PI + 0.5, (a1 - n) / PI + 0.5);
                    // Farther than the radius: occludes less, fading out at twice the radius.
                    if (len > radius)
                        bits &= (uint(saturate(2.0 - len / radius) * 32.0) >= 16u) ? 0xFFFFFFFFu : 0x55555555u;
                    if (control.x & FLAG_IL)
                    {
                        uint fresh = bits & ~mask;
                        if (fresh)
                        {
                            float3 Ns = t1.Load(int3(sp, 0)).xyz;
                            float emit = saturate(dot(Ns, -L) * 0.75 + 0.25);
                            float3 c = t2.SampleLevel(linearClamp, (float2(sp) + 0.5) / sizes.xy, 0.0).rgb;
                            indirect += float(countbits(fresh)) * (1.0 / 32.0) * c * emit * saturate(dot(N, L));
                        }
                    }
                    mask |= bits;
                }
            }
            visibility += 1.0 - float(countbits(mask)) * (1.0 / 32.0);
        }
        visibility /= float(slices);
        indirect /= float(slices);
        o.a = float4(indirect * fade, lerp(1.0, visibility, fade));
    }

    // Contact shadows: a short march upwards (lightView) in the full resolution depth.
    if ((control.x & FLAG_SHADOW) && lightView.w > 0.0)
    {
        float3 Ld = lightView.xyz;
        float ndl = dot(N, Ld);
        if (ndl > 0.0)
        {
            float3 origin = P + N * (0.003 * z + 0.01);
            float noise = Ign(float2(ap) + float2(71.0, 13.0), frame);
            float occlusion = 0.0;
            const uint STEPS = 16u;
            [loop] for (uint i = 0; i < STEPS; i++)
            {
                float t = (float(i) + noise) / float(STEPS);
                float distance = t * t * shadowParams.x;
                float3 Q = origin + Ld * distance;
                if (Q.z <= 0.05)
                    break;
                float2 quv = ProjectUV(Q);
                if (any(quv <= 0.0) || any(quv >= 1.0))
                    break;
                float sz = ViewZ(t3.Load(int3(int2(quv * sizes.zw), 0)).x);
                if (sz <= 0.0)
                    continue;
                float dz = Q.z - sz;
                if (dz > 0.003 * Q.z + 0.01 && dz < shadowParams.y)
                {
                    occlusion = 1.0 - t * t;
                    break;
                }
            }
            o.s = 1.0 - occlusion * lightView.w * smoothstep(0.0, 0.3, ndl) * fade;
        }
    }
    return o;
}
#endif

#ifdef TEMPORAL
// t0, t1: this frame's gather; t2, t3, t4: history (a, s, view z); t5: motion (scene uv offset to
// last frame); t6: view z.
struct Out { float4 a : SV_Target0; float s : SV_Target1; float z : SV_Target2; };
Out main(float4 position : SV_Position)
{
    int2 ap = int2(position.xy);
    Out o;
    o.a = t0.Load(int3(ap, 0));
    o.s = t1.Load(int3(ap, 0)).x;
    float z = t6.Load(int3(ap, 0)).x;
    o.z = z;
    if (!(control.x & FLAG_HISTORY) || z <= 0.0)
        return o;
    float2 uv = RepUV(ap);
    float2 previous = uv + t5.SampleLevel(pointClamp, uv, 0.0).xy;
    if (any(previous <= 0.0) || any(previous >= 1.0))
        return o;
    // The AO texture's uv of that scene point (its texels stand for their top-left scene pixel).
    float2 aoUV = previous + (Scale() - 1.0) * 0.5 / sizes.zw;
    float hz = t4.SampleLevel(pointClamp, aoUV, 0.0).x;
    if (hz <= 0.0 || abs(hz - z) > 0.04 * z + 0.02)
        return o;
    float4 ha = t2.SampleLevel(linearClamp, aoUV, 0.0);
    float hs = t3.SampleLevel(linearClamp, aoUV, 0.0).x;
    const float blend = 0.12;
    o.a = lerp(ha, o.a, blend);
    o.s = lerp(hs, o.s, blend);
    return o;
}
#endif

#ifdef BLUR
// t0, t1: input (a, s); t2: view z; t3: normal. Horizontal or vertical (FLAG_VERTICAL).
struct Out { float4 a : SV_Target0; float s : SV_Target1; };
Out main(float4 position : SV_Position)
{
    int2 ap = int2(position.xy);
    Out o;
    o.a = t0.Load(int3(ap, 0));
    o.s = t1.Load(int3(ap, 0)).x;
    float z = t2.Load(int3(ap, 0)).x;
    if (z <= 0.0)
        return o;
    float3 n = t3.Load(int3(ap, 0)).xyz;
    int2 direction = (control.x & FLAG_VERTICAL) ? int2(0, 1) : int2(1, 0);
    float4 sumA = o.a;
    float sumS = o.s, total = 1.0;
    [unroll] for (int i = -4; i <= 4; i++)
    {
        if (i == 0)
            continue;
        int2 q = ap + direction * i;
        if (any(q < 0) || any(q >= int2(sizes.xy)))
            continue;
        float zq = t2.Load(int3(q, 0)).x;
        if (zq <= 0.0)
            continue;
        float3 nq = t3.Load(int3(q, 0)).xyz;
        float weight = exp(-float(i * i) / 12.0) * saturate(1.0 - abs(zq - z) / (0.02 * z + 0.01)) * pow(saturate(dot(n, nq)), 8.0);
        sumA += t0.Load(int3(q, 0)) * weight;
        sumS += t1.Load(int3(q, 0)).x * weight;
        total += weight;
    }
    o.a = sumA / total;
    o.s = sumS / total;
    return o;
}
#endif

#ifdef COMPOSITE
// t0: scene colour (copy), t1: scene depth, t2, t3: filtered (a, s), t4: AO view z.
float4 main(float4 position : SV_Position) : SV_Target
{
    int2 p = int2(position.xy);
    float4 c = t0.Load(int3(p, 0));
    // Before / after: the left half keeps the original (a thin line marks the split).
    if (control.x & FLAG_SPLIT)
    {
        int half = int(sizes.z) / 2;
        if (p.x < half - 1)
            return c;
        if (p.x <= half)
            return float4(1, 1, 1, c.a);
    }
    float z = ViewZ(t1.Load(int3(p, 0)).x);
    bool show = (control.x & (FLAG_SHOW_AO | FLAG_SHOW_IL | FLAG_SHOW_SHADOW)) != 0u;
    if (z <= 0.0)
        return show ? float4(1, 1, 1, c.a) * ((control.x & FLAG_SHOW_IL) ? 0.0 : 1.0) : c;
    float4 a;
    float s;
    if (control.x & FLAG_HALF)
    {
        // Bilateral upsampling: the four AO texels around, bilinear weights times depth similarity.
        float2 f = float2(p) * 0.5;
        int2 base = int2(floor(f));
        float2 fr = f - float2(base);
        a = 0.0;
        s = 0.0;
        float total = 0.0;
        [unroll] for (int j = 0; j < 4; j++)
        {
            int2 o = int2(j & 1, j >> 1);
            int2 q = min(base + o, int2(sizes.xy) - 1);
            float zq = t4.Load(int3(q, 0)).x;
            float bilinear = (o.x ? fr.x : 1.0 - fr.x) * (o.y ? fr.y : 1.0 - fr.y);
            float weight = (bilinear + 1e-3) * (zq > 0.0 ? 1.0 / (abs(zq - z) / z + 0.01) : 1e-3);
            a += t2.Load(int3(q, 0)) * weight;
            s += t3.Load(int3(q, 0)).x * weight;
            total += weight;
        }
        a /= total;
        s /= total;
    }
    else
    {
        a = t2.Load(int3(p, 0));
        s = t3.Load(int3(p, 0)).x;
    }
    float ao = 1.0;
    if (control.x & FLAG_AO)
    {
        ao = lerp(1.0, a.a, aoParams.z);
        // Bright pixels (emissive, highlights, the sun on a wall) keep most of their light.
        float bright = smoothstep(0.75, 1.0, max(c.r, max(c.g, c.b)));
        ao = lerp(ao, 1.0, 0.6 * bright);
    }
    float3 indirect = (control.x & FLAG_IL) ? a.rgb * aoParams.w : 0.0;
    if (control.x & FLAG_SHOW_AO)
        return float4(ao.xxx, c.a);
    if (control.x & FLAG_SHOW_IL)
        return float4(indirect, c.a);
    if (control.x & FLAG_SHOW_SHADOW)
        return float4(s.xxx, c.a);
    // Never darker than this fraction of the original (gaps under walls or props, where AO and the
    // contact shadow add up, would otherwise turn into black lines).
    float darkening = max(ao * s, 0.5);
    return float4(c.rgb * (darkening + indirect), c.a);
}
#endif
)";

        const char* FULLSCREEN_VS = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

        constexpr uint32_t FLAG_AO = 1, FLAG_IL = 2, FLAG_SHADOW = 4, FLAG_TEMPORAL = 8, FLAG_HALF = 16, FLAG_SHOW_AO = 32, FLAG_SHOW_IL = 64,
                           FLAG_SHOW_SHADOW = 128, FLAG_HISTORY = 256, FLAG_VERTICAL = 512, FLAG_SPLIT = 1024;

        struct Constants
        {
            float proj[4];
            float projZ[4];
            float depthParams[4];
            float sizes[4];
            float aoParams[4];
            float lightView[4];
            float shadowParams[4];
            uint32_t control[4];
        };
        static_assert(sizeof(Constants) == 128);
        constexpr uint32_t CONSTANT_DWORDS = sizeof(Constants) / 4;

        constexpr uint32_t FRAME_SLOTS = 3;  // frames in flight (the renderer keeps at most 2 queued)
        constexpr uint32_t TABLE = 8;        // SRVs per pass
        constexpr uint32_t PASSES = 6;       // prep, gather, temporal, blur x2, composite
        constexpr uint32_t QUERY_SLOTS = 4;

        struct Target
        {
            ComPtr<ID3D12Resource> resource;
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        };

        // Tunables (world units: the village's camera has near 0.5, far 1000; Crash is ~1.2 tall).
        // CRASH_LIGHTING_TUNE="radius=1.2;thickness=0.5;fade=90;shadow_length=0.8;..." overrides them.
        struct Tuning
        {
            float radius = 1.6f;          // AO / indirect light reach
            float thickness = 0.5f;       // assumed thickness of what the depth buffer shows
            float aoLow = 0.9f, aoHigh = 1.0f;  // AO strength per quality
            float gi = 3.0f;              // indirect light at 100%
            float shadowLength = 1.2f;    // contact shadow ray (upwards)
            float shadowThickness = 0.35f;
            float shadowStrength = 0.5f;
            float fade = 90.0f;           // everything fades out from 0.6x to 1x this view distance
            float sky = 0.9f;             // fraction of the far plane counted as sky
        } g_tuning;

        bool g_ready = false;
        ID3D12Device* g_device = nullptr;
        CompileFunction g_compile;
        ComPtr<ID3D12RootSignature> g_rootSignature;
        ComPtr<ID3DBlob> g_vs;
        ComPtr<ID3D12PipelineState> g_prepPipeline, g_gatherPipeline, g_temporalPipeline, g_blurPipeline, g_compositePipeline;
        DXGI_FORMAT g_compositeFormat = DXGI_FORMAT_UNKNOWN;
        ComPtr<ID3D12DescriptorHeap> g_srvHeap, g_rtvHeap;
        uint32_t g_srvSize = 0, g_rtvSize = 0;

        // AO resolution targets.
        Target g_viewZ, g_normal, g_gatherA, g_gatherS, g_blurA, g_blurS;
        Target g_historyA[2], g_historyS[2], g_historyZ[2];
        ComPtr<ID3D12Resource> g_colorCopy;
        D3D12_RESOURCE_STATES g_colorCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
        uint32_t g_aoWidth = 0, g_aoHeight = 0, g_sceneWidth = 0, g_sceneHeight = 0;
        DXGI_FORMAT g_colorFormat = DXGI_FORMAT_UNKNOWN;
        std::deque<std::pair<uint64_t, ComPtr<ID3D12Resource>>> g_retired;
        uint32_t g_historyIndex = 0;
        uint64_t g_lastFrame = UINT64_MAX;
        bool g_historyValid = false;
        uint32_t g_lastFlags = 0;

        // GPU timing.
        ComPtr<ID3D12QueryHeap> g_queryHeap;
        ComPtr<ID3D12Resource> g_queryReadback;
        uint64_t g_timestampFrequency = 0;
        uint64_t g_querySlotFrame[QUERY_SLOTS] = { UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX };
        double g_gpuMs = 0.0;
        uint32_t g_gpuFrames = 0;

        void ParseTuning()
        {
            const char* text = getenv("CRASH_LIGHTING_TUNE");
            if (!text)
                return;
            std::string all = text;
            size_t at = 0;
            while (at < all.size())
            {
                size_t end = all.find_first_of(";,", at);
                std::string item = all.substr(at, end == std::string::npos ? std::string::npos : end - at);
                at = end == std::string::npos ? all.size() : end + 1;
                size_t equals = item.find('=');
                if (equals == std::string::npos)
                    continue;
                std::string key = item.substr(0, equals);
                float value = float(atof(item.c_str() + equals + 1));
                struct { const char* name; float* field; } fields[] = {
                    { "radius", &g_tuning.radius }, { "thickness", &g_tuning.thickness }, { "ao_low", &g_tuning.aoLow },
                    { "ao_high", &g_tuning.aoHigh }, { "gi", &g_tuning.gi }, { "shadow_length", &g_tuning.shadowLength },
                    { "shadow_thickness", &g_tuning.shadowThickness }, { "shadow", &g_tuning.shadowStrength }, { "fade", &g_tuning.fade },
                    { "sky", &g_tuning.sky },
                };
                bool known = false;
                for (auto& field : fields)
                {
                    if (key == field.name)
                    {
                        *field.field = value;
                        known = true;
                    }
                }
                Log("[lighting] tune %s = %g%s", key.c_str(), value, known ? "" : " (unknown, ignored)");
            }
        }

        void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES after)
        {
            if (state == after)
                return;
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = state;
            barrier.Transition.StateAfter = after;
            list->ResourceBarrier(1, &barrier);
            state = after;
        }

        constexpr D3D12_RESOURCE_STATES SHADER_READ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        void Read(ID3D12GraphicsCommandList* list, Target& target) { Transition(list, target.resource.Get(), target.state, SHADER_READ); }
        void Write(ID3D12GraphicsCommandList* list, Target& target) { Transition(list, target.resource.Get(), target.state, D3D12_RESOURCE_STATE_RENDER_TARGET); }
        void Read(ID3D12GraphicsCommandList* list, const Texture& texture)
        {
            if (texture.resource && (*texture.state & SHADER_READ) != SHADER_READ)
                Transition(list, texture.resource, *texture.state, SHADER_READ);
        }

        ComPtr<ID3D12PipelineState> CreatePipeline(const char* pass, std::initializer_list<DXGI_FORMAT> formats)
        {
            std::string source = std::string("#define ") + pass + " 1\n" + LIGHTING_HLSL;
            ComPtr<ID3DBlob> ps = g_compile(source, "ps_5_1", (std::string("lighting_") + pass).c_str());
            if (!ps)
                return nullptr;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = g_rootSignature.Get();
            desc.VS = { g_vs->GetBufferPointer(), g_vs->GetBufferSize() };
            desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = UINT(formats.size());
            uint32_t i = 0;
            for (DXGI_FORMAT format : formats)
            {
                desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                desc.RTVFormats[i++] = format;
            }
            desc.SampleDesc.Count = 1;
            ComPtr<ID3D12PipelineState> pipeline;
            if (FAILED(g_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))))
                return nullptr;
            return pipeline;
        }

        constexpr DXGI_FORMAT FORMAT_Z = DXGI_FORMAT_R32_FLOAT, FORMAT_N = DXGI_FORMAT_R8G8B8A8_SNORM, FORMAT_A = DXGI_FORMAT_R16G16B16A16_FLOAT,
                              FORMAT_S = DXGI_FORMAT_R8_UNORM;

        bool CreateTarget(Target& target, uint32_t index, uint32_t width, uint32_t height, DXGI_FORMAT format, uint64_t frame)
        {
            if (target.resource)
                g_retired.push_back({ frame, target.resource });
            target = Target{};
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_CLEAR_VALUE clear{};
            clear.Format = format;
            if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, target.state, &clear, IID_PPV_ARGS(&target.resource))))
                return false;
            target.rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            target.rtv.ptr += size_t(index) * g_rtvSize;
            g_device->CreateRenderTargetView(target.resource.Get(), nullptr, target.rtv);
            return true;
        }

        bool EnsureTargets(ID3D12Resource* color, uint32_t aoWidth, uint32_t aoHeight, uint32_t sceneWidth, uint32_t sceneHeight, uint64_t frame)
        {
            while (!g_retired.empty() && g_retired.front().first + FRAME_SLOTS <= frame)
                g_retired.pop_front();
            D3D12_RESOURCE_DESC colorDesc = color->GetDesc();
            if (g_viewZ.resource && g_aoWidth == aoWidth && g_aoHeight == aoHeight && g_sceneWidth == sceneWidth && g_sceneHeight == sceneHeight &&
                g_colorFormat == colorDesc.Format && g_colorCopy && g_colorCopy->GetDesc().Width == colorDesc.Width &&
                g_colorCopy->GetDesc().Height == colorDesc.Height)
                return true;
            uint32_t index = 0;
            bool ok = CreateTarget(g_viewZ, index++, aoWidth, aoHeight, FORMAT_Z, frame) && CreateTarget(g_normal, index++, aoWidth, aoHeight, FORMAT_N, frame) &&
                      CreateTarget(g_gatherA, index++, aoWidth, aoHeight, FORMAT_A, frame) && CreateTarget(g_gatherS, index++, aoWidth, aoHeight, FORMAT_S, frame) &&
                      CreateTarget(g_blurA, index++, aoWidth, aoHeight, FORMAT_A, frame) && CreateTarget(g_blurS, index++, aoWidth, aoHeight, FORMAT_S, frame);
            for (int i = 0; i < 2 && ok; i++)
                ok = CreateTarget(g_historyA[i], index++, aoWidth, aoHeight, FORMAT_A, frame) &&
                     CreateTarget(g_historyS[i], index++, aoWidth, aoHeight, FORMAT_S, frame) &&
                     CreateTarget(g_historyZ[i], index++, aoWidth, aoHeight, FORMAT_Z, frame);
            if (g_colorCopy)
                g_retired.push_back({ frame, g_colorCopy });
            g_colorCopy.Reset();
            if (ok)
            {
                colorDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
                D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
                g_colorCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
                ok = SUCCEEDED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &colorDesc, g_colorCopyState, nullptr, IID_PPV_ARGS(&g_colorCopy)));
            }
            if (!ok)
            {
                Log("[lighting] could not create the %ux%u targets", aoWidth, aoHeight);
                g_viewZ = Target{};
                return false;
            }
            g_aoWidth = aoWidth;
            g_aoHeight = aoHeight;
            g_sceneWidth = sceneWidth;
            g_sceneHeight = sceneHeight;
            g_colorFormat = colorDesc.Format;
            g_historyValid = false;
            Log("[lighting] targets: scene %ux%u, AO %ux%u, colour format %d", sceneWidth, sceneHeight, aoWidth, aoHeight, int(colorDesc.Format));
            return true;
        }

        // Rows of the view matrix from the view-projection (world -> clip, column-major): view z is
        // the clip w row, x and y the clip x and y rows without their z part (Gram-Schmidt).
        void ViewFromViewProj(const std::array<double, 16>& vp, std::array<double, 16>& view)
        {
            auto row = [&](int r) { return std::array<double, 4>{ vp[0 * 4 + r], vp[1 * 4 + r], vp[2 * 4 + r], vp[3 * 4 + r] }; };
            auto dot3 = [](const std::array<double, 4>& a, const std::array<double, 4>& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
            auto scaled = [](std::array<double, 4> a, double s) { for (double& v : a) v *= s; return a; };
            auto minus = [](std::array<double, 4> a, const std::array<double, 4>& b) { for (int i = 0; i < 4; i++) a[i] -= b[i]; return a; };
            std::array<double, 4> v2 = row(3);
            v2 = scaled(v2, 1.0 / std::max(sqrt(dot3(v2, v2)), 1e-12));
            std::array<double, 4> v0 = minus(row(0), scaled(v2, dot3(row(0), v2)));
            v0 = scaled(v0, 1.0 / std::max(sqrt(dot3(v0, v0)), 1e-12));
            std::array<double, 4> v1 = minus(minus(row(1), scaled(v2, dot3(row(1), v2))), scaled(v0, dot3(row(1), v0)));
            v1 = scaled(v1, 1.0 / std::max(sqrt(dot3(v1, v1)), 1e-12));
            view = {};
            for (int c = 0; c < 4; c++)
            {
                view[c * 4 + 0] = v0[c];
                view[c * 4 + 1] = v1[c];
                view[c * 4 + 2] = v2[c];
            }
            view[15] = 1.0;
        }

        std::array<double, 16> Multiply(const std::array<double, 16>& a, const std::array<double, 16>& b)
        {
            std::array<double, 16> r{};
            for (int c = 0; c < 4; c++)
                for (int row = 0; row < 4; row++)
                {
                    double sum = 0.0;
                    for (int k = 0; k < 4; k++)
                        sum += a[k * 4 + row] * b[c * 4 + k];
                    r[c * 4 + row] = sum;
                }
            return r;
        }

        // Inverse of a rigid transform (rotation rows + translation).
        std::array<double, 16> InvertRigid(const std::array<double, 16>& m)
        {
            std::array<double, 16> r{};
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    r[j * 4 + i] = m[i * 4 + j];
            for (int i = 0; i < 3; i++)
                r[3 * 4 + i] = -(r[0 * 4 + i] * m[3 * 4 + 0] + r[1 * 4 + i] * m[3 * 4 + 1] + r[2 * 4 + i] * m[3 * 4 + 2]);
            r[15] = 1.0;
            return r;
        }

        void BindTable(ID3D12GraphicsCommandList* list, uint32_t base, uint32_t pass, std::initializer_list<ID3D12Resource*> resources)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += size_t(base + pass * TABLE) * g_srvSize;
            uint32_t i = 0;
            for (ID3D12Resource* resource : resources)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE handle{ cpu.ptr + size_t(i) * g_srvSize };
                if (resource)
                {
                    g_device->CreateShaderResourceView(resource, nullptr, handle);
                }
                else
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC null{};
                    null.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    null.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    null.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    null.Texture2D.MipLevels = 1;
                    g_device->CreateShaderResourceView(nullptr, &null, handle);
                }
                i++;
            }
            for (; i < TABLE; i++)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC null{};
                null.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                null.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                null.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                null.Texture2D.MipLevels = 1;
                g_device->CreateShaderResourceView(nullptr, &null, D3D12_CPU_DESCRIPTOR_HANDLE{ cpu.ptr + size_t(i) * g_srvSize });
            }
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += uint64_t(base + pass * TABLE) * g_srvSize;
            list->SetGraphicsRootDescriptorTable(1, gpu);
        }

        void Draw(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline, std::initializer_list<D3D12_CPU_DESCRIPTOR_HANDLE> targets, uint32_t width,
                  uint32_t height)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4];
            uint32_t count = 0;
            for (D3D12_CPU_DESCRIPTOR_HANDLE target : targets)
                rtvs[count++] = target;
            list->OMSetRenderTargets(count, rtvs, FALSE, nullptr);
            D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
            D3D12_RECT scissor{ 0, 0, LONG(width), LONG(height) };
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->SetPipelineState(pipeline);
            list->DrawInstanced(3, 1, 0, 0);
        }

        void ReadTimings(uint32_t slot)
        {
            if (g_querySlotFrame[slot] == UINT64_MAX || !g_timestampFrequency)
                return;
            uint64_t* values = nullptr;
            D3D12_RANGE range{ size_t(slot) * 16, size_t(slot) * 16 + 16 };
            if (SUCCEEDED(g_queryReadback->Map(0, &range, reinterpret_cast<void**>(&values))))
            {
                uint64_t begin = values[slot * 2], end = values[slot * 2 + 1];
                if (end > begin)
                {
                    g_gpuMs += double(end - begin) * 1000.0 / double(g_timestampFrequency);
                    g_gpuFrames++;
                }
                D3D12_RANGE none{ 0, 0 };
                g_queryReadback->Unmap(0, &none);
            }
            g_querySlotFrame[slot] = UINT64_MAX;
        }
    }

    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, const CompileFunction& compile)
    {
        g_device = device;
        g_compile = compile;
        ParseTuning();

        // Root signature: 32 constants at b0, SRV table t0-t7, point (s0) and linear (s1) clamp.
        D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, TABLE, 0, 0, 0 };
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = { 0, 0, CONSTANT_DWORDS };
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = { 1, &range };
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        for (uint32_t i = 0; i < 2; i++)
        {
            samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
            samplers[i].ShaderRegister = i;
            samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        D3D12_ROOT_SIGNATURE_DESC rootDesc{ 2, parameters, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> blob, errors;
        if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature))))
        {
            Log("[lighting] could not create the root signature");
            return false;
        }
        g_vs = compile(FULLSCREEN_VS, "vs_5_1", "lighting_vs");
        if (!g_vs)
            return false;
        g_prepPipeline = CreatePipeline("PREP", { FORMAT_Z, FORMAT_N });
        g_gatherPipeline = CreatePipeline("GATHER", { FORMAT_A, FORMAT_S });
        g_temporalPipeline = CreatePipeline("TEMPORAL", { FORMAT_A, FORMAT_S, FORMAT_Z });
        g_blurPipeline = CreatePipeline("BLUR", { FORMAT_A, FORMAT_S });
        if (!g_prepPipeline || !g_gatherPipeline || !g_temporalPipeline || !g_blurPipeline)
        {
            Log("[lighting] could not compile the shaders");
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC srvHeap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, FRAME_SLOTS * PASSES * TABLE, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeap{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 12, D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
        if (FAILED(device->CreateDescriptorHeap(&srvHeap, IID_PPV_ARGS(&g_srvHeap))) || FAILED(device->CreateDescriptorHeap(&rtvHeap, IID_PPV_ARGS(&g_rtvHeap))))
            return false;
        g_srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g_rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        // Timestamps around the passes (TakeGpuMs).
        D3D12_QUERY_HEAP_DESC queryDesc{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, QUERY_SLOTS * 2, 0 };
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = QUERY_SLOTS * 2 * sizeof(uint64_t);
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES readback{ D3D12_HEAP_TYPE_READBACK };
        if (queue && SUCCEEDED(queue->GetTimestampFrequency(&g_timestampFrequency)) && SUCCEEDED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&g_queryHeap))))
        {
            if (FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&g_queryReadback))))
                g_queryHeap.Reset();
        }
        g_ready = true;
        Log("[lighting] ready (radius %g, thickness %g, gi %g, contact shadow %g / %g, fade %g)", g_tuning.radius, g_tuning.thickness, g_tuning.gi,
            g_tuning.shadowLength, g_tuning.shadowThickness, g_tuning.fade);
        return true;
    }

    bool Ready()
    {
        return g_ready;
    }

    namespace
    {
        std::atomic<bool> g_bypass{ false };
    }

    bool ToggleBypass()
    {
        bool on = g_bypass.load();
        g_bypass = !on;
        Log("[lighting] F8: remaster %s", on ? "on" : "off");
        return on;
    }

    bool SoftShadows()
    {
        return !g_bypass && settings::Get().softShadows;
    }

    Options CurrentOptions()
    {
        settings::Values values = settings::Get();
        Options options;
        if (g_bypass)
            return options;
        options.ao = values.ssao != settings::AoQuality::Off;
        options.gi = values.gi && values.giIntensity > 0;
        options.contactShadows = values.contactShadows;
        options.fullResolution = values.ssao == settings::AoQuality::High;
        options.aoStrength = values.ssao == settings::AoQuality::High ? g_tuning.aoHigh : values.ssao == settings::AoQuality::Low ? g_tuning.aoLow : 0.0f;
        options.giIntensity = float(values.giIntensity) / 100.0f * g_tuning.gi;
        options.slices = options.fullResolution ? 3 : 2;
        options.steps = options.fullResolution ? 8 : 6;
        return options;
    }

    bool Apply(const Frame& frame, const Options& options)
    {
        if (!g_ready || !options.Any() || !frame.color.resource || !frame.depth.resource || frame.width < 16 || frame.height < 16)
            return false;
        ID3D12GraphicsCommandList* list = frame.list;
        D3D12_RESOURCE_DESC colorDesc = frame.color.resource->GetDesc();
        if (!g_compositePipeline || g_compositeFormat != colorDesc.Format)
        {
            g_compositePipeline = CreatePipeline("COMPOSITE", { colorDesc.Format });
            g_compositeFormat = colorDesc.Format;
            if (!g_compositePipeline)
            {
                Log("[lighting] could not create the composite pipeline for format %d", int(colorDesc.Format));
                g_ready = false;
                return false;
            }
        }

        // Camera: view matrix and projection from the view-projection.
        std::array<double, 16> view, projection;
        ViewFromViewProj(frame.viewProj, view);
        projection = Multiply(frame.viewProj, InvertRigid(view));
        auto P = [&](int r, int c) { return projection[c * 4 + r]; };
        static uint32_t applied = 0;
        if (applied++ % 900 < 2)
        {
            Log("[lighting] frame %llu projection:", (unsigned long long)frame.frame);
            Log("[lighting]   projection: P00 %.4f P02 %.4f P11 %.4f P12 %.4f P22 %.4f P23 %.4f P32 %.4f P33 %.4f (ignored terms: %.2g %.2g %.2g %.2g %.2g %.2g)",
                P(0, 0), P(0, 2), P(1, 1), P(1, 2), P(2, 2), P(2, 3), P(3, 2), P(3, 3), P(0, 1), P(0, 3), P(1, 0), P(1, 3), P(3, 0), P(3, 1));
        }
        // Only perspective cameras (a 2D / orthographic "scene" has no depth to work with).
        if (!(P(3, 2) > 1e-6) || !(P(0, 0) > 0.0) || !(P(1, 1) > 0.0))
            return false;
        // HIGH is full resolution up to 1440 lines; above that (4K) the AO is computed at half
        // resolution, like LOW but with more samples: full resolution at 4K costs ~3.8 ms of GPU
        // (RTX 5080), half ~1 ms, and the bilateral upsampling keeps the edges.
        bool half = !options.fullResolution || frame.height > 1440;
        uint32_t aoWidth = half ? (frame.width + 1) / 2 : frame.width, aoHeight = half ? (frame.height + 1) / 2 : frame.height;
        if (!EnsureTargets(frame.color.resource, aoWidth, aoHeight, frame.width, frame.height, frame.frame))
            return false;
        Constants constants{};
        constants.proj[0] = float(P(0, 0));
        constants.proj[1] = float(P(0, 2));
        constants.proj[2] = float(P(1, 1));
        constants.proj[3] = float(P(1, 2));
        constants.projZ[0] = float(P(2, 2));
        constants.projZ[1] = float(P(2, 3));
        constants.projZ[2] = float(P(3, 2));
        constants.projZ[3] = float(P(3, 3));
        // Far plane: the view z of window depth at ndc z = 1.
        double farNdc = 1.0;
        double farZ = (P(2, 3) - farNdc * P(3, 3)) / (farNdc * P(3, 2) - P(2, 2));
        if (!(farZ > 1.0 && farZ < 1e7))
            farZ = 1000.0;
        constants.depthParams[0] = fabsf(frame.zScale) > 1e-6f ? frame.zScale : 1.0f;
        constants.depthParams[1] = frame.zOffset;
        constants.depthParams[2] = g_tuning.fade * 0.6f;
        constants.depthParams[3] = float(farZ * g_tuning.sky);
        constants.sizes[0] = float(aoWidth);
        constants.sizes[1] = float(aoHeight);
        constants.sizes[2] = float(frame.width);
        constants.sizes[3] = float(frame.height);
        constants.aoParams[0] = g_tuning.radius;
        constants.aoParams[1] = g_tuning.thickness;
        constants.aoParams[2] = options.aoStrength;
        constants.aoParams[3] = options.giIntensity;
        if (options.contactShadows)
        {
            // Upwards (world +y), in view space.
            double towards[3] = { 0.0, 1.0, 0.0 };
            double v[3];
            for (int r = 0; r < 3; r++)
                v[r] = view[0 * 4 + r] * towards[0] + view[1 * 4 + r] * towards[1] + view[2 * 4 + r] * towards[2];
            double length = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (length > 1e-6)
            {
                for (int i = 0; i < 3; i++)
                    constants.lightView[i] = float(v[i] / length);
                constants.lightView[3] = g_tuning.shadowStrength;
            }
        }
        constants.shadowParams[0] = g_tuning.shadowLength;
        constants.shadowParams[1] = g_tuning.shadowThickness;
        constants.shadowParams[2] = g_tuning.fade;

        static const bool noTemporal = getenv("CRASH_LIGHTING_NO_TEMPORAL") != nullptr;  // debug: no accumulation
        static const uint32_t show = [] {
            const char* text = getenv("CRASH_LIGHTING_SHOW");  // debug view: ao | gi | shadow | split (left half original)
            if (!text)
                return 0u;
            std::string value = text;
            return value == "ao" ? FLAG_SHOW_AO : value == "gi" ? FLAG_SHOW_IL : value == "shadow" ? FLAG_SHOW_SHADOW : value == "split" ? FLAG_SPLIT : 0u;
        }();
        uint32_t flags = (options.ao ? FLAG_AO : 0) | (options.gi ? FLAG_IL : 0) | (options.contactShadows ? FLAG_SHADOW : 0) | (half ? FLAG_HALF : 0) | show;
        bool temporal = !noTemporal && frame.motion.resource;
        if (temporal)
            flags |= FLAG_TEMPORAL;
        // History: last frame drew the same passes at the same size, and no camera cut.
        uint32_t passFlags = flags & (FLAG_AO | FLAG_IL | FLAG_SHADOW | FLAG_HALF);
        bool history = temporal && g_historyValid && !frame.reset && g_lastFrame + 1 == frame.frame && g_lastFlags == passFlags;
        g_lastFrame = frame.frame;
        g_lastFlags = passFlags;
        constants.control[1] = uint32_t(frame.frame);
        constants.control[2] = options.slices;
        constants.control[3] = options.steps;

        uint32_t querySlot = uint32_t(frame.frame % QUERY_SLOTS);
        if (g_queryHeap)
        {
            ReadTimings(querySlot);
            list->EndQuery(g_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, querySlot * 2);
        }

        uint32_t base = uint32_t(frame.frame % FRAME_SLOTS) * PASSES * TABLE;
        ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(g_rootSignature.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        auto setConstants = [&](uint32_t extraFlags) {
            constants.control[0] = flags | extraFlags;
            list->SetGraphicsRoot32BitConstants(0, CONSTANT_DWORDS, &constants, 0);
        };

        // Copy of the scene colour (the composite writes the original).
        Transition(list, frame.color.resource, *frame.color.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(list, g_colorCopy.Get(), g_colorCopyState, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(g_colorCopy.Get(), frame.color.resource);
        Transition(list, g_colorCopy.Get(), g_colorCopyState, SHADER_READ);
        Transition(list, frame.color.resource, *frame.color.state, SHADER_READ);
        Read(list, frame.depth);
        if (temporal)
            Read(list, frame.motion);

        // Prep.
        Write(list, g_viewZ);
        Write(list, g_normal);
        setConstants(0);
        BindTable(list, base, 0, { frame.depth.resource });
        Draw(list, g_prepPipeline.Get(), { g_viewZ.rtv, g_normal.rtv }, aoWidth, aoHeight);
        Read(list, g_viewZ);
        Read(list, g_normal);

        // Gather.
        Write(list, g_gatherA);
        Write(list, g_gatherS);
        BindTable(list, base, 1, { g_viewZ.resource.Get(), g_normal.resource.Get(), frame.color.resource, frame.depth.resource });
        Draw(list, g_gatherPipeline.Get(), { g_gatherA.rtv, g_gatherS.rtv }, aoWidth, aoHeight);
        Read(list, g_gatherA);
        Read(list, g_gatherS);

        // Temporal: into this frame's history targets (the other ones hold last frame's).
        uint32_t current = g_historyIndex ^ 1u, previous = g_historyIndex;
        Write(list, g_historyA[current]);
        Write(list, g_historyS[current]);
        Write(list, g_historyZ[current]);
        Read(list, g_historyA[previous]);
        Read(list, g_historyS[previous]);
        Read(list, g_historyZ[previous]);
        setConstants(history ? FLAG_HISTORY : 0);
        BindTable(list, base, 2,
                  { g_gatherA.resource.Get(), g_gatherS.resource.Get(), g_historyA[previous].resource.Get(), g_historyS[previous].resource.Get(),
                    g_historyZ[previous].resource.Get(), temporal ? frame.motion.resource : nullptr, g_viewZ.resource.Get() });
        Draw(list, g_temporalPipeline.Get(), { g_historyA[current].rtv, g_historyS[current].rtv, g_historyZ[current].rtv }, aoWidth, aoHeight);
        Read(list, g_historyA[current]);
        Read(list, g_historyS[current]);
        g_historyIndex = current;
        g_historyValid = temporal;

        // Blur: history -> blur (horizontal) -> gather (vertical).
        Write(list, g_blurA);
        Write(list, g_blurS);
        setConstants(0);
        BindTable(list, base, 3, { g_historyA[current].resource.Get(), g_historyS[current].resource.Get(), g_viewZ.resource.Get(), g_normal.resource.Get() });
        Draw(list, g_blurPipeline.Get(), { g_blurA.rtv, g_blurS.rtv }, aoWidth, aoHeight);
        Read(list, g_blurA);
        Read(list, g_blurS);
        Write(list, g_gatherA);
        Write(list, g_gatherS);
        setConstants(FLAG_VERTICAL);
        BindTable(list, base, 4, { g_blurA.resource.Get(), g_blurS.resource.Get(), g_viewZ.resource.Get(), g_normal.resource.Get() });
        Draw(list, g_blurPipeline.Get(), { g_gatherA.rtv, g_gatherS.rtv }, aoWidth, aoHeight);
        Read(list, g_gatherA);
        Read(list, g_gatherS);

        // Composite into the scene colour.
        Transition(list, frame.color.resource, *frame.color.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        setConstants(0);
        BindTable(list, base, 5, { g_colorCopy.Get(), frame.depth.resource, g_gatherA.resource.Get(), g_gatherS.resource.Get(), g_viewZ.resource.Get() });
        Draw(list, g_compositePipeline.Get(), { frame.colorRtv }, frame.width, frame.height);

        if (g_queryHeap)
        {
            list->EndQuery(g_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, querySlot * 2 + 1);
            list->ResolveQueryData(g_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, querySlot * 2, 2, g_queryReadback.Get(), uint64_t(querySlot) * 16);
            g_querySlotFrame[querySlot] = frame.frame;
        }
        return true;
    }

    double TakeGpuMs(uint32_t& frames)
    {
        frames = g_gpuFrames;
        double average = g_gpuFrames ? g_gpuMs / g_gpuFrames : 0.0;
        g_gpuMs = 0.0;
        g_gpuFrames = 0;
        return average;
    }
}
