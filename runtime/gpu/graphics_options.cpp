#include "graphics_options.h"
#include "../diagnostics.h"

#include <regex>

namespace gpu::graphics
{
    namespace
    {
        constexpr uint64_t MOTION_BLUR_PS = 0x1577D3C304364818ull;
        constexpr uint64_t DEPTH_OF_FIELD_PS = 0x45775928E9EBD556ull;
        constexpr uint64_t SHADOW_PS = 0xFDF408078EA760BBull;

        // Inserts `code` before the generated entry point body.
        bool InsertHelpers(std::string& hlsl, const char* code)
        {
            size_t at = hlsl.find("void xe_main()");
            if (at == std::string::npos)
                return false;
            hlsl.insert(at, code);
            return true;
        }

        // Replaces every reading of float constant `index` (xe_c[index].swizzle) by `replacement`
        // followed by the swizzle. Returns how many were replaced.
        int ReplaceConstant(std::string& hlsl, int index, const std::string& replacement)
        {
            std::string pattern = "xe_c[" + std::to_string(index) + "].";
            int count = 0;
            for (size_t at = hlsl.find(pattern); at != std::string::npos; at = hlsl.find(pattern, at + replacement.size() + 1))
            {
                hlsl.replace(at, pattern.size() - 1, replacement);
                count++;
            }
            return count;
        }

        // motionblurfp.hlsl: the velocity maps hold 0.5 + v / 2 (8 bits, 0x80 = still); the pass
        // blurs along max(current, previous) and, for still pixels, along Params.zw.
        bool PatchMotionBlur(std::string& hlsl)
        {
            // The two velocity fetches (fetch constants 2 and 3): scale around the neutral value.
            std::regex fetch(R"((float4 t = xe_swizzle\(xe_t([23])\.[^;]*;))");
            std::string patched = std::regex_replace(hlsl, fetch, "$1 t.xy = xe_mb_velocity(t.xy);");
            if (patched == hlsl)
                return false;
            hlsl = std::move(patched);
            if (!ReplaceConstant(hlsl, 0, "xe_mb_params()"))
                return false;
            return InsertHelpers(hlsl, R"(// Graphics option: motion blur intensity (xe_effect.x).
float2 xe_mb_velocity(float2 v) { const float neutral = 128.0 / 255.0; return neutral + (v - neutral) * xe_effect.x; }
float4 xe_mb_params() { return float4(xe_c[0].xy, xe_c[0].zw * xe_effect.x); }

)");
        }

        // DOF.hlsl: tap radius = |(blur * 0.5 + 0.5) * MaxRadius.y - MaxRadius.x|.
        bool PatchDepthOfField(std::string& hlsl)
        {
            return ReplaceConstant(hlsl, 5, "(xe_c[5] * xe_effect.y)") > 0;
        }

        // shadowshaderfp: 9 taps at uv + c2..c10 (UV offsets, about 1.5 pixels at 720p) of the
        // stencil (z after the fetch swizzle; > 0 = in shadow); colour = 1 - sum over the shadowed
        // taps of (1 - r1) * weight (c30..c38). Soft shadows (xe_jitter.z, lighting remaster): the
        // same sum over a rotated 16-tap disk 1.35x wider (each tap, like the original's filtered
        // fetch, in shadow if any of its 4 texels is).
        bool PatchShadow(std::string& hlsl)
        {
            int count = 0;
            for (int c = 2; c <= 10; c++)
                count += ReplaceConstant(hlsl, c, "(xe_c[" + std::to_string(c) + "] * xe_effect.z)") > 0;
            if (count != 9)
                return false;
            const std::string entry = "void xe_main()\n{\n";
            size_t at = hlsl.find(entry);
            if (at == std::string::npos)
                return false;
            hlsl.insert(at + entry.size(), "    if (xe_jitter.z > 0.5) { xe_soft_shadow(); return; }\n");
            return InsertHelpers(hlsl, R"(// Graphics option: soft shadows (xe_jitter.z).
float xe_shadow_tap(float2 uv, uint channel)
{
    float4 g = channel == 0u ? xe_t0.GatherRed(xe_s0, uv) : channel == 1u ? xe_t0.GatherGreen(xe_s0, uv)
             : channel == 2u ? xe_t0.GatherBlue(xe_s0, uv) : xe_t0.GatherAlpha(xe_s0, uv);
    // As the original's filtered fetch: in shadow if any of the 4 texels is.
    return any(g > 0.0) ? 1.0 : 0.0;
}
void xe_soft_shadow()
{
    float2 uv = r[0].xy;
    float2 size; xe_t0.GetDimensions(size.x, size.y);
    uint channel = (xe_tex_swizzles[0].x >> 6) & 7u;
    float2 extent = 0.0;
    float4 weight = 0.0;
    [unroll] for (int i = 0; i < 9; i++)
    {
        extent = max(extent, abs(xe_c[2 + i].xy));
        weight += xe_c[30 + i];
    }
    extent *= xe_effect.z * 1.35;
    float shadowed;
    if (channel >= 4u)
        shadowed = channel == 5u ? 1.0 : 0.0;
    else
    {
        float2 pixel = uv * size;
        float angle = 6.2831853 * frac(52.9829189 * frac(dot(floor(pixel), float2(0.06711056, 0.00583715))));
        shadowed = 0.0;
        [unroll] for (int k = 0; k < 16; k++)
        {
            float radius = sqrt((float(k) + 0.5) / 16.0);
            float theta = float(k) * 2.3999632 + angle;
            shadowed += xe_shadow_tap(uv + radius * float2(cos(theta), sin(theta)) * extent, channel);
        }
        shadowed /= 16.0;
    }
    xe_oC[0] = xe_c[255].x - shadowed * (xe_c[255].x - r[1]) * weight;
}

)");
        }
    }

    const char* PatchShader(ShaderStage stage, uint64_t hash, std::string& hlsl)
    {
        if (stage != ShaderStage::Pixel)
            return nullptr;
        std::string copy = hlsl;
        bool ok;
        const char* name;
        switch (hash)
        {
        case MOTION_BLUR_PS: name = "motion blur intensity"; ok = PatchMotionBlur(copy); break;
        case DEPTH_OF_FIELD_PS: name = "depth of field intensity"; ok = PatchDepthOfField(copy); break;
        case SHADOW_PS: name = "shadow softness"; ok = PatchShadow(copy); break;
        default: return nullptr;
        }
        if (!ok)
        {
            Log("[gpu] graphics option \"%s\": shader %016llx is not as expected, left unchanged", name, (unsigned long long)hash);
            return nullptr;
        }
        hlsl = std::move(copy);
        return name;
    }
}
