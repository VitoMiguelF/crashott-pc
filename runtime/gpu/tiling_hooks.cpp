// Predicated tiling of the scene, and the single-pass scene (docs/internals/native-gpu-plan.md, M2).
//
// Pure3D's xnContext renders the 2x MSAA scene in 3 screen tiles (448 px wide, overlapping 32):
// xnDisplay::vf04 (0x8238F5B0) creates the scene's color targets at tile size (480x720, from
// align32(1280 / 3 + 63)) through 0x8226A868; xnContext::vf04 (0x8238CD00, start of frame) calls
// BeginTiling and sets xnContext+1220 (tiling on); xnContext::vf05 (0x8238E068, end of frame) and the
// mid-frame resolve before refraction (0x8238CE98) call EndTiling. While tiling is on, the xn
// helpers resolve once per tile rect (xnContext+1116, 3 D3DRECTs) and D3D's kick (0x8226CE40) does not
// hand the segments to the GPU: it appends them to a list (device+13224) that EndTiling replays once
// per tile, so the command processor only sees the scene at the end of the frame and parses it three
// times, while the game thread waits for it (runtime/NOTES.md, "GPU nativa").
//
// The single pass (on unless CRASH_SINGLE_PASS=0): the scene targets are created full size, with
// explicit EDRAM bases (three 1280x720 2x surfaces don't fit D3D's 2048-tile allocator); BeginTiling
// is skipped, so D3D kicks the scene's segments as it builds them and the command processor works
// while the game thread does; an EndTiling becomes one Resolve of the whole surface with its flags
// and clear values; of the xn helpers' per-tile resolves, the first becomes a whole-surface resolve
// and the other two are dropped. xnContext+1220 stays set: the xn code's non-tiled branches read
// texture slots that are never created (and mix up indices), they are dead code on this title.
// Our render targets are host images, so the EDRAM size that made tiling necessary on the console
// doesn't apply. Measured in the village (desempenho_vila_direto): 171 -> 421 fps, command
// processor 3.5 -> 1.45 ms per frame, 1,585 -> 574 draws per frame.
//
// D3D signatures (Xbox 360 SDK; a float argument also takes a GPR slot; arguments 9+ are in the
// caller's parameter area, r1 + 0x50 + 8 * (n - 9), a 32-bit value in the low word):
//   BeginTiling(device r3, Flags r4, Count r5, const D3DRECT* pTileRects r6,
//               const D3DVECTOR4* pClearColor r7, float ClearZ f1, DWORD ClearStencil r9)
//   EndTiling(device r3, ResolveFlags r4, const D3DRECT* pResolveRects r5, D3DBaseTexture* pDest r6,
//             const D3DVECTOR4* pClearColor r7, float ClearZ f1, DWORD ClearStencil r9,
//             const D3DRESOLVE_PARAMETERS* pParameters r10)
//   Resolve(device r3, Flags r4, const D3DRECT* pSourceRect r5, D3DBaseTexture* pDest r6,
//           const D3DPOINT* pDestPoint r7, UINT DestLevel r8, UINT DestSliceOrFace r9,
//           const D3DVECTOR4* pClearColor r10, float ClearZ f1, DWORD ClearStencil [r1+0x5C],
//           const D3DRESOLVE_PARAMETERS* pParameters [r1+0x64])
//
// CRASH_TRACE_TILING=<n>[@<frame>]: logs the arguments of the first n calls of each of these
// functions (and of Clear) from frame <frame> on (default 0), with the frame number.

#include "../diagnostics.h"
#include "../kernel/kernel.h"
#include "../kernel/memory.h"
#include "tiling.h"

#include <cstdlib>
#include <cstring>
#include <string>

extern "C" PPC_FUNC(__imp__sub_8226A868);  // create a render target (width r3, height r4, format r5, MSAA r6)
extern "C" PPC_FUNC(__imp__sub_8227A5F8);  // BeginTiling
extern "C" PPC_FUNC(__imp__sub_8227AB80);  // EndTiling
extern "C" PPC_FUNC(__imp__sub_8227F9E8);  // Resolve
extern "C" PPC_FUNC(__imp__sub_82284290);  // Clear
extern "C" PPC_FUNC(__imp__sub_82274080);  // Swap
PPC_FUNC(sub_8227F9E8);                   // our Resolve hook below (C++ linkage, like the generated code)

