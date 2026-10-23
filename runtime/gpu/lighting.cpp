#include "lighting.h"
#include "../diagnostics.h"
#include "../settings/settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

namespace gpu::lighting
{
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

        constexpr uint32_t TABLE = 8;        // textures per pass
        constexpr uint32_t QUERY_SLOTS = 4;  // timestamp pairs (device timestamps 0..7)

        using Target = rhi::Texture*;

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
        rhi::Device* g_device = nullptr;
        rhi::Pipeline* g_prepPipeline = nullptr;
        rhi::Pipeline* g_gatherPipeline = nullptr;
        rhi::Pipeline* g_temporalPipeline = nullptr;
        rhi::Pipeline* g_blurPipeline = nullptr;
        rhi::Pipeline* g_compositePipeline = nullptr;
        DXGI_FORMAT g_compositeFormat = DXGI_FORMAT_UNKNOWN;

        // AO resolution targets.
        Target g_viewZ, g_normal, g_gatherA, g_gatherS, g_blurA, g_blurS;
        Target g_historyA[2], g_historyS[2], g_historyZ[2];
        Target g_colorCopy;
        uint32_t g_aoWidth = 0, g_aoHeight = 0, g_sceneWidth = 0, g_sceneHeight = 0;
        DXGI_FORMAT g_colorFormat = DXGI_FORMAT_UNKNOWN;
        uint32_t g_historyIndex = 0;
        uint64_t g_lastFrame = UINT64_MAX;
        bool g_historyValid = false;
        uint32_t g_lastFlags = 0;

        // GPU timing (timestamps 0..QUERY_SLOTS*2 of the device).
        bool g_timestamps = false;
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

        rhi::Pipeline* CreatePipeline(const char* pass, std::initializer_list<DXGI_FORMAT> formats)
        {
            rhi::PassPipelineDesc desc;
            desc.name = std::string("lighting_") + pass;
            desc.vs = FULLSCREEN_VS;
            desc.ps = std::string("#define ") + pass + " 1\n" + LIGHTING_HLSL;
            desc.targetCount = 0;
            for (DXGI_FORMAT format : formats)
                desc.targets[desc.targetCount++] = format;
            desc.textureCount = TABLE;
            desc.constantBytes = sizeof(Constants);
            // Point (s0) and linear (s1) clamp.
            desc.samplers[1].minFilter = desc.samplers[1].magFilter = rhi::Filter::Linear;
            desc.samplerCount = 2;
            return g_device->CreatePassPipeline(desc);
        }

        constexpr DXGI_FORMAT FORMAT_Z = DXGI_FORMAT_R32_FLOAT, FORMAT_N = DXGI_FORMAT_R8G8B8A8_SNORM, FORMAT_A = DXGI_FORMAT_R16G16B16A16_FLOAT,
                              FORMAT_S = DXGI_FORMAT_R8_UNORM;

        bool CreateTarget(Target& target, uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t usage = rhi::USAGE_SAMPLED | rhi::USAGE_RENDER_TARGET)
        {
            if (target)
                g_device->DestroyTexture(target);
            rhi::TextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.format = format;
            desc.usage = usage;
            desc.name = "lighting target";
            target = g_device->CreateTexture(desc);
            return target != nullptr;
        }

        bool EnsureTargets(rhi::Texture* color, uint32_t aoWidth, uint32_t aoHeight, uint32_t sceneWidth, uint32_t sceneHeight)
        {
            const rhi::TextureDesc& colorDesc = color->desc;
            if (g_viewZ && g_aoWidth == aoWidth && g_aoHeight == aoHeight && g_sceneWidth == sceneWidth && g_sceneHeight == sceneHeight &&
                g_colorFormat == colorDesc.format && g_colorCopy && g_colorCopy->desc.width == colorDesc.width && g_colorCopy->desc.height == colorDesc.height)
                return true;
            bool ok = CreateTarget(g_viewZ, aoWidth, aoHeight, FORMAT_Z) && CreateTarget(g_normal, aoWidth, aoHeight, FORMAT_N) &&
                      CreateTarget(g_gatherA, aoWidth, aoHeight, FORMAT_A) && CreateTarget(g_gatherS, aoWidth, aoHeight, FORMAT_S) &&
                      CreateTarget(g_blurA, aoWidth, aoHeight, FORMAT_A) && CreateTarget(g_blurS, aoWidth, aoHeight, FORMAT_S);
            for (int i = 0; i < 2 && ok; i++)
                ok = CreateTarget(g_historyA[i], aoWidth, aoHeight, FORMAT_A) && CreateTarget(g_historyS[i], aoWidth, aoHeight, FORMAT_S) &&
                     CreateTarget(g_historyZ[i], aoWidth, aoHeight, FORMAT_Z);
            if (ok)
                ok = CreateTarget(g_colorCopy, colorDesc.width, colorDesc.height, colorDesc.format, rhi::USAGE_SAMPLED);
            if (!ok)
            {
                Log("[lighting] could not create the %ux%u targets", aoWidth, aoHeight);
                return false;
            }
            g_aoWidth = aoWidth;
            g_aoHeight = aoHeight;
            g_sceneWidth = sceneWidth;
            g_sceneHeight = sceneHeight;
            g_colorFormat = colorDesc.format;
            g_historyValid = false;
            Log("[lighting] targets: scene %ux%u, AO %ux%u, colour format %d", sceneWidth, sceneHeight, aoWidth, aoHeight, int(colorDesc.format));
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

        void Draw(rhi::Pipeline* pipeline, std::initializer_list<rhi::Texture*> targets, uint32_t width, uint32_t height,
                  std::initializer_list<rhi::Texture*> textures, const Constants& constants)
        {
            rhi::PassDraw pass;
            pass.pipeline = pipeline;
            pass.targetCount = 0;
            for (rhi::Texture* target : targets)
                pass.targets[pass.targetCount++] = target;
            pass.viewport = { 0.0f, 0.0f, float(width), float(height) };
            pass.scissor = { 0, 0, int32_t(width), int32_t(height) };
            pass.textureCount = TABLE;
            uint32_t i = 0;
            for (rhi::Texture* texture : textures)
                pass.textures[i++] = { texture };
            pass.constants = &constants;
            pass.constantBytes = sizeof(constants);
            g_device->DrawPass(pass);
        }

        void ReadTimings(uint32_t slot)
        {
            if (g_querySlotFrame[slot] == UINT64_MAX || !g_timestamps)
                return;
            uint64_t values[2];
            if (g_device->ReadTimestamps(slot * 2, 2, values) && values[1] > values[0])
            {
                g_gpuMs += double(values[1] - values[0]) * 1000.0 / g_device->TimestampFrequency();
                g_gpuFrames++;
            }
            g_querySlotFrame[slot] = UINT64_MAX;
        }
    }