bool gpu::SinglePassScene()
{
    static const bool on = [] {
        const char* text = getenv("CRASH_SINGLE_PASS");
        bool enabled = !(text && strcmp(text, "0") == 0);
        Log("[tiling] %s", enabled ? "single-pass scene (no predicated tiling)" : "predicated tiling as on the console (CRASH_SINGLE_PASS=0)");
        return enabled;
    }();
    return on;
}

namespace
{
    using gpu::SinglePassScene;
    constexpr uint32_t D3D_TILING = 13088;    // device: non-zero while BeginTiling..EndTiling defers

    uint32_t g_frame;

    struct TraceOptions
    {
        uint32_t budget = 0, fromFrame = 0;
    };

    const TraceOptions& Trace()
    {
        static const TraceOptions options = [] {
            TraceOptions o;
            if (const char* text = getenv("CRASH_TRACE_TILING"))
            {
                o.budget = uint32_t(atoi(text));
                if (const char* at = strchr(text, '@'))
                    o.fromFrame = uint32_t(atoi(at + 1));
            }
            return o;
        }();
        return options;
    }

    bool Tracing(uint32_t& used)
    {
        return Trace().budget && g_frame >= Trace().fromFrame && used++ < Trace().budget;
    }

    std::string Rects(uint32_t address, uint32_t count)
    {
        if (!address)
            return "null";
        std::string text;
        for (uint32_t i = 0; i < count && i < 8; i++)
        {
            char item[64];
            snprintf(item, sizeof(item), "%s(%d,%d)-(%d,%d)", i ? " " : "", int32_t(LoadBE32(address + i * 16)),
                     int32_t(LoadBE32(address + i * 16 + 4)), int32_t(LoadBE32(address + i * 16 + 8)), int32_t(LoadBE32(address + i * 16 + 12)));
            text += item;
        }
        return text;
    }

    std::string Color(uint32_t address)
    {
        if (!address)
            return "null";
        char text[96];
        float c[4];
        for (int i = 0; i < 4; i++)
        {
            uint32_t bits = LoadBE32(address + i * 4);
            memcpy(&c[i], &bits, 4);
        }
        snprintf(text, sizeof(text), "(%g %g %g %g)", c[0], c[1], c[2], c[3]);
        return text;
    }
}

PPC_FUNC(sub_8226A868)
{
    // The scene's tile-sized targets (480x720: color 0, depth/stencil, color 1 = velocity, in this
    // order) become full size (1280x720). Three 1280x720 2x surfaces don't fit D3D's EDRAM allocator
    // (2048 tiles, 1440 each), so they get explicit bases through D3DSURFACE_PARAMETERS (Base,
    // HierarchicalZBase, ColorExpBias), which skips the allocator. Our render targets are host images
    // keyed by base, so the bases only have to differ from each other and from the 1x targets the
    // post-processing and HUD use (color at 0, depth at 720).
    static constexpr uint32_t SCENE_BASES[3] = { 1440, 2048, 2880 };
    static uint32_t s_created;
    uint32_t width = ctx.r3.u32, height = ctx.r4.u32, format = ctx.r5.u32, msaa = ctx.r6.u32, parameters = ctx.r7.u32;
    bool scene = SinglePassScene() && width == 480 && height == 720 && !parameters;
    if (scene)
    {
        static const uint32_t s_parameters = GuestAlloc(16 * 3);
        uint32_t index = s_created++ % 3;
        uint32_t block = s_parameters + index * 16;
        StoreBE32(block + 0, SCENE_BASES[index]);
        StoreBE32(block + 4, 0);
        StoreBE32(block + 8, 0);
        ctx.r3.u64 = 1280;
        ctx.r7.u64 = block;
    }
    __imp__sub_8226A868(ctx, base);
    uint32_t surface = ctx.r3.u32;
    Log("[tiling] surface %ux%u format 0x%08X MSAA %u -> 0x%08X (EDRAM base %u)%s", scene ? 1280u : width, height, format, msaa, surface,
        surface ? LoadBE32(surface + 28) & 0xFFF : 0, scene ? ", single-pass scene target" : "");
}

PPC_FUNC(sub_8227A5F8)
{
    static uint32_t s_used;
    if (Tracing(s_used))
        Log("[tiling] frame %u BeginTiling flags 0x%X count %u tiles %s clear color %s z %g stencil %u%s", g_frame, ctx.r4.u32, ctx.r5.u32,
            Rects(ctx.r6.u32, ctx.r5.u32).c_str(), Color(ctx.r7.u32).c_str(), ctx.f1.f64, ctx.r9.u32, SinglePassScene() ? " (skipped)" : "");
    if (SinglePassScene())
    {
        // Crash passes no clear (flags 0, pClearColor null): nothing else to do.
        if (ctx.r4.u32 || ctx.r7.u32)
            Log("[tiling] BeginTiling with flags 0x%X / clear color: not handled by the single pass", ctx.r4.u32);
        return;
    }
    __imp__sub_8227A5F8(ctx, base);
}