    bool Init(rhi::Device* device)
    {
        g_device = device;
        ParseTuning();
        g_prepPipeline = CreatePipeline("PREP", { FORMAT_Z, FORMAT_N });
        g_gatherPipeline = CreatePipeline("GATHER", { FORMAT_A, FORMAT_S });
        g_temporalPipeline = CreatePipeline("TEMPORAL", { FORMAT_A, FORMAT_S, FORMAT_Z });
        g_blurPipeline = CreatePipeline("BLUR", { FORMAT_A, FORMAT_S });
        if (!g_prepPipeline || !g_gatherPipeline || !g_temporalPipeline || !g_blurPipeline)
        {
            Log("[lighting] could not compile the shaders");
            return false;
        }
        g_timestamps = device->TimestampFrequency() > 0.0;
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
        if (!g_ready || !options.Any() || !frame.color || !frame.depth || frame.width < 16 || frame.height < 16)
            return false;
        DXGI_FORMAT colorFormat = frame.color->desc.RenderFormat();
        if (!g_compositePipeline || g_compositeFormat != colorFormat)
        {
            g_compositePipeline = CreatePipeline("COMPOSITE", { colorFormat });
            g_compositeFormat = colorFormat;
            if (!g_compositePipeline)
            {
                Log("[lighting] could not create the composite pipeline for format %d", int(colorFormat));
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
        if (!EnsureTargets(frame.color, aoWidth, aoHeight, frame.width, frame.height))
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
        bool temporal = !noTemporal && frame.motion;
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
        if (g_timestamps)
        {
            ReadTimings(querySlot);
            g_device->WriteTimestamp(querySlot * 2);
        }
        constants.control[0] = flags;

        // Copy of the scene colour (the composite writes the original).
        g_device->CopyTexture(g_colorCopy, frame.color, nullptr);

        // Prep.
        Draw(g_prepPipeline, { g_viewZ, g_normal }, aoWidth, aoHeight, { frame.depth }, constants);

        // Gather.
        Draw(g_gatherPipeline, { g_gatherA, g_gatherS }, aoWidth, aoHeight, { g_viewZ, g_normal, frame.color, frame.depth }, constants);

        // Temporal: into this frame's history targets (the other ones hold last frame's).
        uint32_t current = g_historyIndex ^ 1u, previous = g_historyIndex;
        constants.control[0] = flags | (history ? FLAG_HISTORY : 0);
        Draw(g_temporalPipeline, { g_historyA[current], g_historyS[current], g_historyZ[current] }, aoWidth, aoHeight,
             { g_gatherA, g_gatherS, g_historyA[previous], g_historyS[previous], g_historyZ[previous], temporal ? frame.motion : nullptr, g_viewZ },
             constants);
        g_historyIndex = current;
        g_historyValid = temporal;

        // Blur: history -> blur (horizontal) -> gather (vertical).
        constants.control[0] = flags;
        Draw(g_blurPipeline, { g_blurA, g_blurS }, aoWidth, aoHeight, { g_historyA[current], g_historyS[current], g_viewZ, g_normal }, constants);
        constants.control[0] = flags | FLAG_VERTICAL;
        Draw(g_blurPipeline, { g_gatherA, g_gatherS }, aoWidth, aoHeight, { g_blurA, g_blurS, g_viewZ, g_normal }, constants);

        // Composite into the scene colour.
        constants.control[0] = flags;
        Draw(g_compositePipeline, { frame.color }, frame.width, frame.height, { g_colorCopy, frame.depth, g_gatherA, g_gatherS, g_viewZ }, constants);

        if (g_timestamps)
        {
            g_device->WriteTimestamp(querySlot * 2 + 1);
            g_device->ResolveTimestamps(querySlot * 2, 2);
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