PPC_FUNC(sub_8227AB80)
{
    static uint32_t s_used;
    bool trace = Tracing(s_used);
    if (trace)
        Log("[tiling] frame %u EndTiling resolve flags 0x%X rects %s dest 0x%08X clear color %s z %g stencil %u params 0x%08X tiling %08X", g_frame,
            ctx.r4.u32, Rects(ctx.r5.u32, 3).c_str(), ctx.r6.u32, Color(ctx.r7.u32).c_str(), ctx.f1.f64, ctx.r9.u32, ctx.r10.u32,
            LoadBE32(ctx.r3.u32 + D3D_TILING));
    if (SinglePassScene() && !LoadBE32(ctx.r3.u32 + D3D_TILING))
    {
        // One resolve of the whole surface with EndTiling's flags, clear values and parameters.
        uint32_t clearColor = ctx.r7.u32, stencil = ctx.r9.u32, parameters = ctx.r10.u32;
        ctx.r5.u64 = 0;           // pSourceRect: the whole surface
        ctx.r7.u64 = 0;           // pDestPoint
        ctx.r8.u64 = 0;           // DestLevel
        ctx.r9.u64 = 0;           // DestSliceOrFace
        ctx.r10.u64 = clearColor;
        StoreBE32(ctx.r1.u32 + 0x5C, stencil);
        StoreBE32(ctx.r1.u32 + 0x64, parameters);
        sub_8227F9E8(ctx, base);
        return;
    }
    __imp__sub_8227AB80(ctx, base);
}

PPC_FUNC(sub_8227F9E8)
{
    static uint32_t s_used;
    if (Tracing(s_used))
        Log("[tiling] frame %u Resolve flags 0x%X rect %s dest 0x%08X point 0x%08X level %u r9 %u clear color %s z %g stencil %u params 0x%08X tiling %08X",
            g_frame, ctx.r4.u32, Rects(ctx.r5.u32, 1).c_str(), ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, Color(ctx.r10.u32).c_str(), ctx.f1.f64,
            LoadBE32(ctx.r1.u32 + 0x5C), LoadBE32(ctx.r1.u32 + 0x64), LoadBE32(ctx.r3.u32 + D3D_TILING));
    if (SinglePassScene() && ctx.r5.u32 && ctx.r7.u32)
    {
        // The xn helpers resolve once per tile rect (0-448, 416-864, 832-1280) to the same point:
        // the first becomes the whole surface, the other two have nothing left to do.
        int32_t x0 = int32_t(LoadBE32(ctx.r5.u32)), y0 = int32_t(LoadBE32(ctx.r5.u32 + 4));
        int32_t x1 = int32_t(LoadBE32(ctx.r5.u32 + 8)), y1 = int32_t(LoadBE32(ctx.r5.u32 + 12));
        int32_t px = int32_t(LoadBE32(ctx.r7.u32)), py = int32_t(LoadBE32(ctx.r7.u32 + 4));
        bool tileRect = y0 == 0 && y1 == 720 && x1 - x0 == 448 && (x0 == 0 || x0 == 416 || x0 == 832) && px == x0 && py == 0;
        if (tileRect && x0 != 0)
            return;
        if (tileRect)
        {
            ctx.r5.u64 = 0;
            ctx.r7.u64 = 0;
        }
    }
    __imp__sub_8227F9E8(ctx, base);
}

PPC_FUNC(sub_82284290)
{
    static uint32_t s_used;
    if (Tracing(s_used))
        Log("[tiling] frame %u Clear count %u rects %s flags 0x%X color 0x%08X z %g stencil %u tiling %08X", g_frame, ctx.r4.u32,
            Rects(ctx.r5.u32, ctx.r4.u32).c_str(), ctx.r6.u32, ctx.r7.u32, ctx.f1.f64, ctx.r9.u32, LoadBE32(ctx.r3.u32 + D3D_TILING));
    __imp__sub_82284290(ctx, base);
}

PPC_FUNC(sub_82274080)
{
    g_frame++;
    __imp__sub_82274080(ctx, base);
}
